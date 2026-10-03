#include "GraphPayload.h"
#include <string.h>
#include <math.h>

namespace graph {

namespace {

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void copyText(char* dst, size_t cap, const char* src) {
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

uint16_t clampIndex(long v, uint16_t limit) {
    if (v < 0) return 0;
    if (v > (long)limit) return limit;
    return (uint16_t)v;
}

}  // namespace

bool parseHexColor(const char* s, Rgb& out) {
    if (!s) return false;
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hexNibble(s[i]);
        if (v[i] < 0) return false;
    }
    out.r = (uint8_t)(v[0] * 16 + v[1]);
    out.g = (uint8_t)(v[2] * 16 + v[3]);
    out.b = (uint8_t)(v[4] * 16 + v[5]);
    return true;
}

bool parsePayload(char* json, JsonDocument& doc, GraphData& out) {
    out.valid = false;
    doc.clear();
    if (!json || deserializeJson(doc, json) != DeserializationError::Ok) return false;
    JsonArrayConst series = doc["series"].as<JsonArrayConst>();
    if (series.isNull()) return false;

    copyText(out.title, sizeof(out.title), doc["title"] | "");
    copyText(out.summary, sizeof(out.summary), doc["summary"] | "");
    copyText(out.unit, sizeof(out.unit), doc["unit"] | "");
    out.max = doc["max"] | 0.0f;
    out.stack = doc["stack"] | true;

    static const Rgb kDefaultColors[MAX_SERIES] = {
        {0x20, 0x80, 0xFF}, {0x00, 0xD0, 0xA0}, {0xFF, 0xA0, 0x20}, {0xE0, 0x40, 0xE0}};

    out.seriesCount = 0;
    uint16_t longest = 0;
    for (JsonObjectConst s : series) {
        if (out.seriesCount >= MAX_SERIES) break;
        Series& dst = out.series[out.seriesCount];
        copyText(dst.label, sizeof(dst.label), s["label"] | "");
        dst.color = kDefaultColors[out.seriesCount];
        parseHexColor(s["color"] | "", dst.color);
        dst.count = 0;
        JsonArrayConst data = s["data"].as<JsonArrayConst>();
        // Keep the newest MAX_POINTS when a series is longer than we can hold.
        size_t total = data.isNull() ? 0 : data.size();
        size_t skip = total > MAX_POINTS ? total - MAX_POINTS : 0;
        size_t i = 0;
        for (JsonVariantConst p : data) {
            if (i++ < skip) continue;
            float v = p.is<float>() ? p.as<float>() : 0.0f;
            if (isnan(v) || isinf(v)) v = 0.0f;
            dst.data[dst.count++] = v;
        }
        if (dst.count > longest) longest = dst.count;
        out.seriesCount++;
    }

    long slots = doc["slots"] | 0L;
    if (slots <= 0) slots = longest;
    if (slots > MAX_POINTS) slots = MAX_POINTS;
    out.slots = (uint16_t)slots;

    out.bandCount = 0;
    for (JsonObjectConst b : doc["bands"].as<JsonArrayConst>()) {
        if (out.bandCount >= MAX_BANDS) break;
        Band& dst = out.bands[out.bandCount];
        dst.from = clampIndex(b["from"] | 0L, out.slots);
        dst.to = clampIndex(b["to"] | 0L, out.slots);
        if (dst.to <= dst.from) continue;
        dst.color = {0x30, 0x18, 0x00};
        parseHexColor(b["color"] | "", dst.color);
        out.bandCount++;
    }

    out.markCount = 0;
    for (JsonObjectConst m : doc["marks"].as<JsonArrayConst>()) {
        if (out.markCount >= MAX_MARKS) break;
        long at = m["at"] | -1L;
        if (at < 0 || at >= (long)out.slots) continue;
        Mark& dst = out.marks[out.markCount];
        dst.at = (uint16_t)at;
        dst.color = {0x40, 0x40, 0x40};
        parseHexColor(m["color"] | "", dst.color);
        out.markCount++;
    }

    out.valid = true;
    return true;
}

float valueAt(const GraphData& g, uint8_t series, uint16_t slot) {
    if (series >= g.seriesCount || slot >= g.slots) return 0.0f;
    const Series& s = g.series[series];
    // Right alignment: the last point sits in the last slot; a shorter series leaves the left empty.
    int idx = (int)s.count - (int)g.slots + (int)slot;
    if (idx < 0 || idx >= (int)s.count) return 0.0f;
    float v = s.data[idx];
    return v > 0.0f ? v : 0.0f;
}

float scaleMax(const GraphData& g) {
    if (g.max > 0.0f) return g.max;
    float top = 0.0f;
    for (uint16_t slot = 0; slot < g.slots; slot++) {
        float col = 0.0f;
        for (uint8_t s = 0; s < g.seriesCount; s++) {
            float v = valueAt(g, s, slot);
            if (g.stack) col += v;
            else if (v > col) col = v;
        }
        if (col > top) top = col;
    }
    return top > 0.001f ? top : 0.001f;
}

int barHeight(float value, float top, int height) {
    if (value <= 0.0f || top <= 0.0f || height <= 0) return 0;
    int h = (int)lroundf(value / top * (float)height);
    if (h > height) h = height;
    return h < 0 ? 0 : h;
}

Layout calculateLayout(int width, int height, bool showHeader) {
    Layout l{};
    l.header = showHeader && height >= 24;
    l.textSize = height >= 64 ? 2 : 1;
    l.gridline = height >= 64;
    if (!l.header) {
        l.title = {0, 0, 0, 0};
        l.plot = {0, 0, (int16_t)width, (int16_t)height};
        return l;
    }
    // The built-in font is 7 px tall per text size; one blank row separates it from the plot, so
    // the plot starts on row 8 at 128x32 and on row 15 at 256x64.
    int headerH = 7 * l.textSize;
    l.title = {0, 0, (int16_t)width, (int16_t)headerH};
    int plotY = headerH + 1;
    l.plot = {0, (int16_t)plotY, (int16_t)width, (int16_t)(height - plotY)};
    return l;
}

}  // namespace graph
