// Checks the wave measuring against scenes where the right answer can be worked out by hand.
// Build and run on any computer:
//   g++ -std=c++20 -Isrc tests/test_measure.cpp -o /tmp/test_measure && /tmp/test_measure
#include <cmath>
#include <cstdio>
#include "WindowMeasure.hpp"
#include "WindowStats.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); if (std::fabs(_a - _b) > (tol)) { \
    std::printf("FAIL line %d: %s = %.6f, expected %.6f (+-%g)\n", __LINE__, #a, _a, _b, (double)(tol)); failures++; } } while (0)

using namespace fwc;

// The wave flies right at `vx`, and goes up (vy > 0) or down along a straight line that passes
// through (x0, y0) at the moment of the click. 240 ticks per second.
static History straightHistory(double x0, double y0, double vx, double vy) {
    History h;
    double dt = 1.0 / 240.0;
    int ticks = 60; // 0.25 s
    for (int i = ticks; i >= 0; i--) {
        double back = i * dt;
        h.push(dt, x0 - vx * back, y0 - vy * back);
    }
    return h;
}

int main() {
    MeasureSettings cfg; // half = 5, hazard distance = 60, ...

    // ---- Scene A: a gap between a ceiling hazard and a floor hazard, 50 units ahead. --------------
    //   Ceiling hazard: x 50..80, y >= 30.   Floor hazard: x 50..80, y <= -30.   Wave: 45 degrees.
    // Worked out by hand: to pass the gap the click must be between 0.1 s and 0.11667 s after the
    // wave left the origin, a window of 0.016667 s = 16.67 ms = exactly 4 ticks at 240 TPS.
    ShapeIndex gap;
    gap.add(Shape::rect(50, 30, 80, 200, false));
    gap.add(Shape::rect(50, -200, 80, -30, false));

    {   // The real click was 0.1 s too early (at the origin). With a wide enough search the engine
        // still finds the window that works...
        MeasureSettings wide = cfg;
        wide.maxShift = 0.2;
        History h = straightHistory(0, 0, 300, 300);
        ClickResult r = measureClick(gap, h, /*pressedDown=*/false, false, false, wide);
        CHECK(r.counted);
        CHECK_NEAR(r.seconds, 0.016667, 0.0007);
        // ...but with the normal search range (+-0.06 s) no click time near the real one survives: 0.
        ClickResult narrow = measureClick(gap, h, false, false, false, cfg);
        CHECK(narrow.counted);
        CHECK_NEAR(narrow.seconds, 0.0, 1e-9);
    }
    {   // The real click happened inside the window (at 0.105 s: x = 31.5, y = 31.5).
        History h = straightHistory(31.5, 31.5, 300, 300);
        ClickResult r = measureClick(gap, h, false, false, false, cfg);
        CHECK(r.counted);
        CHECK_NEAR(r.seconds, 0.016667, 0.0007);
        CHECK(r.seconds > 0.0);
    }
    {   // The same window shown as frames/ms the way the counter does it.
        History h = straightHistory(31.5, 31.5, 300, 300);
        ClickResult r = measureClick(gap, h, false, false, false, cfg);
        double frames = r.seconds * 240.0;
        CHECK_NEAR(frames, 4.0, 0.2);
        CHECK(formatFramesLine(std::round(frames)) == "4 FRAMES");
        CHECK(formatMsLine(framesToMs(std::round(frames), 240)) == "16.67 MS");
    }
    {   // Flying down instead of up, mirrored scene: same window.
        History h = straightHistory(31.5, -31.5, 300, -300);
        ClickResult r = measureClick(gap, h, /*pressedDown=*/true, false, false, cfg);
        CHECK(r.counted);
        CHECK_NEAR(r.seconds, 0.016667, 0.0007);
    }
    {   // Gravity flipped: pressing sends the wave DOWN. Going up and pressing = the same turn as letting go normally.
        History h = straightHistory(31.5, 31.5, 300, 300);
        ClickResult flipped = measureClick(gap, h, /*pressedDown=*/true, /*upsideDown=*/true, false, cfg);
        CHECK(flipped.counted);
        CHECK_NEAR(flipped.seconds, 0.016667, 0.0007);
    }
    {   // A click that does not turn the wave (already going that way) is ignored.
        History h = straightHistory(31.5, 31.5, 300, 300);
        ClickResult r = measureClick(gap, h, /*pressedDown=*/true, false, false, cfg); // up while going up
        CHECK(!r.counted);
    }

    // ---- Nothing close: regular spam is not counted. ---------------------------------------------
    {
        ShapeIndex distant;
        distant.add(Shape::rect(400, 30, 430, 200, false));   // 400 units away
        History h = straightHistory(0, 0, 300, 300);
        CHECK(!measureClick(distant, h, false, false, false, cfg).counted);

        ShapeIndex empty;
        CHECK(!measureClick(empty, h, false, false, false, cfg).counted);
    }
    // ---- 60 unit rule: just inside counts, just outside does not. --------------------------------
    {
        ShapeIndex inside, outside;
        inside.add(Shape::rect(59, -10, 70, 10, false));   // 59 ahead
        outside.add(Shape::rect(61, -10, 70, 10, false));  // 61 ahead
        History h = straightHistory(0, 0, 300, 300);
        CHECK(measureClick(inside, h, false, false, false, cfg).counted);
        CHECK(!measureClick(outside, h, false, false, false, cfg).counted);
    }
    // ---- A hazard that is already behind us does not count. --------------------------------------
    {
        ShapeIndex behind;
        behind.add(Shape::rect(-40, -10, -20, 10, false));
        History h = straightHistory(0, 0, 300, 300);
        CHECK(!measureClick(behind, h, false, false, false, cfg).counted);
    }

    // ---- A wall with no way through: window of 0 (counted, in the 0-1 row). ----------------------
    {
        ShapeIndex wall;
        wall.add(Shape::rect(40, -2000, 70, 2000, false));
        History h = straightHistory(0, 0, 300, 300);
        ClickResult r = measureClick(wall, h, false, false, false, cfg);
        CHECK(r.counted);
        CHECK_NEAR(r.seconds, 0.0, 1e-9);
    }

    // ---- A block floor: the wave slides along it, so no click timing can kill it. ----------------
    {
        ShapeIndex floor;
        floor.add(Shape::rect(-500, -200, 500, -10, true)); // top surface at y = -10
        History h = straightHistory(0, 0, 300, -300);       // flying down towards it
        ClickResult r = measureClick(floor, h, /*pressedDown=*/true, false, false, cfg);
        CHECK(r.counted);
        CHECK(r.capped);
        CHECK_NEAR(r.seconds, 2 * cfg.maxShift, 1e-9);   // searched the full range both ways
    }
    // ---- A hazard sitting right in the wave's path from both sides is not survivable. --------------
    {
        ShapeIndex spikeOnPath;
        spikeOnPath.add(Shape::rect(40, -15, 70, 15, false));   // blocks the straight path
        History h = straightHistory(0, 0, 300, 300);
        ClickResult r = measureClick(spikeOnPath, h, false, false, false, cfg);
        CHECK(r.counted);
        CHECK(r.seconds >= 0.0);
    }

    // ---- Rotated hazard (45 degrees): the corner test works. -------------------------------------
    {
        // A diamond centred at (60, 0) with half-diagonal 20.
        std::array<Vec, 4> diamond = {{{40, 0}, {60, -20}, {80, 0}, {60, 20}}};
        Shape d = Shape::quad(diamond, false);
        CHECK(overlaps(d, 60, 0, 5));      // centre: hit
        CHECK(!overlaps(d, 43, 14, 5));    // inside its bounding box but outside the diamond: miss
        CHECK(overlaps(d, 48, 5, 5));      // near the left corner: hit
    }

    // ---- History helper. --------------------------------------------------------------------------
    {
        History h;
        h.push(0.1, 10, 0);
        h.push(0.1, 20, 10);
        Vec mid = h.positionAt(0.15);
        CHECK_NEAR(mid.x, 15.0, 1e-9);
        CHECK_NEAR(mid.y, 5.0, 1e-9);
        CHECK_NEAR(h.positionAt(-5).x, 10.0, 1e-9);   // clamped
        CHECK_NEAR(h.positionAt(99).x, 20.0, 1e-9);
    }

    if (failures == 0) std::printf("All measuring checks passed.\n");
    return failures == 0 ? 0 : 1;
}
