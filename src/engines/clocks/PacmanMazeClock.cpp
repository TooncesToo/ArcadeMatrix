#include "PacmanMazeClock.h"
#include "cwpacman/Clockface.h"
#include "cwpacman/cw_shim.h"
#include <esp_heap_caps.h>
#include <time.h>

namespace {
constexpr int SCENE = 64;
PacmanMazeClock::Debug g_debug{ false, 0, 0, 0, 0 };
CWDateTime g_dateTime;   // the face holds a pointer to this; one face runs at a time
}  // namespace

PacmanMazeClock::PacmanMazeClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    storedTime = { 0, 0, 0 };
    // clock_size doubles as the zoom: 1 shows the whole maze, 4 fills a 256 px panel with a window
    // on it that follows Pac-Man.
    int zoom = config ? config->getInt("clock_size", 1) : 1;
    m_zoom = constrain(zoom, 1, 4);
    m_canvas = new GFXcanvas16(SCENE, SCENE);
    if (m_canvas && m_canvas->getBuffer()) {
        m_shadow = (uint16_t*)heap_caps_malloc(SCENE * SCENE * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m_shadow) m_shadow = (uint16_t*)malloc(SCENE * SCENE * sizeof(uint16_t));
        Locator::provide(m_canvas);
        m_face = new CwMazeFace(m_canvas);
    }
}

PacmanMazeClock::Debug PacmanMazeClock::debug() { return g_debug; }

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

    // The maze is a framed picture, not a landscape: carrying its outer wall out to the edges
    // floods the panel with blue, so the sides are simply black.
    if (m_fullBlit) {
        matrix->fillRect(0, 0, ox, panelH, 0x0000);
        matrix->fillRect(ox + SCENE, 0, panelW - (ox + SCENE), panelH, 0x0000);
        if (oy > 0) {
            matrix->fillRect(ox, 0, SCENE, oy, 0x0000);
            matrix->fillRect(ox, oy + SCENE, SCENE, panelH - (oy + SCENE), 0x0000);
        }
    }

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
        }
    }
    m_fullBlit = false;
}

// Magnified view: each scene pixel becomes a zoom x zoom block, and the window follows Pac-Man,
// stepping only when he nears an edge so the panel is not repainted on every frame.
void PacmanMazeClock::presentZoomed(int panelW, int panelH) {
    if (!m_canvas || !matrix || !m_face) return;
    const uint16_t* src = m_canvas->getBuffer();
    const int z = m_zoom;
    const int viewW = (panelW + z - 1) / z;      // scene pixels visible across
    const int viewH = (panelH + z - 1) / z;

    // Keep Pac-Man inside a margin; when he leaves it the window steps to re-centre him.
    const int marginX = viewW / 4, marginY = viewH / 4;
    int px = m_face->pacmanX(), py = m_face->pacmanY();
    int wantX = m_camX, wantY = m_camY;
    if (px - m_camX < marginX) wantX = px - marginX;
    if (px - m_camX > viewW - marginX) wantX = px - (viewW - marginX);
    if (py - m_camY < marginY) wantY = py - marginY;
    if (py - m_camY > viewH - marginY) wantY = py - (viewH - marginY);
    wantX = constrain(wantX, 0, max(0, SCENE - viewW));
    wantY = constrain(wantY, 0, max(0, SCENE - viewH));
    bool moved = (wantX != m_camX) || (wantY != m_camY);
    m_camX = wantX; m_camY = wantY;

    for (int vy = 0; vy < viewH; vy++) {
        int sy = m_camY + vy;
        if (sy < 0 || sy >= SCENE) continue;
        const uint16_t* row = src + sy * SCENE;
        uint16_t* shadowRow = m_shadow ? (m_shadow + sy * SCENE) : nullptr;
        for (int vx = 0; vx < viewW; vx++) {
            int sx = m_camX + vx;
            if (sx < 0 || sx >= SCENE) continue;
            uint16_t c = row[sx];
            if (!m_fullBlit && !moved && shadowRow && shadowRow[sx] == c) continue;
            if (shadowRow) shadowRow[sx] = c;
            matrix->fillRect(vx * z, vy * z, z, z, c);
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

    // Diagnostics: how many canvas pixels are painted, and where Pac-Man is.
    {
        const uint16_t* buf = m_canvas->getBuffer();
        uint16_t lit = 0;
        for (int i = 0; i < SCENE * SCENE; i++) if (buf[i]) lit++;
        g_debug.alive = true;
        g_debug.litPixels = lit;
        g_debug.pacX = (int16_t)m_face->pacmanX();
        g_debug.pacY = (int16_t)m_face->pacmanY();
        g_debug.blits++;
    }

    m_hasFrame = true;
    if (m_zoom > 1) {
        presentZoomed(w, h);
    } else {
        present(w, h);
    }
}

void PacmanMazeClock::onDisplayGeometryChanged(const DisplayGeometry& geometry) {
    m_fullBlit = true;
}
