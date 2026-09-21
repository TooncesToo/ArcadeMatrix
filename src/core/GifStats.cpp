#include "GifStats.h"
#include "SDUtils.h"
#include "SdLockGuard.h"
#include "Logger.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <time.h>
#include <string.h>

namespace {
    constexpr int MAX_FOLDERS = 64;
    constexpr uint32_t FLUSH_INTERVAL_MS = 15UL * 60UL * 1000UL;
    constexpr uint32_t KEEPER_PERIOD_MS = 10000;
    constexpr size_t STAT_LINE_MAX = 256;

    struct Folder {
        char root[12];              // "/gifs" or "/gifs_tate"
        char name[64];
        GifStats::Entry* items = nullptr;   // PSRAM
        uint32_t n = 0, cap = 0;
        uint32_t plays = 0;         // sum of counts
        uint32_t last = 0;          // most recent play, epoch
        bool loaded = false;        // stats.txt merged in
        bool dirty = false;
        uint32_t dirtySinceMs = 0;
    };

    Folder g_folders[MAX_FOLDERS];
    int g_folderCount = 0;
    SemaphoreHandle_t g_mutex = nullptr;      // guards g_folders; held only for microseconds
    volatile bool g_resetRequested = false;
    volatile uint32_t g_flushReqSeq = 0;       // bumped by requestFlush()
    volatile uint32_t g_flushDoneSeq = 0;      // the request the keeper has finished writing
    volatile uint32_t g_lastFlushMs = 0;       // how long the last full write pass took
    uint32_t g_since = 0;
    TaskHandle_t g_task = nullptr;

    uint32_t nowEpoch() {
        time_t t = time(nullptr);
        return (t > 1600000000) ? (uint32_t)t : 0;
    }

    bool grow(Folder& f) {
        uint32_t ncap = f.cap ? f.cap + f.cap / 2 + 16 : 64;
        auto* p = (GifStats::Entry*)heap_caps_realloc(f.items, ncap * sizeof(GifStats::Entry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = (GifStats::Entry*)heap_caps_realloc(f.items, ncap * sizeof(GifStats::Entry), MALLOC_CAP_8BIT);
        if (!p) return false;
        f.items = p; f.cap = ncap;
        return true;
    }

    GifStats::Entry* find(Folder& f, uint32_t h) {
        for (uint32_t i = 0; i < f.n; i++) if (f.items[i].nameHash == h) return &f.items[i];
        return nullptr;
    }

    GifStats::Entry* upsert(Folder& f, uint32_t h) {
        if (auto* e = find(f, h)) return e;
        if (f.n == f.cap && !grow(f)) return nullptr;
        f.items[f.n] = { h, 0, 0 };
        return &f.items[f.n++];
    }

    Folder* folderFor(const char* root, const char* name, bool create) {
        for (int i = 0; i < g_folderCount; i++) {
            if (strcmp(g_folders[i].root, root) == 0 && strcmp(g_folders[i].name, name) == 0) return &g_folders[i];
        }
        if (!create || g_folderCount >= MAX_FOLDERS) return nullptr;
        Folder& f = g_folders[g_folderCount++];
        strlcpy(f.root, root, sizeof(f.root));
        strlcpy(f.name, name, sizeof(f.name));
        return &f;
    }

    // Split "/gifs/Arcade/foo.gif" into root, folder, name. Returns false for anything else.
    bool splitPath(const char* p, char* root, size_t rootN, char* folder, size_t folderN, const char*& name) {
        const char* roots[] = { "/gifs_tate/", "/gifs/" };
        for (const char* r : roots) {
            size_t rl = strlen(r);
            if (strncmp(p, r, rl) != 0) continue;
            const char* rest = p + rl;
            const char* slash = strchr(rest, '/');
            if (!slash || slash == rest || !slash[1] || strchr(slash + 1, '/')) return false;   // exactly folder/name
            strlcpy(root, r, rootN); root[rl - 1] = '\0';                                       // drop trailing '/'
            size_t fl = (size_t)(slash - rest); if (fl >= folderN) return false;
            memcpy(folder, rest, fl); folder[fl] = '\0';
            name = slash + 1;
            return true;
        }
        return false;
    }

    // ---- card I/O, keeper task only ----
    void loadFolder(Folder& f) {
        char path[160]; snprintf(path, sizeof(path), "%s/%s/stats.txt", f.root, f.name);
        SdLockGuard guard(pdMS_TO_TICKS(3000));
        if (!guard) return;                       // try again next round
        if (sd.exists(path)) {
            FsFile file = sd.open(path, FILE_OPEN_READ);
            if (file) {
                char line[STAT_LINE_MAX];
                while (file.available()) {
                    int n = file.readBytesUntil('\n', line, sizeof(line) - 1); line[n] = '\0';
                    char* tab1 = strchr(line, '\t'); if (!tab1) continue;
                    char* tab2 = strchr(tab1 + 1, '\t'); if (!tab2) continue;
                    *tab1 = '\0'; *tab2 = '\0';
                    uint32_t cnt = strtoul(tab1 + 1, nullptr, 10);
                    uint32_t last = strtoul(tab2 + 1, nullptr, 10);
                    if (cnt == 0) continue;
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        if (auto* e = upsert(f, GifStats::hashName(line))) {   // add to anything counted since boot
                            uint32_t sum = e->count + cnt; e->count = (sum > 65535) ? 65535 : (uint16_t)sum;
                            if (last > e->last) e->last = last;
                            f.plays += cnt; if (last > f.last) f.last = last;
                        }
                        xSemaphoreGive(g_mutex);
                    }
                }
                file.close();
            }
        }
        f.loaded = true;
    }

    // Write stats.txt by walking index.txt so names come from the card and deleted files drop out.
    void writeFolder(Folder& f) {
        char idxPath[160], outPath[160], tmpPath[160];
        snprintf(idxPath, sizeof(idxPath), "%s/%s/index.txt", f.root, f.name);
        snprintf(outPath, sizeof(outPath), "%s/%s/stats.txt", f.root, f.name);
        snprintf(tmpPath, sizeof(tmpPath), "%s/%s/stats.tmp", f.root, f.name);
        SdLockGuard guard(pdMS_TO_TICKS(5000));
        if (!guard) return;
        FsFile idx = sd.open(idxPath, FILE_OPEN_READ);
        if (!idx) return;
        FsFile out = sd.open(tmpPath, FILE_OPEN_WRITE);
        if (!out) { idx.close(); return; }
        char line[STAT_LINE_MAX]; uint32_t written = 0;
        while (idx.available()) {
            int n = idx.readBytesUntil('\n', line, sizeof(line) - 1); line[n] = '\0';
            while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
            if (n == 0) continue;
            uint32_t h = GifStats::hashName(line);
            uint16_t cnt = 0; uint32_t last = 0;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (auto* e = find(f, h)) { cnt = e->count; last = e->last; }
                xSemaphoreGive(g_mutex);
            }
            if (cnt == 0) continue;
            out.printf("%s\t%u\t%lu\n", line, (unsigned)cnt, (unsigned long)last);
            written++;
        }
        idx.close(); out.close();
        sd.remove(outPath);
        sd.rename(tmpPath, outPath);
        f.dirty = false;
        LOGD("GifStats", "wrote %s (%lu entries)", outPath, (unsigned long)written);
    }

    void loadMeta() {
        SdLockGuard guard(pdMS_TO_TICKS(3000));
        if (!guard) return;
        if (sd.exists("/gifs/stats_meta.txt")) {
            FsFile f = sd.open("/gifs/stats_meta.txt", FILE_OPEN_READ);
            if (f) { char buf[32]; int n = f.readBytesUntil('\n', buf, sizeof(buf) - 1); buf[n] = '\0'; g_since = strtoul(buf, nullptr, 10); f.close(); }
        }
        if (g_since == 0) {
            g_since = nowEpoch();
            if (g_since) { FsFile f = sd.open("/gifs/stats_meta.txt", FILE_OPEN_WRITE); if (f) { f.printf("%lu\n", (unsigned long)g_since); f.close(); } }
        }
    }

    // Folders present on the card, so the summary covers folders that have not been played since boot.
    void discoverFolders() {
        const char* roots[] = { "/gifs", "/gifs_tate" };
        for (const char* r : roots) {
            SdLockGuard guard(pdMS_TO_TICKS(3000));
            if (!guard) continue;
            FsFile dir = sd.open(r, FILE_OPEN_READ);
            if (!dir || !isDirectory(dir)) continue;
            FsFile e;
            while (getNextFile(dir, e)) {
                if (!isDirectory(e)) continue;
                String nm = getFileName(e); int ls = nm.lastIndexOf('/'); if (ls >= 0) nm = nm.substring(ls + 1);
                if (isMacJunk(nm) || nm.length() >= 64) continue;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) { folderFor(r, nm.c_str(), true); xSemaphoreGive(g_mutex); }
            }
            if (e) e.close();
            dir.close();
        }
    }

    void doReset() {
        for (int i = 0; i < g_folderCount; i++) {
            Folder& f = g_folders[i];
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) { f.n = 0; f.plays = 0; f.last = 0; f.dirty = false; xSemaphoreGive(g_mutex); }
            char path[160]; snprintf(path, sizeof(path), "%s/%s/stats.txt", f.root, f.name);
            SdLockGuard guard(pdMS_TO_TICKS(3000));
            if (guard && sd.exists(path)) sd.remove(path);
        }
        g_since = nowEpoch();
        SdLockGuard guard(pdMS_TO_TICKS(3000));
        if (guard) { FsFile f = sd.open("/gifs/stats_meta.txt", FILE_OPEN_WRITE); if (f) { f.printf("%lu\n", (unsigned long)g_since); f.close(); } }
        LOGI("GifStats", "statistics reset");
    }

    void keeperTask(void*) {
        vTaskDelay(pdMS_TO_TICKS(30000));         // let boot and the first screens settle
        loadMeta();
        discoverFolders();
        for (;;) {
            if (g_resetRequested) { g_resetRequested = false; doReset(); }
            // Claim the pending request before writing, so flushNow() is only told about a pass that saw it.
            uint32_t seq = g_flushReqSeq;
            bool flushAll = (seq != g_flushDoneSeq);
            for (int i = 0; i < g_folderCount; i++) {
                Folder& f = g_folders[i];
                if (!f.loaded) { loadFolder(f); if (!flushAll) vTaskDelay(pdMS_TO_TICKS(200)); }
            }
            uint32_t now = millis();
            uint32_t writeStart = now; uint32_t written = 0;
            for (int i = 0; i < g_folderCount; i++) {
                Folder& f = g_folders[i];
                if (f.dirty && f.loaded && (flushAll || now - f.dirtySinceMs >= FLUSH_INTERVAL_MS)) {
                    written++;
                    writeFolder(f);
                    if (!flushAll) vTaskDelay(pdMS_TO_TICKS(200));   // pace background writes, but not a shutdown flush
                }
            }
            if (written) { g_lastFlushMs = millis() - writeStart; LOGI("GifStats", "wrote %lu folder(s) in %lu ms", (unsigned long)written, (unsigned long)g_lastFlushMs); }
            g_flushDoneSeq = seq;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(KEEPER_PERIOD_MS));   // requestFlush()/flushNow() wake us early
        }
    }
}

namespace GifStats {
    uint32_t hashName(const char* s) {
        uint32_t h = 2166136261u;
        while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
        return h;
    }

    void start() {
        if (g_task) return;
        g_mutex = xSemaphoreCreateMutex();
        if (xTaskCreatePinnedToCore(keeperTask, "gif_stats", 8192, nullptr, 1, &g_task, 0) != pdPASS) {
            LOGW("GifStats", "Could not start the stats keeper task");
            g_task = nullptr;
        }
    }

    void recordPlay(const char* fullPath) {
        if (!g_mutex || !fullPath) return;
        char root[12], folder[64]; const char* name = nullptr;
        if (!splitPath(fullPath, root, sizeof(root), folder, sizeof(folder), name)) return;
        uint32_t h = hashName(name);
        uint32_t t = nowEpoch();
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;   // never stall the render loop
        Folder* f = folderFor(root, folder, true);
        if (f) {
            if (auto* e = upsert(*f, h)) {
                if (e->count < 65535) e->count++;
                e->last = t;
                f->plays++; f->last = t;
                if (!f->dirty) { f->dirty = true; f->dirtySinceMs = millis(); }
            }
        }
        xSemaphoreGive(g_mutex);
    }

    size_t folderEntries(const char* root, const char* folder, Entry* out, size_t max, uint32_t& plays, uint32_t& played) {
        plays = 0; played = 0;
        if (!g_mutex) return 0;
        size_t n = 0;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
        if (Folder* f = folderFor(root, folder, false)) {
            plays = f->plays;
            for (uint32_t i = 0; i < f->n; i++) {
                if (f->items[i].count == 0) continue;
                played++;
                if (n < max) out[n++] = f->items[i];
            }
        }
        xSemaphoreGive(g_mutex);
        return n;
    }

    void forEachFolder(const char* root, void (*fn)(const char*, uint32_t, uint32_t, uint32_t, void*), void* user) {
        if (!g_mutex) return;
        // Copy under the lock, call back outside it.
        struct Row { char name[64]; uint32_t plays, played, last; } rows[MAX_FOLDERS]; int cnt = 0;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
        for (int i = 0; i < g_folderCount && cnt < MAX_FOLDERS; i++) {
            Folder& f = g_folders[i];
            if (strcmp(f.root, root) != 0) continue;
            uint32_t played = 0; for (uint32_t k = 0; k < f.n; k++) if (f.items[k].count) played++;
            strlcpy(rows[cnt].name, f.name, sizeof(rows[cnt].name));
            rows[cnt].plays = f.plays; rows[cnt].played = played; rows[cnt].last = f.last; cnt++;
        }
        xSemaphoreGive(g_mutex);
        for (int i = 0; i < cnt; i++) fn(rows[i].name, rows[i].plays, rows[i].played, rows[i].last, user);
    }

    uint32_t lastFlushMs() { return g_lastFlushMs; }

    uint32_t since() { return g_since; }
    void requestReset() { g_resetRequested = true; }
    void requestFlush() { g_flushReqSeq++; if (g_task) xTaskNotifyGive(g_task); }

    bool flushNow(uint32_t timeoutMs) {
        if (!g_task) return false;
        requestFlush();
        uint32_t want = g_flushReqSeq;
        uint32_t t0 = millis();
        while ((int32_t)(g_flushDoneSeq - want) < 0) {   // wait for a pass that actually saw this request
            if (millis() - t0 >= timeoutMs) return false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return true;
    }
}
