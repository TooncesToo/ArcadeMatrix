#include "WeatherPageRenderer.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include "../renderers/WeatherIcon.h"
#include "../../../include/core/I18n.h"

namespace weather_page {

namespace {

constexpr char kDegree = (char)0xF7;   // Adafruit GFX draws the CP437 degree glyph for 0xF7 with CP437 mode off

/// Everything one page shows, as plain text.
struct PageText {
    bool isNow;
    const char* icon;
    char label[24], labelLong[24];
    char desc[32], descLong[32];
    char top[32], bottom[40];           ///< the two temperature rows (NOW: live temp / humidity+wind)
    char bottomNoDir[40];               ///< NOW row 2 without the wind direction, when it does not fit
};

int textW(const char* t, int size) {
    size_t n = strlen(t);
    return n ? (int)n * 6 * size - size : 0;
}

void conditionText(const char* cond, Lang lang, char* shortOut, size_t shortCap, char* longOut, size_t longCap) {
    const int group = feed::conditionGroup(cond);
    if (group < 0) {   // unknown condition: show it as received
        strlcpy(shortOut, cond, shortCap);
        strlcpy(longOut, cond, longCap);
        return;
    }
    strlcpy(shortOut, I18n::getHaConditionLabel(group, false, lang), shortCap);
    strlcpy(longOut, I18n::getHaConditionLabel(group, true, lang), longCap);
}

void buildPage(const feed::WeatherFeed& wx, uint8_t page, PageText& p) {
    memset(&p, 0, sizeof(p));
    const Lang lang = I18n::getLang();
    const char unit = wx.imperial ? 'F' : 'C';
    const bool hasNow = wx.hasCurrent;
    p.isNow = hasNow && page == 0;

    if (p.isNow) {
        const char* cond = wx.currentCondition[0] ? wx.currentCondition : (wx.dayCount ? wx.days[0].condition : "");
        p.icon = feed::iconForCondition(cond);
        strlcpy(p.label, I18n::getNowLabel(lang), sizeof(p.label));
        strlcpy(p.labelLong, p.label, sizeof(p.labelLong));
        snprintf(p.top, sizeof(p.top), "%ld%c%c", lroundf(wx.currentTemp), kDegree, unit);
        // "<humidity>%  <dir> <speed><unit>": two spaces before the wind group; the direction never
        // stands without a speed.
        char speed[16] = {0}, wind[24] = {0};
        if (!isnan(wx.wind)) {
            snprintf(speed, sizeof(speed), "%ld%s", lroundf(wx.wind), wx.windUnit);
            if (wx.windDir[0]) snprintf(wind, sizeof(wind), "%s %s", wx.windDir, speed);
        }
        auto build = [&](const char* w, char* out, size_t cap) {
            if (wx.humidity >= 0 && w[0]) snprintf(out, cap, "%d%%  %s", wx.humidity, w);
            else if (wx.humidity >= 0) snprintf(out, cap, "%d%%", wx.humidity);
            else strlcpy(out, w, cap);
        };
        build(wind[0] ? wind : speed, p.bottom, sizeof(p.bottom));
        build(speed, p.bottomNoDir, sizeof(p.bottomNoDir));
        if (!p.bottom[0]) {   // neither humidity nor wind: the short condition instead
            char longUnused[32];
            conditionText(cond, lang, p.bottom, sizeof(p.bottom), longUnused, sizeof(longUnused));
            strlcpy(p.bottomNoDir, p.bottom, sizeof(p.bottomNoDir));
        }
        return;
    }

    const uint8_t day = (uint8_t)(page - (hasNow ? 1 : 0));
    const feed::WeatherDay* d = day < wx.dayCount ? &wx.days[day] : nullptr;
    const char* cond = (d && d->condition[0]) ? d->condition : (day == 0 ? wx.currentCondition : "");
    p.icon = feed::iconForCondition(cond);
    conditionText(cond, lang, p.desc, sizeof(p.desc), p.descLong, sizeof(p.descLong));
    struct tm t;
    int wday = 0;
    if (getLocalTime(&t, 0)) wday = t.tm_wday;
    wday = (wday + day) % 7;
    strlcpy(p.label, I18n::getWeatherDayLabel(wday, day == 0, day == 1), sizeof(p.label));
    strlcpy(p.labelLong, I18n::getWeatherDayLabelLong(wday, day == 0, day == 1, lang), sizeof(p.labelLong));
    const float lo = (d && d->hasTemps) ? d->tempMin : wx.currentTemp;
    const float hi = (d && d->hasTemps) ? d->tempMax : wx.currentTemp;
    // °F: high on top (US convention); °C: low on top (upstream order).
    const float first = wx.imperial ? hi : lo;
    const float second = wx.imperial ? lo : hi;
    snprintf(p.top, sizeof(p.top), "%ld%c%c", lroundf(first), kDegree, unit);
    snprintf(p.bottom, sizeof(p.bottom), "%ld%c%c", lroundf(second), kDegree, unit);
}

/// Long text when it fits `w`, else the short one, else the short one cut ("Partl." style).
void fitText(const char* longText, const char* shortText, int w, int size, char* out, size_t cap) {
    if (longText[0] && textW(longText, size) <= w) { strlcpy(out, longText, cap); return; }
    strlcpy(out, shortText[0] ? shortText : longText, cap);
    const int maxChars = (w + size) / (6 * size) > 1 ? (w + size) / (6 * size) : 1;
    if ((int)strlen(out) > maxChars) {
        if (maxChars > 3 && (size_t)maxChars < cap) { out[maxChars - 1] = '.'; out[maxChars] = '\0'; }
        else if ((size_t)maxChars < cap) out[maxChars] = '\0';
    }
}

void printShadowed(MatrixPanel_I2S_DMA* m, int x, int y, const char* t, uint16_t c) {
    m->setTextColor(0);
    m->setCursor(x + 1, y + 1);
    m->print(t);
    m->setTextColor(c);
    m->setCursor(x, y);
    m->print(t);
}

}  // namespace

void draw(MatrixPanel_I2S_DMA* m, const feed::WeatherFeed& wx, uint8_t page) {
    if (!m) return;
    PageText p;
    buildPage(wx, page, p);
    m->setFont(nullptr);
    m->cp437(false);   // the degree byte relies on it (another engine may have left it on)
    m->setTextWrap(false);

    const int mw = m->width(), mh = m->height();
    const uint16_t white = m->color565(255, 255, 255);
    const uint16_t cyan = m->color565(120, 200, 255);
    const uint16_t orange = m->color565(255, 150, 50);
    const uint16_t colorLabel = m->color565(180, 180, 255);
    const uint16_t colorDesc = m->color565(210, 210, 210);
    // Row colours: NOW = white live reading over grey humidity/wind; days = high orange / low cyan,
    // whichever row each one is on.
    const uint16_t topColor = p.isNow ? white : (wx.imperial ? orange : cyan);
    const uint16_t bottomColor = p.isNow ? colorDesc : (wx.imperial ? cyan : orange);

    const bool wide = mw >= 256 && mh >= 64;
    const bool mid = !wide && mw >= 128 && mh <= 32;
    if (wide || mid) {
        // icon | day label + condition | temperatures right-aligned (layout-v2 §B).
        const int s = wide ? 2 : 1;
        const int margin = wide ? 8 : 4;
        const int iconX = margin;
        WeatherIcon::draw(m, p.icon, iconX, (mh - 24 * s) / 2, s);
        const int row1 = wide ? 10 : 4;
        const int row2 = wide ? 38 : 18;
        const int rightEdge = mw - margin;
        const int midX = iconX + 24 * s + margin;

        // NOW row 2 must start at or after the label column; if not, drop the wind direction.
        const char* bottom = p.bottom;
        if (p.isNow && rightEdge - textW(p.bottom, s) < midX) bottom = p.bottomNoDir;

        m->setTextSize(s);
        printShadowed(m, rightEdge - textW(p.top, s), row1, p.top, topColor);
        printShadowed(m, rightEdge - textW(bottom, s), row2, bottom, bottomColor);

        // Middle column: the widest temperature sets its width (on NOW only row 1 does).
        const int tempW = p.isNow ? textW(p.top, s)
                                  : (textW(p.top, s) > textW(bottom, s) ? textW(p.top, s) : textW(bottom, s));
        const int midW = rightEdge - tempW - margin - midX;
        char text[40];
        if (wide) {
            // Unabbreviated label at size 2 when it fits, else the short one.
            strlcpy(text, (p.labelLong[0] && textW(p.labelLong, 2) <= midW) ? p.labelLong : p.label, sizeof(text));
            m->setTextSize(2);
            m->setTextColor(colorLabel);
            m->setCursor(midX, row1);
            m->print(text);
            if (!p.isNow && (p.descLong[0] || p.desc[0])) {
                const char* desc = p.descLong[0] ? p.descLong : p.desc;
                int size = textW(desc, 2) <= midW ? 2 : 1;
                if (size == 1 && textW(desc, 1) > midW) desc = p.desc;   // last resort: short form
                m->setTextSize(size);
                m->setTextColor(colorDesc);
                m->setCursor(midX, size == 2 ? row2 : row2 + 4);
                m->print(desc);
            }
        } else {
            fitText(p.labelLong, p.label, midW, 1, text, sizeof(text));
            m->setTextColor(colorLabel);
            m->setCursor(midX, row1);
            m->print(text);
            if (!p.isNow && (p.descLong[0] || p.desc[0])) {
                fitText(p.descLong, p.desc, midW, 1, text, sizeof(text));
                m->setTextColor(colorDesc);
                m->setCursor(midX, row2);
                m->print(text);
            }
        }
        return;
    }

    m->setTextSize(1);
    if (mw <= 64 && mh <= 32) {
        // 64x32: label, both temperatures on one line, condition.
        const int iconX = 2;
        WeatherIcon::draw(m, p.icon, iconX, (mh - 24) / 2, 1);
        const int rightX = iconX + 26;
        const int maxChars = (mw - rightX - 1) / 6 > 1 ? (mw - rightX - 1) / 6 : 1;
        m->setTextColor(colorLabel);
        m->setCursor(rightX, 2);
        m->print(p.label);
        m->setTextColor(topColor);
        m->setCursor(rightX, 11);
        m->print(p.top);
        m->setTextColor(bottomColor);
        m->setCursor(rightX + (int)strlen(p.top) * 6 + 2, 11);
        m->print(p.isNow ? "" : p.bottom);
        const char* line3 = p.isNow ? p.bottomNoDir : p.desc;
        char text[40];
        strlcpy(text, line3, sizeof(text));
        if ((int)strlen(text) > maxChars) {
            if (maxChars > 3) { text[maxChars - 1] = '.'; text[maxChars] = '\0'; }
            else text[maxChars] = '\0';
        }
        m->setTextColor(colorDesc);
        m->setCursor(rightX, 21);
        m->print(text);
        return;
    }

    // Square / vertical panels: everything centred in a column.
    WeatherIcon::draw(m, p.icon, (mw - 24) / 2, 14, 1);
    m->setTextColor(colorLabel);
    m->setCursor((mw - (int)strlen(p.label) * 6) / 2, 3);
    m->print(p.label);
    const char* line = p.isNow ? p.bottomNoDir : p.desc;
    m->setTextColor(colorDesc);
    m->setCursor((mw - (int)strlen(line) * 6) / 2, 40);
    m->print(line);
    m->setTextColor(topColor);
    m->setCursor((mw - (int)strlen(p.top) * 6) / 2, 49);
    m->print(p.top);
    if (!p.isNow) {
        m->setTextColor(bottomColor);
        m->setCursor((mw - (int)strlen(p.bottom) * 6) / 2, 57);
        m->print(p.bottom);
    }
}

}  // namespace weather_page
