#ifndef CW_SHIM_H
#define CW_SHIM_H

#include <Arduino.h>
#include <Adafruit_GFX.h>

/**
 * The few framework pieces the Clockwise maze clockface expects, so its sources can be carried over
 * with their logic untouched.
 *
 * In the Clockwise firmware these come from Game.h, Locator.h and EventBus.h. Here they are a
 * handful of declarations over an Adafruit_GFX surface, which is the 64x64 canvas the face draws
 * into before it is blitted to the panel.
 */

enum class Direction : uint8_t { UP, DOWN, LEFT, RIGHT, NONE };
enum class EventType : uint8_t { COLLISION, TICK };

class Sprite {
public:
    virtual ~Sprite() = default;
    int _x = 0;
    int _y = 0;
    int _width = 0;
    int _height = 0;

    /// Box overlap, the collision test the face expects from the framework's Sprite.
    bool collidedWith(const Sprite* other) const {
        if (!other) return false;
        return _x < other->_x + other->_width && _x + _width > other->_x &&
               _y < other->_y + other->_height && _y + _height > other->_y;
    }
};

class EventTask {
public:
    virtual ~EventTask() = default;
    virtual void execute(EventType event, Sprite* caller) = 0;
};

/// The display the face draws into, provided once at construction.
class Locator {
public:
    static void provide(Adafruit_GFX* display) { s_display = display; }
    static Adafruit_GFX* getDisplay() { return s_display; }

private:
    static Adafruit_GFX* s_display;
};

/// Time source, filled from the engine's clock before each update.
class CWDateTime {
public:
    void set(uint8_t h, uint8_t m, uint8_t s, uint8_t day, uint8_t month, uint8_t weekday) {
        _h = h; _m = m; _s = s; _day = day; _month = month; _weekday = weekday;
    }
    uint8_t getHour() const { return _h; }
    uint8_t getMinute() const { return _m; }
    /// Formatted variants, as the framework provides them ("%02d" and friends).
    const char* getHour(const char* fmt) { snprintf(_buf, sizeof(_buf), fmt, (int)_h); return _buf; }
    const char* getMinute(const char* fmt) { snprintf(_buf2, sizeof(_buf2), fmt, (int)_m); return _buf2; }
    uint32_t getMilliseconds() const { return millis(); }
    uint8_t getSecond() const { return _s; }
    uint8_t getDay() const { return _day; }
    uint8_t getMonth() const { return _month; }
    uint8_t getWeekday() const { return _weekday; }

private:
    uint8_t _h = 0, _m = 0, _s = 0, _day = 1, _month = 1, _weekday = 0;
    char _buf[8] = {0};
    char _buf2[8] = {0};
};

#endif
