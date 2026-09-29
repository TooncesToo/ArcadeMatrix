#ifndef ARCADEMAZECLOCK_H
#define ARCADEMAZECLOCK_H

#include "../ClockEngine.h"

/**
 * The arcade maze, at arcade proportions, with the time in the ghost house.
 *
 * The map is the 28x31 grid of the original game: the same corridors, the four energizers, the side
 * tunnels and the ghost house in the middle. Walls are drawn the way the arcade draws them, as thin
 * outlines around the wall blocks rather than solid tiles, so the maze reads as lines rather than
 * bricks.
 *
 * A 4:1 sign cannot hold 31 rows at a readable size, so the panel is a window onto the maze: the
 * view follows Pac-Man, scrolling vertically as he moves between rows, and the clock sits where the
 * ghost house is, which means it scrolls in and out of view as he travels, exactly as a score or a
 * bonus fruit would.
 *
 * The maze is drawn into a PSRAM canvas at cell resolution and only the pixels that changed are
 * pushed to the panel, so the ghosts and the pellets cost very little between camera moves.
 */
class ArcadeMazeClock : public ClockFace {
public:
    ArcadeMazeClock(MatrixPanel_I2S_DMA* display, const EngineConfig* config = nullptr);
    ~ArcadeMazeClock() override;
    void draw(const TimeData& t) override;
    void update() override;
    void onDisplayGeometryChanged(const DisplayGeometry& geometry) override;
    bool wantsClear() const override { return false; }
    bool hasNewFrame() const override { return m_hasFrame; }
    void onActivated() override { m_fullBlit = true; m_hasFrame = true; }

    static constexpr int COLS = 28;
    static constexpr int ROWS = 31;

private:
    struct Mover {
        float x, y;          ///< pixel position of the sprite centre in the scene
        int dirX, dirY;      ///< current heading, one cell at a time
        int homeCol, homeRow;
        uint16_t color;
        bool frightened;
        uint32_t frightenedUntil;
        uint8_t scatterCorner;
    };

    TimeData storedTime;

    int m_cell = 8;             ///< pixels per maze cell
    int m_sceneW = 0, m_sceneH = 0;
    uint16_t* m_canvas = nullptr;
    uint16_t* m_shadow = nullptr;
    char m_map[ROWS][COLS + 1];     ///< working copy: pellets get eaten out of it
    bool m_hasFrame = true;
    bool m_fullBlit = true;
    bool m_started = false;
    uint32_t m_lastStepMs = 0;
    int m_camY = 0;

    Mover m_pac;
    Mover m_ghosts[4];
    uint8_t m_pacFrame = 0;
    uint32_t m_energizerBlinkMs = 0;

    void resetMaze();
    void drawMazeIntoCanvas();
    void drawWalls();
    void drawPellet(int col, int row);
    void drawClockInHouse();
    void stepEntities(uint32_t now);
    void moveMover(Mover& m, bool isPac);
    bool walkable(int col, int row, bool ghost) const;
    void chooseGhostDirection(Mover& g, int index);
    void choosePacDirection(Mover& p);
    void drawEntities();
    void present(int panelW, int panelH);

    // Canvas primitives: the canvas lives in PSRAM, so it is written directly rather than through
    // Adafruit_GFX, which would need the buffer in internal RAM.
    inline void px(int x, int y, uint16_t c) {
        if (x < 0 || y < 0 || x >= m_sceneW || y >= m_sceneH || !m_canvas) return;
        m_canvas[y * m_sceneW + x] = c;
    }
    void fillRect(int x, int y, int w, int h, uint16_t c);
    void hLine(int x, int y, int w, uint16_t c);
    void vLine(int x, int y, int h, uint16_t c);
    void drawDigit(int x, int y, char d, uint16_t c);
    void drawText(int x, int y, const char* s, uint16_t c);
};

#endif
