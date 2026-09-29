#include "WorldMapClock.h"
#include "WorldMapAssets.h"
#include "SmallFont.h"
#include "ClockwiseScene.h"
#include <stdio.h>

namespace {
constexpr int MAP_W = 120;
constexpr int MAP_H = 56;
constexpr int TZ_SIZE = 5;          ///< pixels per hour of longitude, as in the original
constexpr int HOME_COLUMN = 32;     ///< the marker's column inside the 64 px scene
constexpr uint16_t MAP_MASK = 0xF81F;
constexpr uint16_t MARKER = 0xF000;
}  // namespace

WorldMapClock::WorldMapClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    storedTime = { 0, 0, 0 };
}

void WorldMapClock::draw(const TimeData& t) {
    storedTime = t;
}

void WorldMapClock::update() {
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

    using namespace ClockwiseScene;
    const int d = divisor(h);
    const int ox = originX(w, h);
    const int oy = originY(h);

    // Column of the map that belongs at the left edge of the scene, from the hour.
    int hour = storedTime.hours % 24;
    int offset = (hour < 12) ? ((12 - hour) * TZ_SIZE) : (MAP_W - (hour % 12) * TZ_SIZE);
    offset = ((offset % MAP_W) + MAP_W) % MAP_W;

    matrix->fillRect(0, 0, w, h, 0x0000);

    // The map wraps, so it can run the full width: the middle 64 px is what the original showed.
    for (int py = 0; py < MAP_H / d; py++) {
        int sy = py * d;
        for (int px = 0; px < w; px++) {
            int sceneX = (px - ox) * d;
            int col = ((offset + sceneX) % MAP_W + MAP_W) % MAP_W;
            uint16_t c = _WORLD_MAP[sy * MAP_W + col];
            if (c == MAP_MASK) continue;
            matrix->drawPixel(px, oy + py, c);
        }
    }

    matrix->drawFastVLine(mapX(HOME_COLUMN, w, h), oy, 64 / d, MARKER);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d", storedTime.hours, storedTime.minutes);
    matrix->setFont(&small4pt7b);
    matrix->setTextSize(1);
    matrix->setTextColor(0xFFFF);
    matrix->setCursor(mapX(1, w, h), mapY(62, h));
    matrix->print(buf);
    matrix->setFont(nullptr);
}

void WorldMapClock::onDisplayGeometryChanged(const DisplayGeometry& geometry) {
    m_dirty = 2;
    lastMinute = -1;
}
