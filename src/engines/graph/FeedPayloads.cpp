#include "FeedPayloads.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

namespace feed {

namespace {

struct ConditionMap { const char* ha; const char* icon; int group; };

// Group indices follow conditionGroup()'s documented order.
const ConditionMap kConditions[] = {
    {"clear-night", "01n", 0},  {"sunny", "01d", 1},          {"partlycloudy", "02d", 2},
    {"cloudy", "04d", 3},       {"fog", "50d", 4},            {"exceptional", "50d", 5},
    {"rainy", "10d", 6},        {"pouring", "09d", 7},        {"lightning", "11d", 8},
    {"lightning-rainy", "11d", 9}, {"snowy", "13d", 10},      {"snowy-rainy", "13d", 11},
    {"hail", "13d", 12},        {"windy", "03d", 13},         {"windy-variant", "03d", 13},
};

const ConditionMap* findCondition(const char* c) {
    if (!c) return nullptr;
    for (const auto& m : kConditions) {
        if (strcmp(m.ha, c) == 0) return &m;
    }
    return nullptr;
}

void copyId(char* dst, size_t cap, const char* src) {
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
}

}  // namespace

Kind detectKind(const JsonDocument& doc) {
    if (doc["series"].is<JsonArrayConst>()) return Kind::Graph;
    if (doc["tiles"].is<JsonArrayConst>()) return Kind::Values;
    if (doc["current"].is<JsonObjectConst>() || doc["days"].is<JsonArrayConst>()) return Kind::Weather;
    return Kind::None;
}

bool parseValues(const JsonDocument& doc, ValuesData& out) {
    out.valid = false;
    JsonArrayConst tiles = doc["tiles"].as<JsonArrayConst>();
    if (tiles.isNull()) return false;
    graph::copyPanelText(out.title, sizeof(out.title), doc["title"] | "");
    out.count = 0;
    for (JsonObjectConst t : tiles) {
        if (out.count >= MAX_TILES) break;
        Tile& dst = out.tiles[out.count];
        graph::copyPanelText(dst.label, sizeof(dst.label), t["label"] | "");
        graph::copyPanelText(dst.labelShort, sizeof(dst.labelShort), t["label_short"] | "");
        // Values are preformatted strings, but accept a bare number too.
        char num[16];
        const char* value = t["value"] | "";
        if (t["value"].is<float>() && !t["value"].is<const char*>()) {
            snprintf(num, sizeof(num), "%g", (double)t["value"].as<float>());
            value = num;
        }
        graph::copyPanelText(dst.value, sizeof(dst.value), value);
        graph::copyPanelText(dst.unit, sizeof(dst.unit), t["unit"] | "");
        dst.color = {0xFF, 0xFF, 0xFF};
        graph::parseHexColor(t["color"] | "", dst.color);
        dst.labelColor = {0x80, 0x80, 0x80};
        graph::parseHexColor(t["label_color"] | "", dst.labelColor);
        out.count++;
    }
    out.valid = true;
    return true;
}

bool parseWeather(const JsonDocument& doc, WeatherFeed& out) {
    out.valid = false;
    JsonObjectConst cur = doc["current"].as<JsonObjectConst>();
    JsonArrayConst days = doc["days"].as<JsonArrayConst>();
    if (cur.isNull() && days.isNull()) return false;

    const char* units = doc["units"] | "metric";
    out.imperial = strcmp(units, "imperial") == 0 || strcmp(units, "fahrenheit") == 0 || strcmp(units, "F") == 0;

    out.hasCurrent = !cur.isNull() && cur["temp"].is<float>();
    out.currentTemp = out.hasCurrent ? cur["temp"].as<float>() : NAN;
    copyId(out.currentCondition, sizeof(out.currentCondition), cur.isNull() ? "" : (cur["condition"] | ""));
    out.humidity = (!cur.isNull() && cur["humidity"].is<int>()) ? (int8_t)cur["humidity"].as<int>() : -1;
    out.wind = (!cur.isNull() && cur["wind"].is<float>()) ? cur["wind"].as<float>() : NAN;
    copyId(out.windUnit, sizeof(out.windUnit), cur.isNull() ? "" : (cur["wind_unit"] | ""));
    copyId(out.windDir, sizeof(out.windDir), cur.isNull() ? "" : (cur["wind_dir"] | ""));

    out.dayCount = 0;
    for (JsonObjectConst d : days) {
        if (out.dayCount >= MAX_DAYS) break;
        WeatherDay& dst = out.days[out.dayCount];
        dst.hasTemps = d["temp_max"].is<float>() && d["temp_min"].is<float>();
        dst.tempMax = dst.hasTemps ? d["temp_max"].as<float>() : NAN;
        dst.tempMin = dst.hasTemps ? d["temp_min"].as<float>() : NAN;
        copyId(dst.condition, sizeof(dst.condition), d["condition"] | "");
        dst.precipProb = d["precip_prob"].is<int>() ? (int8_t)d["precip_prob"].as<int>() : -1;
        out.dayCount++;
    }
    out.valid = out.hasCurrent || out.dayCount > 0;
    return out.valid;
}

const char* iconForCondition(const char* haCondition) {
    const ConditionMap* m = findCondition(haCondition);
    return m ? m->icon : "03d";
}

int conditionGroup(const char* haCondition) {
    const ConditionMap* m = findCondition(haCondition);
    return m ? m->group : -1;
}

}  // namespace feed
