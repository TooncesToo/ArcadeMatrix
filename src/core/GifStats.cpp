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
#include <stdlib.h>

namespace {
    constexpr int MAX_FOLDERS = 64;
    constexpr uint32_t FLUSH_INTERVAL_MS = 15UL * 60UL * 1000UL;
    constexpr uint32_t KEEPER_PERIOD_MS = 10000;
    constexpr uint32_t HEADROOM = 24;          // free slots the keeper keeps ahead of the render path
    constexpr size_t   MAX_FILE_BYTES = 512 * 1024;   // refuse absurd stats.txt files

    struct Folder {
        char root[12];              // "/gifs" or "/gifs_tate"
        char name[64];
        GifStats::Entry* items = nullptr;   // PSRAM
        uint32_t n = 0, cap = 0;
        uint32_t plays = 0;         // sum of counts
        uint32_t last = 0;          // most recent play, epoch
        bool loaded = false;        // stats.txt read in
        bool dirty = false;
        uint32_t dirtySinceMs = 0;
    };

    Folder g_folders[MAX_FOLDERS];
    int g_folderCount = 0;
    SemaphoreHandle_t g_mutex = nullptr;      // guards g_folders; held only for microseconds
    volatile bool g_resetRequested = false;
    volatile uint32_t g_flushReqSeq = 0;      // bumped by requestFlush()
    volatile uint32_t g_flushDoneSeq = 0;     // the request the keeper has finished writing
    volatile uint32_t g_lastFlushMs = 0;      // how long the last write pass took
    volatile uint32_t g_lastFlushFolders = 0;
    volatile uint32_t g_dropped = 0;          // plays lost because no slot was free
    uint32_t g_since = 0;
    TaskHandle_t g_task = nullptr;

    uint32_t nowEpoch() {
        time_t t = time(nullptr);
        return (t > 1600000000) ? (uint32_t)t : 0;
    }

    // ---- entry table (keeper grows it; the render path only fills reserved slots) ----
    bool reserve(Folder& f, uint32_t want) {
        if (f.cap >= want) return true;
        uint32_t ncap = f.cap ? f.cap + f.cap / 2 + HEADROOM : 64;
        if (ncap < want) ncap = want;
        auto* p = (GifStats::Entry*)heap_caps_realloc(f.items, (size_t)ncap * sizeof(GifStats::Entry),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = (GifStats::Entry*)heap_caps_realloc(f.items, (size_t)ncap * sizeof(GifStats::Entry), MALLOC_CAP_8BIT);
        if (!p) return false;
        f.items = p; f.cap = ncap;
        return true;
    }

    GifStats::Entry* find(Folder& f, uint32_t h) {
        for (uint32_t i = 0; i < f.n; i++) if (f.items[i].nameHash == h) return &f.items[i];
        return nullptr;
    }

    Folder* folderFor(const char* root, const char* name, bool create) {
        for (int i = 0; i < g_folderCount; i++)
            if (strcmp(g_folders[i].root, root) == 0 && strcmp(g_folders[i].name, name) == 0) return &g_folders[i];
        if (!create || g_folderCount >= MAX_FOLDERS) return nullptr;
        Folder& f = g_folders[g_folderCount];
        strlcpy(f.root, root, sizeof(f.root));
        strlcpy(f.name, name, sizeof(f.name));
        return &g_folders[g_folderCount++];
    }

    // "/gifs/Arcade/foo.gif" -> root "/gifs", folder "Arcade", name "foo.gif"
    bool splitPath(const char* p, char* root, size_t rootN, char* folder, size_t folderN, const char*& name) {
        if (!p || *p != '/') return false;
        const char* s1 = strchr(p + 1, '/');   if (!s1) return false;
        const char* s2 = strchr(s1 + 1, '/');  if (!s2) return false;
        size_t rl = (size_t)(s1 - p), fl = (size_t)(s2 - s1 - 1);
        if (rl == 0 || rl >= rootN || fl == 0 || fl >= folderN) return false;
        memcpy(root, p, rl); root[rl] = '\0';
        memcpy(folder, s1 + 1, fl); folder[fl] = '\0';
        name = s2 + 1;
        return *name != '\0';
    }

    // ---- card I/O, keeper task only: one short hold per file, parsing happens outside it ----
    char* readWholeFile(const char* path, size_t& lenOut) {
        lenOut = 0;
        SdLockGuard guard(pdMS_TO_TICKS(8000));
        if (!guard) return nullptr;                       // busy: the caller retries next round
        if (!sd.exists(path)) { lenOut = 0; return (char*)1; }   // "nothing there" is a success
        FsFile f = sd.open(path, FILE_OPEN_READ);
        if (!f) return nullptr;
        size_t sz = f.size();
        if (sz == 0 || sz > MAX_FILE_BYTES) { f.close(); lenOut = 0; return (char*)1; }
        char* buf = (char*)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char*)malloc(sz + 1);
        if (!buf) { f.close(); return nullptr; }
        size_t n = f.read((uint8_t*)buf, sz);
        f.close();
        buf[n] = '\0'; lenOut = n;
        return buf;
    }

    void loadFolder(Folder& f) {
        char path[160]; snprintf(path, sizeof(path), "%s/%s/stats.txt", f.root, f.name);
        size_t len = 0;
        char* buf = readWholeFile(path, len);
        if (!buf) return;                                 // card busy, try again next round
        if (buf == (char*)1) { f.loaded = true; return; } // no file yet
        char* p = buf;
        while (p && *p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            char* tab1 = strchr(p, '\t');
            char* tab2 = tab1 ? strchr(tab1 + 1, '\t') : nullptr;
            if (tab1 && tab2) {
                *tab1 = '\0'; *tab2 = '\0';
                uint32_t cnt = strtoul(tab1 + 1, nullptr, 10);
                uint32_t last = strtoul(tab2 + 1, nullptr, 10);
                size_t nameLen = strlen(p);
                while (nameLen && (p[nameLen - 1] == '\r' || p[nameLen - 1] == ' ')) p[--nameLen] = '\0';
                if (cnt && nameLen && nameLen < GifStats::GIFSTATS_NAME_CAP) {
                    uint32_t h = GifStats::hashName(p);
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        GifStats::Entry* e = find(f, h);
                        if (!e && reserve(f, f.n + 1)) {
                            e = &f.items[f.n++];
                            e->nameHash = h; e->count = 0; e->last = 0;
                            strlcpy(e->name, p, GifStats::GIFSTATS_NAME_CAP);
                        }
                        if (e) {                          // add to anything already counted since boot
                            uint32_t sum = e->count + cnt;
                            e->count = (sum > 65535) ? 65535 : (uint16_t)sum;
                            if (last > e->last) e->last = last;
                            f.plays += cnt;
                            if (last > f.last) f.last = last;
                        }
                        xSemaphoreGive(g_mutex);
                    }
                }
            }
            p = nl ? nl + 1 : nullptr;
        }
        free(buf);
        f.loaded = true;
    }

    // Build the file in memory, then write it in one short hold.
    void writeFolder(Folder& f) {
        size_t cap = 0;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
        cap = (size_t)f.n * (GifStats::GIFSTATS_NAME_CAP + 24) + 64;
        xSemaphoreGive(g_mutex);
        char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char*)malloc(cap);
        if (!buf) return;

        size_t used = 0; uint32_t lines = 0;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (uint32_t i = 0; i < f.n && used + GifStats::GIFSTATS_NAME_CAP + 24 < cap; i++) {
                const GifStats::Entry& e = f.items[i];
                if (e.count == 0 || e.name[0] == '\0') continue;
                int n = snprintf(buf + used, cap - used, "%s\t%u\t%lu\n", e.name, (unsigned)e.count, (unsigned long)e.last);
                if (n > 0) { used += (size_t)n; lines++; }
            }
            f.dirty = false;            // plays during the write mark it dirty again, as they should
            xSemaphoreGive(g_mutex);
        }

        char outPath[160], tmpPath[160];
        snprintf(outPath, sizeof(outPath), "%s/%s/stats.txt", f.root, f.name);
        snprintf(tmpPath, sizeof(tmpPath), "%s/%s/stats.tmp", f.root, f.name);
        bool ok = false;
        {
            SdLockGuard guard(pdMS_TO_TICKS(8000));
            if (guard) {
                FsFile out = sd.open(tmpPath, FILE_OPEN_WRITE);
                if (out) {
                    ok = (used == 0) || (out.write((const uint8_t*)buf, used) == (int)used);
                    out.close();
                    if (ok) { sd.remove(outPath); ok = sd.rename(tmpPath, outPath); }
                    if (!ok) sd.remove(tmpPath);
                }
            }
        }
        free(buf);
        if (!ok) {                       // keep it dirty so the next pass tries again
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                if (!f.dirty) { f.dirty = true; f.dirtySinceMs = millis(); }
                xSemaphoreGive(g_mutex);
            }
            LOGW("GifStats", "could not write %s", outPath);
        } else {
            LOGD("GifStats", "wrote %s (%lu entries)", outPath, (unsigned long)lines);
        }
    }

    void loadMeta() {
        size_t len = 0;
        char* buf = readWholeFile("/gifs/stats_meta.txt", len);
        if (buf && buf != (char*)1) { g_since = strtoul(buf, nullptr, 10); free(buf); }
        if (g_since == 0) {
            g_since = nowEpoch();
            if (g_since) {
                SdLockGuard guard(pdMS_TO_TICKS(8000));
                if (guard) { FsFile f = sd.open("/gifs/stats_meta.txt", FILE_OPEN_WRITE); if (f) { f.printf("%lu\n", (unsigned long)g_since); f.close(); } }
            }
        }
    }

    // Folders present on the card, so the summary covers folders that have not been played since boot.
    void discoverFolders() {
        const char* roots[] = { "/gifs", "/gifs_tate" };
        for (const char* r : roots) {
            SdLockGuard guard(pdMS_TO_TICKS(8000));
            if (!guard) continue;
            FsFile dir = sd.open(r, FILE_OPEN_READ);
            if (!dir || !isDirectory(dir)) continue;
            FsFile e;
            while (getNextFile(dir, e)) {
                if (!isDirectory(e)) continue;
                String nm = getFileName(e); int ls = nm.lastIndexOf('/'); if (ls >= 0) nm = nm.substring(ls + 1);
                if (isMacJunk(nm) || nm.length() >= 64) continue;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) { folderFor(r, nm.c_str(), true); xSemaphoreGive(g_mutex); }
            }
            if (e) e.close();
            dir.close();
        }
    }

    void doReset() {
        for (int i = 0; i < g_folderCount; i++) {
            Folder& f = g_folders[i];
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) { f.n = 0; f.plays = 0; f.last = 0; f.dirty = false; xSemaphoreGive(g_mutex); }
            char path[160]; snprintf(path, sizeof(path), "%s/%s/stats.txt", f.root, f.name);
            SdLockGuard guard(pdMS_TO_TICKS(8000));
            if (guard && sd.exists(path)) sd.remove(path);
        }
        g_since = nowEpoch();
        g_dropped = 0;
        SdLockGuard guard(pdMS_TO_TICKS(8000));
        if (guard) { FsFile f = sd.open("/gifs/stats_meta.txt", FILE_OPEN_WRITE); if (f) { f.printf("%lu\n", (unsigned long)g_since); f.close(); } }
        LOGI("GifStats", "statistics reset");
    }

    void keeperTask(void*) {
        vTaskDelay(pdMS_TO_TICKS(20000));         // let boot and the first screens settle
        loadMeta();
        discoverFolders();
        for (;;) {
            if (g_resetRequested) { g_resetRequested = false; doReset(); }
            // Claim the pending request before writing, so flushNow() is only told about a pass that saw it.
            uint32_t seq = g_flushReqSeq;
            bool flushAll = (seq != g_flushDoneSeq);

            for (int i = 0; i < g_folderCount; i++) {
                Folder& f = g_folders[i];
                if (!f.loaded) { loadFolder(f); if (!flushAll) vTaskDelay(pdMS_TO_TICKS(100)); }
                // Keep slots free so recordPlay() on the render task never has to allocate.
                if (f.loaded && f.cap - f.n < HEADROOM) {
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) { reserve(f, f.n + HEADROOM); xSemaphoreGive(g_mutex); }
                }
            }

            uint32_t now = millis();
            uint32_t writeStart = now, written = 0;
            for (int i = 0; i < g_folderCount; i++) {
                Folder& f = g_folders[i];
                if (f.dirty && f.loaded && (flushAll || now - f.dirtySinceMs >= FLUSH_INTERVAL_MS)) {
                    written++;
                    writeFolder(f);
                    if (!flushAll) vTaskDelay(pdMS_TO_TICKS(100));   // pace background writes, not a shutdown flush
                }
            }
            if (written) {
                g_lastFlushMs = millis() - writeStart;
                g_lastFlushFolders = written;
                LOGI("GifStats", "wrote %lu folder(s) in %lu ms", (unsigned long)written, (unsigned long)g_lastFlushMs);
            }
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
        if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
        if (!g_mutex) { LOGE("GifStats", "no mutex; statistics disabled"); return; }
        if (xTaskCreatePinnedToCore(keeperTask, "gif_stats", 6144, nullptr, 1, &g_task, 0) != pdPASS) {
            g_task = nullptr;
            LOGE("GifStats", "keeper task did not start; statistics disabled");
        }
    }

    void recordPlay(const char* fullPath) {
        if (!g_mutex || !fullPath) return;
        char root[12], folder[64]; const char* name = nullptr;
        if (!splitPath(fullPath, root, sizeof(root), folder, sizeof(folder), name)) return;
        uint32_t h = hashName(name);
        uint32_t t = nowEpoch();
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) != pdTRUE) { g_dropped++; return; }   // never stall the render loop
        Folder* f = folderFor(root, folder, true);
        if (f) {
            Entry* e = find(*f, h);
            if (!e) {
                if (f->n < f->cap) {                 // slot reserved by the keeper: no allocation here
                    e = &f->items[f->n++];
                    e->nameHash = h; e->count = 0; e->last = 0;
                    strlcpy(e->name, name, GIFSTATS_NAME_CAP);
                    if (strlen(name) >= GIFSTATS_NAME_CAP) e->name[0] = '\0';   // too long to persist; counted only
                } else {
                    g_dropped++;                     // keeper will grow the table before the next play
                }
            }
            if (e) {
                if (e->count < 65535) e->count++;
                e->last = t;
                f->plays++; f->last = t;
                if (!f->dirty) { f->dirty = true; f->dirtySinceMs = millis(); }
            }
        }
        xSemaphoreGive(g_mutex);
        if (g_task) xTaskNotifyGive(g_task);   // wake the keeper so it can top up free slots
    }

    size_t folderEntries(const char* root, const char* folder, Entry* out, size_t max, uint32_t& plays, uint32_t& played) {
        plays = 0; played = 0;
        if (!g_mutex) return 0;
        size_t n = 0;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
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
        if (!g_mutex || !fn) return;
        for (int i = 0; i < g_folderCount; i++) {
            char name[64]; uint32_t plays = 0, played = 0, last = 0;
            bool match = false;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                Folder& f = g_folders[i];
                if (strcmp(f.root, root) == 0) {
                    match = true;
                    strlcpy(name, f.name, sizeof(name));
                    plays = f.plays; last = f.last;
                    for (uint32_t k = 0; k < f.n; k++) if (f.items[k].count) played++;
                }
                xSemaphoreGive(g_mutex);
            }
            if (match) fn(name, plays, played, last, user);   // called outside the lock
        }
    }

    uint32_t since() { return g_since; }
    uint32_t lastFlushMs() { return g_lastFlushMs; }
    uint32_t lastFlushFolders() { return g_lastFlushFolders; }
    uint32_t dropped() { return g_dropped; }
    void requestReset() { g_resetRequested = true; if (g_task) xTaskNotifyGive(g_task); }
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
