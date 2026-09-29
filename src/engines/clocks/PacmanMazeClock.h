#ifndef PACMANMAZECLOCK_H
#define PACMANMAZECLOCK_H

#include "../ClockEngine.h"
#include <Adafruit_GFX.h>

class CwMazeFace;

/**
 * Maze clock: the 64x64 "Clockwise" clockface cw-cf-0x05, where Pac-Man works his way around a maze
 * with the ghosts chasing him and the time laid into the middle of it.
 *
 * That face is written for a square panel and draws at coordinates 0..63, so rather than rework its
 * logic it draws into a 64x64 canvas of its own and the canvas is blitted to the middle of the
 * panel, with its edge columns carried out to the sides. Only pixels that actually changed are
 * pushed to the panel, so a maze full of small moving sprites costs a fraction of a full repaint.
 */
class PacmanMazeClock : public ClockFace {
public:
    /// Last frame's state, for /api/stats: how much of the canvas is painted and where Pac-Man is.
    struct Debug { bool alive; uint16_t litPixels; int16_t pacX; int16_t pacY; uint16_t blits; };
    static Debug debug();
    PacmanMazeClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config = nullptr);
    ~PacmanMazeClock() override;
    void draw(const TimeData& t) override;
    void update() override;
    void onDisplayGeometryChanged(const DisplayGeometry& geometry) override;
    bool wantsClear() const override { return false; }
    bool hasNewFrame() const override { return m_hasFrame; }
    void onActivated() override { m_fullBlit = true; m_hasFrame = true; }

private:
    TimeData storedTime;
    GFXcanvas16* m_canvas = nullptr;
    uint16_t* m_shadow = nullptr;     ///< what the panel already shows, for the difference blit
    CwMazeFace* m_face = nullptr;
    bool m_started = false;
    bool m_fullBlit = true;
    bool m_hasFrame = true;

    // Zoom: 1 shows the whole scene centred, 2 magnifies it and the view follows Pac-Man, so the
    // panel is a window onto the maze rather than the whole of it.
    int m_zoom = 1;
    int m_camX = 0;
    int m_camY = 0;

    void present(int panelW, int panelH);
    void presentZoomed(int panelW, int panelH);
};

#endif
