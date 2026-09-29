#include "PacmanMazeClock.h"
#include "cwpacman/Clockface.h"
#include "cwpacman/cw_shim.h"
#include <esp_heap_caps.h>
#include <time.h>

namespace {
constexpr int SCENE = 64;
CWDateTime g_dateTime;   // the face holds a pointer to this; one face runs at a time
}  // namespace

PacmanMazeClock::PacmanMazeClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    storedTime = { 0, 0, 0 };
    m_canvas = new GFXcanvas16(SCENE, SCENE);
    if (m_canvas && m_canvas->getBuffer()) {
        m_shadow = (uint16_t*)heap_caps_malloc(SCENE * SCENE * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m_shadow) m_shadow = (uint16_t*)malloc(SCENE * SCENE * sizeof(uint16_t));
        Locator::provide(m_canvas);
        m_face = new CwMazeFace(m_canvas);
    }
}

PacmanMazeClock::~PacmanMazeClock() {
    delete m_face;
    m_face = nullptr;
    if (m_shadow) { heap_caps_free(m_shadow); m_shadow = nullptr; }
    delete m_canvas;
    m_canvas = nullptr;
    Locator::provide(nullptr);
}

void PacmanMazeClock::draw(const TimeData& t) {
    storedTime = t;
}

// Push the canvas to the panel: the scene in the middle, its edge columns carried to the sides, and
// only the pixels that differ from what is already on the panel.
void PacmanMazeClock::present(int panelW, int panelH) {
    if (!m_canvas || !matrix) return;
    const uint16_t* src = m_canvas->getBuffer();
    const int ox = (panelW - SCENE) / 2;
    const int oy = (panelH - SCENE) / 2;

    for (int y = 0; y < SCENE; y++) {
        const uint16_t* row = src + y * SCENE;
        uint16_t* shadowRow = m_shadow ? (m_shadow + y * SCENE) : nullptr;
        int py = oy + y;
        if (py < 0 || py >= panelH) continue;
        for (int x = 0; x < SCENE; x++) {
            uint16_t c = row[x];
            if (!m_fullBlit && shadowRow && shadowRow[x] == c) continue;
            if (shadowRow) shadowRow[x] = c;
            int px = ox + x;
            if (px >= 0 && px < panelW) matrix->drawPixel(px, py, c);
            // The sides repeat the scene's outermost columns.
            if (x == 0) {
                for (int e = 0; e < ox; e++) matrix->drawPixel(e, py, c);
            } else if (x == SCENE - 1) {
                for (int e = ox + SCENE; e < panelW; e++) matrix->drawPixel(e, py, c);
            }
        }
    }
    m_fullBlit = false;
}

void PacmanMazeClock::update() {
    if (!matrix || !m_face || !m_canvas) return;
    const int w = matrix->width();
    const int h = matrix->height();
    if (h < SCENE) { m_hasFrame = false; return; }   // the maze needs its full 64 rows

    // Feed the face the time, including the date parts it puts under the maze.
    struct tm lt;
    time_t nowT = time(nullptr);
    if (nowT > 1600000000) {
        localtime_r(&nowT, &lt);
        g_dateTime.set(storedTime.hours, storedTime.minutes, storedTime.seconds,
                       (uint8_t)lt.tm_mday, (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_wday);
    } else {
        g_dateTime.set(storedTime.hours, storedTime.minutes, storedTime.seconds, 1, 1, 0);
    }

    Locator::provide(m_canvas);
    if (!m_started) {
        m_face->setup(&g_dateTime);
        m_started = true;
        m_fullBlit = true;
    }
    m_face->update();

    m_hasFrame = true;
    present(w, h);
}

void PacmanMazeClock::onDisplayGeometryChanged(const DisplayGeometry& geometry) {
    m_fullBlit = true;
}
