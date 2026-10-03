#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ArduinoJson.h>

/**
 * Graph payload v1, as published over the data MQTT feed (see docs/CONFIGURATION.md, "Graph"):
 *
 *   {"v":1, "title":"AC 24H", "summary":"12.3kWh", "unit":"kW", "slots":96, "max":6, "stack":true,
 *    "series":[{"label":"UP","color":"#2080FF","data":[0.0,1.25,...]}, ...],
 *    "bands":[{"from":60,"to":72,"color":"#301800"}], "marks":[{"at":28,"color":"#404040"}]}
 *
 * Everything here is plain data with fixed capacity, so a parsed graph can be copied between the
 * cores without touching the heap. The parser and the layout helpers have no Arduino dependency and
 * are covered by the host native tests.
 */
namespace graph {

static constexpr uint8_t MAX_SERIES = 4;
static constexpr uint16_t MAX_POINTS = 288;
static constexpr uint8_t MAX_BANDS = 8;
static constexpr uint8_t MAX_MARKS = 8;

struct Rgb { uint8_t r, g, b; };

struct Series {
    char label[8];
    Rgb color;
    uint16_t count;            ///< points held in `data` (newest last)
    float data[MAX_POINTS];
};

struct Band { uint16_t from, to; Rgb color; };   ///< slot range [from, to)
struct Mark { uint16_t at; Rgb color; };

struct GraphData {
    bool valid;
    char title[24];
    char summary[16];
    char unit[8];
    uint16_t slots;            ///< x positions; data is right-aligned into them
    float max;                 ///< y scale top; <= 0 = auto
    bool stack;
    uint8_t seriesCount;
    uint8_t bandCount;
    uint8_t markCount;
    Series series[MAX_SERIES];
    Band bands[MAX_BANDS];
    Mark marks[MAX_MARKS];
};

/// Parses "#RRGGBB" (or "RRGGBB"); returns false and leaves `out` alone on anything else.
bool parseHexColor(const char* s, Rgb& out);

/**
 * Parses a v1 payload into `out`. `json` must be writable and NUL-terminated (ArduinoJson parses
 * it in place, so no strings are copied into `doc`). `doc` is caller-owned scratch, sized for the
 * largest payload. Returns false - with out.valid = false - when the payload is not a graph (no
 * `series` array) or is not valid JSON.
 */
bool parsePayload(char* json, JsonDocument& doc, GraphData& out);

/// Stacked (or tallest, when not stacking) value at slot `slot`, honouring right alignment.
float valueAt(const GraphData& g, uint8_t series, uint16_t slot);

/// The y scale top: `max` when given, else the largest column (stacked sum or tallest point).
float scaleMax(const GraphData& g);

/// Slot drawn at column `x` of a graph `width` columns wide.
inline uint16_t slotForColumn(int x, int width, uint16_t slots) {
    if (width <= 0 || slots == 0) return 0;
    return (uint16_t)(((uint32_t)x * slots) / (uint32_t)width);
}

/// First column of slot `slot` (the left edge used for marks).
inline int columnForSlot(uint16_t slot, int width, uint16_t slots) {
    if (slots == 0) return 0;
    return (int)(((uint32_t)slot * (uint32_t)width + slots - 1) / slots);
}

/// Bar height in pixels for `value` on a `height`-pixel graph scaled to `top`.
int barHeight(float value, float top, int height);

struct Area { int16_t x, y, w, h; };
struct Layout {
    bool header;
    uint8_t textSize;          ///< built-in 5x7 font scale for the header
    Area title;                ///< header row (title left, summary right)
    Area plot;
    bool gridline;             ///< dim line at half scale (64-px-tall panels)
};

/**
 * 128x32: header rows 0..6, graph rows 8..31. 256x64: header at text size 2, graph rows 15..63
 * with a half-scale gridline. Other sizes scale by height; the header is dropped below 24 px.
 */
Layout calculateLayout(int width, int height, bool showHeader);

}  // namespace graph
