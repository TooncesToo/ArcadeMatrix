#include "ArcadeMazeClock.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>

namespace {

// The arcade map, 28 x 31.
//   # wall    . pellet   o energizer   space corridor or empty
//   - ghost house door   H ghost house interior (where the clock goes)
const char* const MAZE[ArcadeMazeClock::ROWS] = {
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.##### ## #####.######",
    "     #.##### ## #####.#     ",
    "     #.##          ##.#     ",
    "     #.## ###--### ##.#     ",
    "######.## #HHHHHH# ##.######",
    "      .   #HHHHHH#   .      ",
    "######.## #HHHHHH# ##.######",
    "     #.## ######## ##.#     ",
    "     #.##          ##.#     ",
    "     #.## ######## ##.#     ",
    "######.## ######## ##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##.......  .......##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################",
};

constexpr uint16_t WALL_BLUE  = 0x001F;   // the maze's blue
constexpr uint16_t PELLET     = 0xFEB2;   // warm white, as the arcade dots
constexpr uint16_t DOOR_PINK  = 0xFD59;
constexpr uint16_t PAC_YELLOW = 0xFFE0;
constexpr uint16_t FRIGHT_BLUE = 0x211F;
constexpr uint16_t EYE_WHITE  = 0xFFFF;

constexpr uint32_t STEP_MS = 90;          // one movement step
constexpr uint32_t FRIGHT_MS = 6000;

// 3x5 digits and a colon, drawn by hand because the canvas is not an Adafruit_GFX surface.
const uint8_t DIGITS[11][5] = {
    { 0b111, 0b101, 0b101, 0b101, 0b111 },  // 0
    { 0b010, 0b110, 0b010, 0b010, 0b111 },
    { 0b111, 0b001, 0b111, 0b100, 0b111 },
    { 0b111, 0b001, 0b111, 0b001, 0b111 },
    { 0b101, 0b101, 0b111, 0b001, 0b001 },
    { 0b111, 0b100, 0b111, 0b001, 0b111 },
    { 0b111, 0b100, 0b111, 0b101, 0b111 },
    { 0b111, 0b001, 0b010, 0b010, 0b010 },
    { 0b111, 0b101, 0b111, 0b101, 0b111 },
    { 0b111, 0b101, 0b111, 0b001, 0b111 },
    { 0b000, 0b010, 0b000, 0b010, 0b000 },  // colon
};

}  // namespace

ArcadeMazeClock::ArcadeMazeClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    storedTime = { 0, 0, 0 };
    int size = config ? config->getInt("clock_size", 8) : 8;
    m_cell = (size >= 4 && size <= 12) ? size : 8;
    m_sceneW = COLS * m_cell;
    m_sceneH = ROWS * m_cell;
    size_t px = (size_t)m_sceneW * m_sceneH;
    m_canvas = (uint16_t*)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    m_shadow = (uint16_t*)heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    resetMaze();
}

ArcadeMazeClock::~ArcadeMazeClock() {
    if (m_canvas) { heap_caps_free(m_canvas); m_canvas = nullptr; }
    if (m_shadow) { heap_caps_free(m_shadow); m_shadow = nullptr; }
}

void ArcadeMazeClock::draw(const TimeData& t) {
    storedTime = t;
}

void ArcadeMazeClock::fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, c);
}
void ArcadeMazeClock::hLine(int x, int y, int w, uint16_t c) { for (int i = 0; i < w; i++) px(x + i, y, c); }
void ArcadeMazeClock::vLine(int x, int y, int h, uint16_t c) { for (int i = 0; i < h; i++) px(x, y + i, c); }

void ArcadeMazeClock::drawDigit(int x, int y, char d, uint16_t c) {
    int idx = (d == ':') ? 10 : (d - '0');
    if (idx < 0 || idx > 10) return;
    for (int r = 0; r < 5; r++)
        for (int b = 0; b < 3; b++)
            if (DIGITS[idx][r] & (1 << (2 - b))) px(x + b, y + r, c);
}

void ArcadeMazeClock::drawText(int x, int y, const char* s, uint16_t c) {
    int cx = x;
    for (const char* p = s; *p; p++) {
        drawDigit(cx, y, *p, c);
        cx += (*p == ':') ? 2 : 4;
    }
}

void ArcadeMazeClock::resetMaze() {
    for (int r = 0; r < ROWS; r++) {
        strncpy(m_map[r], MAZE[r], COLS);
        m_map[r][COLS] = '\0';
    }
    // Pac-Man starts where the arcade puts him, below the house.
    m_pac = { (13.5f + 0.5f) * m_cell, (23 + 0.5f) * m_cell, -1, 0, 13, 23, PAC_YELLOW, false, 0, 0 };
    const uint16_t ghostColors[4] = { 0xF800, 0xFD3C, 0x07FF, 0xFCC0 };   // red, pink, cyan, orange
    for (int i = 0; i < 4; i++) {
        m_ghosts[i] = { (11.0f + i + 0.5f) * m_cell, (14 + 0.5f) * m_cell, (i % 2) ? 1 : -1, 0,
                        11 + i, 14, ghostColors[i], false, 0, (uint8_t)i };
    }
    m_fullBlit = true;
}

bool ArcadeMazeClock::walkable(int col, int row, bool ghost) const {
    if (row < 0 || row >= ROWS) return false;
    if (col < 0 || col >= COLS) return true;      // the side tunnels wrap
    char c = m_map[row][col];
    if (c == '#') return false;
    if (c == '-') return ghost;                   // only ghosts pass the house door
    return true;
}

// Walls are drawn as outlines: a blue line on every side of a wall block that faces open space,
// which is how the arcade maze reads as thin double lines rather than solid tiles.
void ArcadeMazeClock::drawWalls() {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char ch = m_map[r][c];
            int x = c * m_cell, y = r * m_cell;
            if (ch == '-') {
                hLine(x, y + m_cell / 2, m_cell, DOOR_PINK);
                continue;
            }
            if (ch != '#') continue;
            bool up = (r > 0) && m_map[r - 1][c] == '#';
            bool down = (r < ROWS - 1) && m_map[r + 1][c] == '#';
            bool left = (c > 0) && m_map[r][c - 1] == '#';
            bool right = (c < COLS - 1) && m_map[r][c + 1] == '#';
            if (!up) hLine(x, y, m_cell, WALL_BLUE);
            if (!down) hLine(x, y + m_cell - 1, m_cell, WALL_BLUE);
            if (!left) vLine(x, y, m_cell, WALL_BLUE);
            if (!right) vLine(x + m_cell - 1, y, m_cell, WALL_BLUE);
            // Inner corners, so junctions close up instead of leaving a gap.
            if (!up && !left) px(x, y, WALL_BLUE);
            if (!up && !right) px(x + m_cell - 1, y, WALL_BLUE);
            if (!down && !left) px(x, y + m_cell - 1, WALL_BLUE);
            if (!down && !right) px(x + m_cell - 1, y + m_cell - 1, WALL_BLUE);
        }
    }
}

void ArcadeMazeClock::drawPellet(int col, int row) {
    char ch = m_map[row][col];
    int cx = col * m_cell + m_cell / 2;
    int cy = row * m_cell + m_cell / 2;
    if (ch == '.') {
        px(cx, cy, PELLET);
        px(cx - 1, cy, PELLET);
        px(cx, cy - 1, PELLET);
        px(cx - 1, cy - 1, PELLET);
    } else if (ch == 'o') {
        bool on = ((millis() / 250) & 1);
        uint16_t c = on ? PELLET : 0x0000;
        int r = m_cell / 3;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, c);
    }
}

// The clock lives in the ghost house, where the arcade shows the ghosts waiting to come out.
void ArcadeMazeClock::drawClockInHouse() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", storedTime.hours, storedTime.minutes);
    int len = 0;
    for (const char* p = buf; *p; p++) len += (*p == ':') ? 2 : 4;
    int houseCx = (14 * m_cell);
    int houseCy = (14 * m_cell) + m_cell / 2;
    drawText(houseCx - len / 2, houseCy - 2, buf, PELLET);
}

void ArcadeMazeClock::drawMazeIntoCanvas() {
    if (!m_canvas) return;
    memset(m_canvas, 0, (size_t)m_sceneW * m_sceneH * sizeof(uint16_t));
    drawWalls();
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (m_map[r][c] == '.' || m_map[r][c] == 'o') drawPellet(c, r);
    drawClockInHouse();
    drawEntities();
}

void ArcadeMazeClock::drawEntities() {
    const int rad = m_cell / 2 - 1;

    // Ghosts: body, skirt and eyes, at cell size.
    for (int i = 0; i < 4; i++) {
        const Mover& g = m_ghosts[i];
        uint16_t body = g.frightened ? FRIGHT_BLUE : g.color;
        int gx = (int)g.x, gy = (int)g.y;
        for (int dy = -rad; dy <= rad; dy++) {
            for (int dx = -rad; dx <= rad; dx++) {
                bool inside = (dy < 0) ? (dx * dx + dy * dy <= rad * rad) : true;
                if (!inside) continue;
                if (dy == rad && ((dx + rad) % 3 == 0)) continue;   // scalloped skirt
                px(gx + dx, gy + dy, body);
            }
        }
        if (!g.frightened) {
            px(gx - rad / 2, gy - 1, EYE_WHITE);
            px(gx + rad / 2, gy - 1, EYE_WHITE);
        }
    }

    // Pac-Man: a disc with a wedge cut out towards the way he is going.
    int pxm = (int)m_pac.x, pym = (int)m_pac.y;
    int mouth = (m_pacFrame & 1) ? rad : rad / 2;
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy > rad * rad) continue;
            int along = m_pac.dirX ? dx * m_pac.dirX : dy * m_pac.dirY;
            int across = m_pac.dirX ? dy : dx;
            if (along > 0 && abs(across) < mouth && abs(across) <= along) continue;   // the mouth
            px(pxm + dx, pym + dy, PAC_YELLOW);
        }
    }
}

void ArcadeMazeClock::choosePacDirection(Mover& p) {
    // Head for the nearest pellet, preferring to keep going rather than turn on the spot.
    int col = (int)(p.x / m_cell), row = (int)(p.y / m_cell);
    int bestDx = 0, bestDy = 0, bestScore = -1;
    const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (auto& d : dirs) {
        int nc = col + d[0], nr = row + d[1];
        if (!walkable(nc, nr, false)) continue;
        int score = 0;
        // Look a few cells ahead for food.
        for (int step = 1; step <= 8; step++) {
            int sc = nc + d[0] * step, sr = nr + d[1] * step;
            if (sc < 0 || sc >= COLS || sr < 0 || sr >= ROWS) break;
            if (!walkable(sc, sr, false)) break;
            char ch = m_map[sr][sc];
            if (ch == 'o') { score += 30 - step; break; }
            if (ch == '.') { score += 20 - step; break; }
        }
        if (d[0] == p.dirX && d[1] == p.dirY) score += 3;              // keep going
        if (d[0] == -p.dirX && d[1] == -p.dirY) score -= 8;            // avoid doubling back
        for (int i = 0; i < 4; i++) {                                   // and avoid the ghosts
            if (m_ghosts[i].frightened) continue;
            int gc = (int)(m_ghosts[i].x / m_cell), gr = (int)(m_ghosts[i].y / m_cell);
            int dist = abs(gc - nc) + abs(gr - nr);
            if (dist < 5) score -= (6 - dist) * 4;
        }
        if (score > bestScore) { bestScore = score; bestDx = d[0]; bestDy = d[1]; }
    }
    if (bestScore >= 0) { p.dirX = bestDx; p.dirY = bestDy; }
}

void ArcadeMazeClock::chooseGhostDirection(Mover& g, int index) {
    int col = (int)(g.x / m_cell), row = (int)(g.y / m_cell);
    int pacCol = (int)(m_pac.x / m_cell), pacRow = (int)(m_pac.y / m_cell);

    // Scatter to a corner for part of the time, chase the rest, flee when frightened: the arcade's
    // rhythm, simplified to a period rather than the original's exact table.
    bool scatter = ((millis() / 7000) % 3) == 0;
    int targetCol = pacCol, targetRow = pacRow;
    if (g.frightened) {
        targetCol = (g.scatterCorner % 2) ? 1 : COLS - 2;
        targetRow = (g.scatterCorner < 2) ? 1 : ROWS - 2;
    } else if (scatter) {
        targetCol = (g.scatterCorner % 2) ? 1 : COLS - 2;
        targetRow = (g.scatterCorner < 2) ? 1 : ROWS - 2;
    } else if (index == 1) {
        targetCol = pacCol + m_pac.dirX * 4;      // ahead of him, like Pinky
        targetRow = pacRow + m_pac.dirY * 4;
    } else if (index == 3) {
        targetCol = pacCol + (col < pacCol ? -3 : 3);   // hangs back, like Clyde
    }

    int bestDx = g.dirX, bestDy = g.dirY, bestDist = INT32_MAX;
    const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (auto& d : dirs) {
        if (d[0] == -g.dirX && d[1] == -g.dirY) continue;     // ghosts never reverse
        int nc = col + d[0], nr = row + d[1];
        if (!walkable(nc, nr, true)) continue;
        int dist = (nc - targetCol) * (nc - targetCol) + (nr - targetRow) * (nr - targetRow);
        if (dist < bestDist) { bestDist = dist; bestDx = d[0]; bestDy = d[1]; }
    }
    g.dirX = bestDx; g.dirY = bestDy;
}

void ArcadeMazeClock::moveMover(Mover& m, bool isPac) {
    float speed = isPac ? (m_cell / 4.0f) : (m.frightened ? m_cell / 6.0f : m_cell / 4.5f);
    m.x += m.dirX * speed;
    m.y += m.dirY * speed;
    // Tunnels: off one side, in at the other.
    if (m.x < -m_cell / 2) m.x = m_sceneW + m_cell / 2 - 1;
    if (m.x > m_sceneW + m_cell / 2) m.x = -m_cell / 2;
    if (m.y < 0) m.y = 0;
    if (m.y > m_sceneH) m.y = (float)m_sceneH;
}

void ArcadeMazeClock::stepEntities(uint32_t now) {
    int col = (int)(m_pac.x / m_cell), row = (int)(m_pac.y / m_cell);

    // Eat what he is standing on.
    if (col >= 0 && col < COLS && row >= 0 && row < ROWS) {
        char& cell = m_map[row][col];
        if (cell == '.') cell = ' ';
        else if (cell == 'o') {
            cell = ' ';
            for (int i = 0; i < 4; i++) {
                m_ghosts[i].frightened = true;
                m_ghosts[i].frightenedUntil = now + FRIGHT_MS;
            }
        }
    }

    // At a cell centre, everyone picks a direction.
    bool atCentre = (fmodf(m_pac.x, (float)m_cell) < m_cell / 3.0f) &&
                    (fmodf(m_pac.y, (float)m_cell) < m_cell / 3.0f);
    if (atCentre || !walkable(col + m_pac.dirX, row + m_pac.dirY, false)) choosePacDirection(m_pac);
    moveMover(m_pac, true);
    m_pacFrame++;

    for (int i = 0; i < 4; i++) {
        Mover& g = m_ghosts[i];
        if (g.frightened && now > g.frightenedUntil) g.frightened = false;
        int gc = (int)(g.x / m_cell), gr = (int)(g.y / m_cell);
        bool gCentre = (fmodf(g.x, (float)m_cell) < m_cell / 3.0f) &&
                       (fmodf(g.y, (float)m_cell) < m_cell / 3.0f);
        if (gCentre || !walkable(gc + g.dirX, gr + g.dirY, true)) chooseGhostDirection(g, i);
        moveMover(g, false);

        // Caught: frightened ghosts go home, otherwise Pac-Man restarts from his corner.
        if (abs((int)g.x - (int)m_pac.x) < m_cell / 2 && abs((int)g.y - (int)m_pac.y) < m_cell / 2) {
            if (g.frightened) {
                g.x = (g.homeCol + 0.5f) * m_cell;
                g.y = (g.homeRow + 0.5f) * m_cell;
                g.frightened = false;
            } else {
                m_pac.x = (13.5f + 0.5f) * m_cell;
                m_pac.y = (23 + 0.5f) * m_cell;
                m_pac.dirX = -1; m_pac.dirY = 0;
            }
        }
    }

    // A cleared maze fills again, so the face never runs out of anything to do.
    bool anyFood = false;
    for (int r = 0; r < ROWS && !anyFood; r++)
        for (int c = 0; c < COLS; c++)
            if (m_map[r][c] == '.' || m_map[r][c] == 'o') { anyFood = true; break; }
    if (!anyFood) resetMaze();
}

void ArcadeMazeClock::present(int panelW, int panelH) {
    if (!m_canvas || !matrix) return;
    const int viewH = panelH;
    const int ox = (panelW - m_sceneW) / 2;      // the maze is narrower than the panel: centre it

    // The view follows Pac-Man down the maze, keeping him inside a margin.
    int want = (int)m_pac.y - viewH / 2;
    if (want < 0) want = 0;
    if (want > m_sceneH - viewH) want = m_sceneH - viewH;
    bool moved = (want != m_camY);
    m_camY = want;

    for (int y = 0; y < viewH; y++) {
        int sy = m_camY + y;
        if (sy < 0 || sy >= m_sceneH) continue;
        const uint16_t* row = m_canvas + sy * m_sceneW;
        uint16_t* shadowRow = m_shadow ? (m_shadow + sy * m_sceneW) : nullptr;
        for (int x = 0; x < m_sceneW; x++) {
            uint16_t c = row[x];
            if (!m_fullBlit && !moved && shadowRow && shadowRow[x] == c) continue;
            if (shadowRow) shadowRow[x] = c;
            int pxx = ox + x;
            if (pxx >= 0 && pxx < panelW) matrix->drawPixel(pxx, y, c);
        }
    }
    if (m_fullBlit || moved) {   // the margins either side of the maze stay black
        if (ox > 0) {
            matrix->fillRect(0, 0, ox, panelH, 0x0000);
            matrix->fillRect(ox + m_sceneW, 0, panelW - (ox + m_sceneW), panelH, 0x0000);
        }
    }
    m_fullBlit = false;
}

void ArcadeMazeClock::update() {
    if (!matrix || !m_canvas) return;
    const int w = matrix->width();
    const int h = matrix->height();

    uint32_t now = millis();
    if (!m_started) { m_started = true; m_lastStepMs = now; }
    if (now - m_lastStepMs >= STEP_MS) {
        m_lastStepMs = now;
        stepEntities(now);
    }

    drawMazeIntoCanvas();
    m_hasFrame = true;
    present(w, h);
}

void ArcadeMazeClock::onDisplayGeometryChanged(const DisplayGeometry& geometry) {
    m_fullBlit = true;
}
