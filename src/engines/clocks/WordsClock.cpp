#include "WordsClock.h"
#include "WordsHourFont.h"
#include "WordsMinuteFont.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

namespace {
const char* const HOURS[13] = { "zero", "one", "two", "three", "four", "five", "six",
                                "seven", "eight", "nine", "ten", "eleven", "twelve" };
const char* const MIN_TENS[6] = { "zero", "ten", "twenty", "thirty", "forty", "fifty" };
const char* const MIN_UNITS[20] = { "zero", "one", "two", "three", "four", "five", "six", "seven",
                                    "eight", "nine", "ten", "eleven", "twelve", "thirteen",
                                    "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
                                    "nineteen" };
const char* const WEEKDAYS[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
const char* const MONTHS[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
constexpr uint16_t ACCENT = 0x2589;   // the muted blue the original uses for the hour and the date
}  // namespace

WordsClockFace::WordsClockFace(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    storedTime = { 0, 0, 0 };
}

void WordsClockFace::draw(const TimeData& t) {
    storedTime = t;
}

// The original's wording, including its special cases.
void WordsClockFace::timeInWords(int h, int m, char* hw, size_t hn, char* mw, size_t mn) const {
    int hour12 = (h % 12);
    if (h == 12) hour12 = 12;

    if (h == 0 && m == 0) {
        snprintf(hw, hn, "midnight");
        mw[0] = '\0';
    } else if (h == 12 && m == 0) {
        snprintf(hw, hn, "noon");
        mw[0] = '\0';
    } else if (m == 0) {
        snprintf(hw, hn, "%s", HOURS[hour12]);
        snprintf(mw, mn, "o'clock");
    } else if (m == 30) {
        snprintf(hw, hn, "%s", HOURS[hour12]);
        snprintf(mw, mn, "%s", (h == 0 || h == 12) ? "thirty" : "a half");
    } else {
        snprintf(hw, hn, "%s", HOURS[hour12]);
        if (m < 10) {
            snprintf(mw, mn, "oh %s", MIN_UNITS[m]);
        } else if (m < 20) {
            snprintf(mw, mn, "%s", MIN_UNITS[m]);
        } else if (m % 10 == 0) {
            snprintf(mw, mn, "%s", MIN_TENS[m / 10]);
        } else {
            snprintf(mw, mn, "%s %s", MIN_TENS[m / 10], MIN_UNITS[m % 10]);
        }
    }
}

void WordsClockFace::drawCentred(const char* text, int centreY, const GFXfont* font, uint16_t color,
                                 int panelW) {
    if (!text || !*text) return;
    matrix->setFont(font);
    matrix->setTextSize(1);
    int16_t bx, by;
    uint16_t bw, bh;
    matrix->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    matrix->setTextColor(color);
    matrix->setCursor((panelW - (int)bw) / 2 - bx, centreY - (int)bh / 2 - by);
    matrix->print(text);
    matrix->setFont(nullptr);
}

void WordsClockFace::update() {
    if (!matrix) return;
    const int w = matrix->width();
    const int h = matrix->height();

    if (lastMinute != storedTime.minutes) {
        lastMinute = storedTime.minutes;
        m_dirty = 2;
    }
    if (m_dirty == 0) { m_hasFrame = false; return; }
    m_dirty--;
    m_hasFrame = true;

    matrix->fillRect(0, 0, w, h, 0x0000);

    char hourWords[24], minuteWords[24];
    timeInWords(storedTime.hours, storedTime.minutes, hourWords, sizeof(hourWords),
                minuteWords, sizeof(minuteWords));

    // Same stack as the original: hour, minutes, a rule, then the date.
    const bool tall = (h >= 48);
    int hourY = tall ? (h * 22 / 100) : (h / 3);
    int minuteY = tall ? (h * 45 / 100) : (h * 2 / 3);
    int ruleY = tall ? (h * 63 / 100) : (h - 8);
    int dateY = tall ? (h * 80 / 100) : (h - 4);

    drawCentred(hourWords, hourY, &hour8pt7b, ACCENT, w);
    drawCentred(minuteWords, minuteY, &minute7pt7b, 0xFFFF, w);

    if (tall) {
        int ruleW = (w * 3) / 4;
        matrix->drawFastHLine((w - ruleW) / 2, ruleY, ruleW, 0xFFFF);

        char dateLine[24] = "";
        time_t nowT = time(nullptr);
        if (nowT > 1600000000) {
            struct tm lt;
            localtime_r(&nowT, &lt);
            // US order: weekday, then month and day, rather than the source face's day-first line.
            snprintf(dateLine, sizeof(dateLine), "%s %s %d", WEEKDAYS[lt.tm_wday % 7],
                     MONTHS[lt.tm_mon % 12], lt.tm_mday);
        }
        drawCentred(dateLine, dateY, &minute7pt7b, ACCENT, w);
    }
}

void WordsClockFace::onDisplayGeometryChanged(const DisplayGeometry& geometry) {
    m_dirty = 2;
    lastMinute = -1;
}
