// Diagnostic only (not for upstream): reports where loopTask is parked when it stops running.
// A core 0 task watches a heartbeat bumped by loop(); after 8 s without one it prints the code
// addresses found on loopTask's saved stack (decode with addr2line) and the sdMutex holder.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

extern TaskHandle_t loopTaskHandle;
extern SemaphoreHandle_t sdMutex;
size_t getArduinoLoopTaskStackSize(void);

volatile uint32_t g_stallProbeBeat = 0;

static bool looksLikeCode(uint32_t v, uint32_t& pc) {
    pc = (v & 0x3FFFFFFFu) | 0x40000000u;  // windowed ABI keeps the call size in the top 2 bits
    return (pc >= 0x42000000u && pc < 0x42800000u) || (pc >= 0x40370000u && pc < 0x403E0000u);
}

static void dumpTaskStack(const char* label, TaskHandle_t h, size_t stackBytes) {
    static char line[1024];
    if (!h) return;
    uint32_t* top = *(uint32_t**)h;  // pxTopOfStack is the first TCB member
    uint8_t* start = (uint8_t*)pxTaskGetStackStart(h);
    uint32_t* end = (uint32_t*)(start + stackBytes);
    int n = snprintf(line, sizeof(line), "[StallProbe] %s (%s) state=%d stack:", label, pcTaskGetName(h), (int)eTaskGetState(h));
    if ((uint8_t*)top >= start && top < end) {
        int printed = 0;
        for (uint32_t* p = top; p < end && printed < 80 && n < (int)sizeof(line) - 12; p++) {
            uint32_t pc;
            if (looksLikeCode(*p, pc)) {
                n += snprintf(line + n, sizeof(line) - n, " 0x%08x", (unsigned)pc);
                printed++;
            }
        }
    }
    snprintf(line + n, sizeof(line) - n, "\n");
    Serial.write((const uint8_t*)line, strlen(line));
    Serial.flush();
}

static void dumpLoopTask(uint32_t stalledMs) {
    TaskHandle_t sdHolder = sdMutex ? xSemaphoreGetMutexHolder(sdMutex) : nullptr;
    char head[128];
    snprintf(head, sizeof(head), "[StallProbe] loopTask stalled %u ms, sdMutex holder=%s\n",
             (unsigned)stalledMs, sdHolder ? pcTaskGetName(sdHolder) : "none");
    Serial.write((const uint8_t*)head, strlen(head));
    dumpTaskStack("loop", loopTaskHandle, getArduinoLoopTaskStackSize());
    if (sdHolder && sdHolder != loopTaskHandle) dumpTaskStack("sdHolder", sdHolder, 8192);
}

static void stallProbeTask(void*) {
    uint32_t lastBeat = g_stallProbeBeat;
    uint32_t lastChange = millis();
    uint32_t reported = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint32_t beat = g_stallProbeBeat;
        uint32_t now = millis();
        if (beat != lastBeat) {
            lastBeat = beat;
            lastChange = now;
            reported = 0;
            continue;
        }
        uint32_t stalled = now - lastChange;
        if (stalled >= 8000 && reported == 0) { dumpLoopTask(stalled); reported = 1; }
        if (stalled >= 20000 && reported == 1) { dumpLoopTask(stalled); reported = 2; }
    }
}


// Test only: cut /gifs/Marquee/index.txt back to its first 416 lines, recreating the truncated
// index that produced the watchdog resets, so the GifEngine fix runs against it on hardware.
#include "core/SDUtils.h"
#include "core/SdLockGuard.h"
static void truncateMarqueeIndexForTest() {
    const char* path = "/gifs/Marquee/index.txt";
    const int keepLines = 416;
    SdLockGuard guard(pdMS_TO_TICKS(10000));
    if (!guard || !sd.exists(path)) { Serial.println("[StallProbe] index truncate: skipped"); return; }
    FsFile in = sd.open(path, FILE_OPEN_READ);
    if (!in) return;
    size_t size = in.size();
    char* buf = (char*)ps_malloc(size + 1);
    if (!buf) { in.close(); return; }
    size_t got = in.read((uint8_t*)buf, size); in.close();
    int lines = 0; size_t cut = got;
    for (size_t i = 0; i < got; i++) if (buf[i] == '\n' && ++lines == keepLines) { cut = i + 1; break; }
    if (lines >= keepLines && cut < got) {
        FsFile out = sd.open(path, FILE_OPEN_WRITE);
        if (out) { out.write((const uint8_t*)buf, cut); out.close(); }
        Serial.printf("[StallProbe] index truncate: %u -> %u bytes (%d lines)\n", (unsigned)got, (unsigned)cut, keepLines);
    } else {
        Serial.printf("[StallProbe] index truncate: already %d lines\n", lines);
    }
    free(buf);
}

void startStallProbe() {
    truncateMarqueeIndexForTest();
    xTaskCreatePinnedToCore(stallProbeTask, "stall_probe", 4096, nullptr, 2, nullptr, 0);
}
