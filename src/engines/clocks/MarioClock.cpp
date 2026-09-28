#include "MarioClock.h"
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Sprite art. One character per pixel, so the runner can be edited in place:
//   .  transparent   c  cap and shirt   s  skin   o  overalls   b  boots
//   w  eye           k  outline
// ---------------------------------------------------------------------------
namespace {

constexpr int RUNNER_COLS = 12;
constexpr int RUNNER_ROWS = 16;

const char* const RUNNER_STAND[RUNNER_ROWS] = {
    "...kkkkk....",
    "..kccccck...",
    "..kcccccck..",
    "..ksssskk...",
    "..kswssk....",
    "..kssssssk..",
    "...kssssk...",
    "..kkcccckk..",
    ".kcoccccock.",
    ".kcoooooock.",
    ".ksooooooosk",
    "..kooooook..",
    "..koo..ook..",
    "..koo..ook..",
    ".kbbk..kbbk.",
    ".kbbk..kbbk.",
};

const char* const RUNNER_RUN[RUNNER_ROWS] = {
    "...kkkkk....",
    "..kccccck...",
    "..kcccccck..",
    "..ksssskk...",
    "..kswssk....",
    "..kssssssk..",
    "...kssssk...",
    "..kkcccckk..",
    ".kcoccccock.",
    "kcooooooock.",
    ".ksoooooosk.",
    "..koooooook.",
    "..koo...ok..",
    ".koo....ok..",
    "kbbk....kbb.",
    "kbb......bb.",
};

const char* const RUNNER_JUMP[RUNNER_ROWS] = {
    "...kkkkk..k.",
    "..kccccck.k.",
    "..kccccccks.",
    "..ksssskkss.",
    "..kswsskss..",
    "..kssssssk..",
    "...kssssk...",
    ".kkkcccckk..",
    "kcocccccock.",
    "kcooooooock.",
    ".ksoooooosk.",
    "..kooooook..",
    "..koo..ook..",
    ".koo....ok..",
    ".kbb....bbk.",
    "kbb......bbk",
};

}  // namespace

MarioClock::MarioClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config)
    : ClockFace(display, config) {
    faceFont.load(config);
    storedTime = { 0, 0, 0 };
}

void MarioClock::draw(const TimeData& t) {
    storedTime = t;
}

uint16_t MarioClock_paletteColor(MatrixPanel_I2S_DMA* m, char c) {
    switch (c) {
        case 'c': return m->color565(216, 40, 40);
        case 's': return m->color565(252, 188, 140);
        case 'o': return m->color565(48, 84, 216);
        case 'b': return m->color565(120, 64, 32);
        case 'w': return m->color565(255, 255, 255);
        case 'k': return m->color565(24, 20, 32);
        default: return 0;
    }
}

void MarioClock::blitMask(const char* const* rows, int rowCount, int cols, int left, int top, int s,
                          bool mirror) {
    if (!matrix) return;
    for (int r = 0; r < rowCount; r++) {
        const char* row = rows[r];
        int len = (int)strlen(row);
        for (int c = 0; c < cols; c++) {
            int src = mirror ? (cols - 1 - c) : c;
            if (src >= len) continue;
            char ch = row[src];
            if (ch == '.') continue;
            uint16_t col = MarioClock_paletteColor(matrix, ch);
            if (!col) continue;
            matrix->fillRect(left + c * s, top + r * s, s, s, col);
        }
    }
}

void MarioClock::drawGround(int w, int h, int s) {
    int groundH = 4 * s;
    int top = h - groundH;
    matrix->fillRect(0, top, w, groundH, matrix->color565(176, 88, 40));
    matrix->drawFastHLine(0, top, w, matrix->color565(232, 152, 88));
    uint16_t mortar = matrix->color565(96, 48, 24);
    int brickW = 8 * s;
    int courseH = groundH / 2;
    for (int row = 0; row < 2; row++) {
        int y = top + s + row * courseH;
        if (y >= h) break;
        matrix->drawFastHLine(0, y, w, mortar);
        for (int x = (row % 2) ? brickW / 2 : 0; x < w; x += brickW) {
            matrix->drawFastVLine(x, y, courseH, mortar);
        }
    }
}

void MarioClock::drawHill(int cx, int baseY, int s) {
    uint16_t body = matrix->color565(40, 152, 72);
    uint16_t edge = matrix->color565(24, 96, 48);
    int radius = 8 * s;
    for (int dy = 0; dy < radius; dy++) {
        int half = (int)((radius - dy) * 1.7f);
        int y = baseY - dy;
        if (y < 0) break;
        matrix->drawFastHLine(cx - half, y, half * 2, body);
        matrix->drawPixel(cx - half, y, edge);
        matrix->drawPixel(cx + half - 1, y, edge);
    }
}

void MarioClock::drawCloud(int x, int y, int s) {
    uint16_t body = matrix->color565(236, 240, 252);
    matrix->fillRect(x + 2 * s, y, 6 * s, 2 * s, body);
    matrix->fillRect(x, y + 2 * s, 10 * s, 2 * s, body);
    matrix->fillRect(x + 5 * s, y - s, 3 * s, 2 * s, body);
}

void MarioClock::drawBlock(int cx, int cy, int s, const char* text, bool lit) {
    int size = 13 * s;
    int left = cx - size / 2;
    int top = cy - size / 2;
    uint16_t face = lit ? matrix->color565(252, 216, 112) : matrix->color565(216, 152, 48);
    uint16_t edge = matrix->color565(96, 56, 16);
    matrix->fillRect(left, top, size, size, face);
    matrix->drawRect(left, top, size, size, edge);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            matrix->fillRect(left + s + i * (size - 3 * s), top + s + j * (size - 3 * s), s, s, edge);
        }
    }

    int box = size - 3 * s;
    int scale = (s >= 2) ? 2 : 1;
    faceFont.apply(*matrix, scale, text, box, box);
    int16_t bx, by;
    uint16_t bw, bh;
    matrix->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    while (scale > 1 && ((int)bw > box || (int)bh > box)) {
        scale--;
        faceFont.apply(*matrix, scale, text, box, box);
        matrix->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    }
    matrix->setTextColor(matrix->color565(40, 24, 8));
    matrix->setCursor(cx - (int)bw / 2 - bx, cy - (int)bh / 2 - by);
    matrix->print(text);
}

void MarioClock::drawRunner(int cx, int footY, int s, bool airborne) {
    const char* const* rows = airborne ? RUNNER_JUMP
                                       : ((runFrame & 1) ? RUNNER_RUN : RUNNER_STAND);
    blitMask(rows, RUNNER_ROWS, RUNNER_COLS, cx - (RUNNER_COLS * s) / 2, footY - RUNNER_ROWS * s, s,
             false);
}

void MarioClock::drawScene(int w, int h, int s) {
    matrix->fillScreen(matrix->color565(24, 40, 96));
    int groundTop = h - 4 * s;

    // The sides are filled with more landscape rather than a stretched square scene.
    int hillStep = 44 * s;
    for (int x = hillStep / 2; x < w + hillStep; x += hillStep) {
        drawHill(x, groundTop, s);
    }
    int cloudStep = 36 * s;
    for (int i = 0, x = 6 * s; x < w; x += cloudStep, i++) {
        drawCloud(x, (i % 2 ? 3 : 6) * s, s);
    }
    drawGround(w, h, s);
}

void MarioClock::update() {
    if (!matrix) return;
    const int w = matrix->width();
    const int h = matrix->height();
    const int s = (h >= 64) ? 2 : 1;

    uint32_t now = millis();
    float dt = (lastFrameMs == 0) ? 0.016f : (now - lastFrameMs) / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    lastFrameMs = now;

    int speedPct = engineConfig ? engineConfig->getInt("clock_speed", 100) : 100;
    speedPct = constrain(speedPct, 25, 300);

    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", storedTime.hours);
    snprintf(mm, sizeof(mm), "%02d", storedTime.minutes);
    if (shownHH[0] == '-') { strcpy(shownHH, hh); strcpy(shownMM, mm); }

    const int groundY = h - 4 * s;
    const int blockY = groundY - 13 * s - 6 * s;     // a jump's height above the ground
    const int hourX = w / 2 - 9 * s;
    const int minX = w / 2 + 9 * s;

    // A minute change sends him in from the left; the hour block is the target on the hour.
    if (lastMinute != storedTime.minutes) {
        bool newHour = (lastMinute >= 0 && storedTime.minutes == 0);
        lastMinute = storedTime.minutes;
        if (phase == Phase::Waiting) {
            phase = Phase::RunIn;
            jumpTarget = newHour ? 0 : 1;
            runnerX = -(float)(RUNNER_COLS * s);
            pendingDigits = true;
        }
    }

    float pace = 26.0f * s * (speedPct / 100.0f);
    int targetX = (jumpTarget == 0) ? hourX : minX;

    switch (phase) {
        case Phase::Waiting:
            break;
        case Phase::RunIn:
            runnerX += pace * dt;
            if ((now / 120) % 2 == 0) runFrame++;
            if (runnerX >= targetX) {
                runnerX = (float)targetX;
                phase = Phase::Jump;
                jumpT = 0.0f;
            }
            break;
        case Phase::Jump:
            jumpT += dt * 1.8f * (speedPct / 100.0f);
            if (jumpT >= 0.5f && pendingDigits) {     // the block is struck at the top of the arc
                pendingDigits = false;
                blockBounce[jumpTarget] = 0.001f;
                strcpy(shownHH, hh);
                strcpy(shownMM, mm);
            }
            if (jumpT >= 1.0f) {
                jumpT = 0.0f;
                phase = Phase::RunOut;
            }
            break;
        case Phase::RunOut:
            runnerX += pace * dt;
            if ((now / 120) % 2 == 0) runFrame++;
            if (runnerX > w + RUNNER_COLS * s) {
                phase = Phase::Waiting;
                runnerX = -(float)(RUNNER_COLS * s);
            }
            break;
    }

    for (int i = 0; i < 2; i++) {
        if (blockBounce[i] > 0.0f) {
            blockBounce[i] += dt * 3.2f;
            if (blockBounce[i] >= 1.0f) blockBounce[i] = 0.0f;
        }
    }

    drawScene(w, h, s);

    for (int i = 0; i < 2; i++) {
        float b = blockBounce[i];
        int lift = (b > 0.0f) ? (int)(sinf(b * 3.14159f) * 3 * s) : 0;
        drawBlock(i == 0 ? hourX : minX, blockY - lift, s, i == 0 ? shownHH : shownMM, b > 0.0f);
    }

    if (phase != Phase::Waiting) {
        int lift = (phase == Phase::Jump) ? (int)(sinf(jumpT * 3.14159f) * (groundY - blockY - 8 * s)) : 0;
        drawRunner((int)runnerX, groundY - lift, s, phase == Phase::Jump);
    }
}
