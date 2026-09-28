#ifndef MARIOCLOCK_H
#define MARIOCLOCK_H

#include "../ClockEngine.h"
#include "ClockFaceFont.h"

/**
 * Side-scrolling platform clock, in the spirit of the 64x64 "Clockwise" Mario face but laid out for
 * a wide panel.
 *
 * The clock itself keeps the square proportions of the original in the middle of the panel: two
 * blocks holding the hours and the minutes, with the ground running the full width and the hills and
 * clouds spread out to fill the sides rather than a square scene stretched across. Once a minute the
 * runner comes in from the left, jumps under a block in the middle, which knocks it up and flips its
 * digits, and carries on out to the right.
 *
 * All the art is drawn here, so the face carries no assets from another project. Geometry follows
 * the panel height (`s = h / 32`), which gives 256x64 a full-size scene and 128x32 the same scene at
 * half scale with the ground and sprite thinned to suit.
 */
class MarioClock : public ClockFace {
public:
    MarioClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config = nullptr);
    void draw(const TimeData& t) override;
    void update() override;

private:
    enum class Phase : uint8_t { Waiting, RunIn, Jump, RunOut };

    TimeData storedTime;
    ClockFaceFont faceFont;

    int lastMinute = -1;
    uint32_t lastFrameMs = 0;
    Phase phase = Phase::Waiting;
    float runnerX = -40.0f;     ///< runner centre, pixels; starts off the left edge
    float jumpT = 0.0f;         ///< 0..1 through the jump arc
    int jumpTarget = 1;         ///< block the jump is aimed at: 0 hours, 1 minutes
    float blockBounce[2] = { 0.0f, 0.0f };
    bool pendingDigits = false; ///< digits flip when the block is actually hit
    char shownHH[4] = "--";
    char shownMM[4] = "--";
    uint8_t runFrame = 0;

    void drawScene(int w, int h, int s);
    void drawGround(int w, int h, int s);
    void drawHill(int cx, int baseY, int s);
    void drawCloud(int x, int y, int s);
    void drawBlock(int cx, int cy, int s, const char* text, bool lit);
    void drawRunner(int cx, int footY, int s, bool airborne);
    void blitMask(const char* const* rows, int rowCount, int cols, int left, int top, int s, bool mirror);
};

#endif
