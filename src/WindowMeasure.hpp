#pragma once

// Real window measuring for EVERY gamemode (cube, ship, ball, UFO, wave, robot, spider, swing).
// Pure geometry + kinematics: no Geometry Dash code in here, so it can be tested on any computer
// (see tests/test_measure.cpp).
//
// The idea ("ghost"):
//   We remember where the player was on the last physics ticks, and also the few ticks AFTER a
//   click (the measurement is done a few ticks late, which is why). From that recording we read:
//     - how the player was moving just BEFORE the click (speed + how that speed was changing),
//     - how the player moved just AFTER the click (the jump / flip / thrust the click produced).
//   Then we ask: "if the click had come a bit earlier or later, and nothing else changed, would the
//   player still get past the hazards in front?" The range of click times that still survive is the
//   window. Its length in seconds is the time you had to click (ms); times the TPS gives frames.
//
//   Because the motion is read from the recording and not hard-coded, the SAME code handles all
//   gamemodes: for the wave it is a straight slope, for a cube a jump arc, for a ship a curve, etc.
//
// Simplifications: objects that move during the level are treated as standing still; slopes are
// ignored; only the hazards right ahead are checked (your later clicks are not taken into account,
// so a window can read larger than a long spam sequence really allows). Spider teleports and very
// sharp motion are handled only roughly.

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

namespace fwc {

struct Vec {
    double x = 0.0;
    double y = 0.0;
};

// ---------------------------------------------------------------------------------------------
// Shapes: hazards (spikes, saws) and blocks.
// ---------------------------------------------------------------------------------------------
struct Shape {
    std::array<Vec, 4> pts{};  // corners in order around the shape
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool aligned = true;       // plain axis-aligned rectangle?
    bool solid = false;        // block: the player slides along top/bottom. Hazard: any touch kills.
    double tol = 0.0;          // slopes: how deep (units) the player may graze the surface without dying

    static Shape rect(double minX, double minY, double maxX, double maxY, bool solid) {
        Shape s;
        s.pts = {{{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}}};
        s.minX = minX; s.minY = minY; s.maxX = maxX; s.maxY = maxY;
        s.aligned = true;
        s.solid = solid;
        return s;
    }

    // A rotated box. Corners must go around the shape (not criss-cross).
    static Shape quad(std::array<Vec, 4> const& pts, bool solid) {
        Shape s;
        s.pts = pts;
        s.minX = s.maxX = pts[0].x;
        s.minY = s.maxY = pts[0].y;
        for (auto const& p : pts) {
            s.minX = std::min(s.minX, p.x); s.maxX = std::max(s.maxX, p.x);
            s.minY = std::min(s.minY, p.y); s.maxY = std::max(s.maxY, p.y);
        }
        s.aligned = false;
        s.solid = solid;
        return s;
    }

    // A right triangle that fills half of the box (centre cx,cy, half sizes hw,hh). (rx,ry) is the
    // direction (each +-1) of the box corner where the right angle sits. The slope is the diagonal
    // across the box; players may graze it by `tol` units (sliding along a slope is legal).
    static Shape slope(double cx, double cy, double hw, double hh, int rx, int ry, bool solid, double tol) {
        Vec R{cx + rx * hw, cy + ry * hh};
        Vec A{cx - rx * hw, cy + ry * hh};
        Vec B{cx + rx * hw, cy - ry * hh};
        Shape s = quad({R, A, B, B}, solid);
        s.tol = tol;
        return s;
    }
};

// Does the player's square hitbox (centre cx,cy, half-size `half`) overlap the shape?
// The square is shrunk by a hair so just touching a surface is not a hit.
inline bool overlaps(Shape const& s, double cx, double cy, double half) {
    double e = std::max(half - 0.01 - s.tol, half * 0.4);
    double pMinX = cx - e, pMaxX = cx + e, pMinY = cy - e, pMaxY = cy + e;
    if (pMaxX <= s.minX || pMinX >= s.maxX || pMaxY <= s.minY || pMinY >= s.maxY) return false;
    if (s.aligned) return true;

    // Rotated box: separating axis test on the box's two edge directions.
    for (int i = 0; i < 2; i++) {
        double ex = s.pts[i + 1].x - s.pts[i].x;
        double ey = s.pts[i + 1].y - s.pts[i].y;
        double nx = -ey, ny = ex;
        double len = std::hypot(nx, ny);
        if (len < 1e-9) continue;
        nx /= len; ny /= len;

        double pMin = std::numeric_limits<double>::infinity(), pMax = -pMin;
        for (double px : {pMinX, pMaxX}) {
            for (double py : {pMinY, pMaxY}) {
                double d = nx * px + ny * py;
                pMin = std::min(pMin, d); pMax = std::max(pMax, d);
            }
        }
        double qMin = std::numeric_limits<double>::infinity(), qMax = -qMin;
        for (auto const& p : s.pts) {
            double d = nx * p.x + ny * p.y;
            qMin = std::min(qMin, d); qMax = std::max(qMax, d);
        }
        if (pMax <= qMin || qMax <= pMin) return false;
    }
    return true;
}

// All shapes of a level, sorted into 100-unit wide columns so nearby ones are found fast.
class ShapeIndex {
public:
    void clear() { m_shapes.clear(); m_cells.clear(); }
    size_t size() const { return m_shapes.size(); }

    void add(Shape const& s) {
        if (!(s.maxX >= s.minX) || !(s.maxY >= s.minY)) return;
        if (s.maxX - s.minX > 50000.0) return; // nonsense
        int index = static_cast<int>(m_shapes.size());
        m_shapes.push_back(s);
        int c0 = cell(s.minX), c1 = cell(s.maxX);
        for (int c = c0; c <= c1; c++) m_cells[c].push_back(index);
    }

    // Shapes whose bounds touch the box. Pointers stay valid until the next add()/clear().
    void query(double minX, double minY, double maxX, double maxY, std::vector<Shape const*>& out) const {
        out.clear();
        std::vector<int> ids;
        for (int c = cell(minX); c <= cell(maxX); c++) {
            auto it = m_cells.find(c);
            if (it == m_cells.end()) continue;
            ids.insert(ids.end(), it->second.begin(), it->second.end());
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (int id : ids) {
            Shape const& s = m_shapes[id];
            if (s.maxX < minX || s.minX > maxX || s.maxY < minY || s.minY > maxY) continue;
            out.push_back(&s);
        }
    }

private:
    static int cell(double x) { return static_cast<int>(std::floor(x / 100.0)); }

    std::vector<Shape> m_shapes;
    std::unordered_map<int, std::vector<int>> m_cells;
};

// ---------------------------------------------------------------------------------------------
// History: where the player was on the last physics ticks (kept a little past each click so the
// after-click motion can be measured).
// ---------------------------------------------------------------------------------------------
struct Sample {
    double t = 0, x = 0, y = 0;
};

class History {
public:
    void clear() { m_samples.clear(); m_time = 0.0; m_ticks = 0; }
    size_t size() const { return m_samples.size(); }
    double now() const { return m_time; }
    long ticks() const { return m_ticks; } // total ticks ever pushed (keeps counting past the cap)

    // Call once per physics tick with the tick length and the player's position after the tick.
    void push(double dt, double x, double y) {
        if (!(dt > 0.0)) return;
        m_time += dt;
        m_ticks++;
        m_samples.push_back({m_time, x, y});
        while (m_samples.size() > 2 && m_samples.back().t - m_samples.front().t > kKeepSeconds) {
            m_samples.pop_front();
        }
    }

    Sample const& last() const { return m_samples.back(); }
    Sample const& at(size_t fromEnd) const { return m_samples[m_samples.size() - 1 - fromEnd]; }
    double span() const { return m_samples.size() < 2 ? 0.0 : m_samples.back().t - m_samples.front().t; }

    // Index of the sample closest in time to `t` (or -1 if empty).
    int indexAt(double t) const {
        if (m_samples.empty()) return -1;
        int best = 0;
        double bestd = std::fabs(m_samples[0].t - t);
        for (size_t i = 1; i < m_samples.size(); i++) {
            double d = std::fabs(m_samples[i].t - t);
            if (d < bestd) { bestd = d; best = static_cast<int>(i); }
        }
        return best;
    }

    Sample const& sample(int i) const { return m_samples[i]; }

    // Position at time t (straight line between ticks, clamped to what we have).
    Vec positionAt(double t) const {
        if (m_samples.empty()) return {};
        if (t <= m_samples.front().t) return {m_samples.front().x, m_samples.front().y};
        if (t >= m_samples.back().t) return {m_samples.back().x, m_samples.back().y};
        for (size_t i = m_samples.size() - 1; i > 0; i--) {
            Sample const& a = m_samples[i - 1];
            Sample const& b = m_samples[i];
            if (t >= a.t) {
                double f = (b.t > a.t) ? (t - a.t) / (b.t - a.t) : 0.0;
                return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
            }
        }
        return {m_samples.front().x, m_samples.front().y};
    }

    static constexpr double kKeepSeconds = 0.35;

private:
    std::deque<Sample> m_samples;
    double m_time = 0.0;
    long m_ticks = 0;
};

// ---------------------------------------------------------------------------------------------
// The player's motion around a click, read from the recording.
// Vertical position is modelled as constant acceleration before the click and (a different) constant
// acceleration after it, which is a good local fit for every gamemode over the short look-ahead.
// ---------------------------------------------------------------------------------------------
struct Motion {
    bool valid = false;
    bool changed = false;     // did this click actually change the motion? (press that did nothing -> false)
    double tc = 0;            // click time
    double xc = 0, yc = 0;    // position at the click
    double vx = 0;            // horizontal speed (units per second)
    double vyBefore = 0, aBefore = 0; // vertical speed & acceleration just before the click
    double vyAfter = 0, aAfter = 0;   // vertical speed & acceleration just after the click
    bool teleport = false;    // spider-style instant jump
    double ySwitch = 0;       // where the player ends up right after a teleport
};

// Fit speed/acceleration from three samples (handles slightly uneven tick lengths).
// Returns the speed at the LAST of the three and the acceleration across them.
inline void fitVelAcc(Sample const& p0, Sample const& p1, Sample const& p2,
                      double& velAtP2, double& acc) {
    double d01 = p1.t - p0.t, d12 = p2.t - p1.t;
    double v01 = d01 > 1e-9 ? (p1.y - p0.y) / d01 : 0.0;
    double v12 = d12 > 1e-9 ? (p2.y - p1.y) / d12 : 0.0;
    double mid = 0.5 * (d01 + d12);
    acc = mid > 1e-9 ? (v12 - v01) / mid : 0.0;
    velAtP2 = v12; // speed over the last step, a good stand-in for the speed at p2
}

// Build the motion around the click at time `clickTime`. Needs a few samples before and after it,
// which is why measuring is done a few ticks after the click.
inline Motion motionFrom(History const& h, double clickTime) {
    Motion m;
    if (h.size() < 9) return m;
    int i = h.indexAt(clickTime);
    int n = static_cast<int>(h.size());
    if (i < 4 || i > n - 5) return m; // not enough on one side yet

    Sample const& c = h.sample(i);
    m.tc = c.t; m.xc = c.x; m.yc = c.y;

    // Horizontal speed from a few ticks either side.
    double dtx = h.sample(i + 2).t - h.sample(i - 2).t;
    m.vx = dtx > 1e-9 ? (h.sample(i + 2).x - h.sample(i - 2).x) / dtx : 0.0;
    if (!(m.vx > 1.0)) return m; // not moving forward (or going backwards): nothing to measure

    fitVelAcc(h.sample(i - 4), h.sample(i - 2), h.sample(i), m.vyBefore, m.aBefore);
    fitVelAcc(h.sample(i), h.sample(i + 2), h.sample(i + 4), m.vyAfter, m.aAfter);

    // Spider-style teleport: a big vertical jump in a single tick right after the click.
    double step1 = std::fabs(h.sample(i + 1).y - c.y);
    if (step1 > 40.0) {
        m.teleport = true;
        m.ySwitch = h.sample(i + 2).y; // where it lands
        // Re-read the after-motion from past the teleport.
        if (i <= n - 7) fitVelAcc(h.sample(i + 2), h.sample(i + 4), h.sample(i + 6), m.vyAfter, m.aAfter);
        else { m.vyAfter = 0; m.aAfter = m.aBefore; }
    }

    // Did the click change anything? Compare where the player really went a few ticks later with
    // where it would have gone if nothing had happened (same speed & acceleration as before).
    double tL = h.sample(i + 4).t - c.t;
    double predicted = m.yc + m.vyBefore * tL + 0.5 * m.aBefore * tL * tL;
    double actual = h.sample(i + 4).y;
    double dv = std::fabs(m.vyAfter - m.vyBefore);
    double da = std::fabs(m.aAfter - m.aBefore);
    // Changed if the path bends away, or the speed/acceleration clearly jumped.
    m.changed = m.teleport || std::fabs(actual - predicted) > 1.5
                || dv > 0.15 * m.vx || da > 40.0;

    m.valid = true;
    return m;
}

// Counterfactual vertical position: where the player would be at absolute time `t` if the click had
// happened `delta` seconds later (delta > 0) or earlier (delta < 0).
inline double counterfactualY(Motion const& m, double t, double delta) {
    double ts = m.tc + delta; // the shifted click moment
    if (t <= ts) {
        double u = t - m.tc; // still doing what it did before the click
        return m.yc + m.vyBefore * u + 0.5 * m.aBefore * u * u;
    }
    double ySwitch = m.teleport ? m.ySwitch
                                : m.yc + m.vyBefore * delta + 0.5 * m.aBefore * delta * delta;
    double tau = t - ts;
    return ySwitch + m.vyAfter * tau + 0.5 * m.aAfter * tau * tau;
}

// ---------------------------------------------------------------------------------------------
// The ghost: fly the counterfactual motion forward and see if it clears everything up to xEnd.
// ---------------------------------------------------------------------------------------------
inline bool ghostSurvives(std::vector<Shape const*> const& shapes, Motion const& m,
                          double half, double xEnd, double delta, double step,
                          double horizonTime = 1e30) {
    double tStart = m.tc + std::min(delta, 0.0);
    double t = tStart;
    double x = m.xc + m.vx * (tStart - m.tc);
    double y = counterfactualY(m, t, delta);
    double yPrev = y;

    for (int guard = 0; x < xEnd && t < horizonTime && guard < 20000; guard++) {
        double tn = t + step;

        // Move forward. Touching ANYTHING from the side is death.
        double nx = x + m.vx * step;
        for (Shape const* s : shapes) {
            if (overlaps(*s, nx, y, half)) return false;
        }
        x = nx;

        // Move up/down by as much as the counterfactual motion wants this step. Hazards kill,
        // blocks stop the vertical movement (the player slides along them).
        double yWant = counterfactualY(m, tn, delta);
        double ny = y + (yWant - yPrev);
        yPrev = yWant;
        bool blocked = false;
        for (Shape const* s : shapes) {
            if (!overlaps(*s, x, ny, half)) continue;
            if (!s->solid) return false;
            blocked = true;
        }
        if (!blocked) y = ny;

        t = tn;
    }
    return true;
}

struct Window {
    double seconds = 0.0;   // length of the window
    double early = 0.0;     // earliest surviving click (relative to the real click, seconds)
    double late = 0.0;      // latest surviving click
    bool capped = false;    // hit the search limit: the real window is at least this big
};

// Finds the range of click times that survive. Assumes that range is one piece.
inline Window measureWindow(std::vector<Shape const*> const& shapes, Motion const& m,
                            double half, double xEnd, double maxShift, double step,
                            double horizonTime = 1e30) {
    Window win;
    double maxEarly = maxShift;

    auto ok = [&](double delta) { return ghostSurvives(shapes, m, half, xEnd, delta, step, horizonTime); };

    // 1. Find any click time that survives, starting with the real one.
    double seed = 0.0;
    bool found = ok(0.0);
    for (int i = 1; !found && i <= 24; i++) {
        double later = maxShift * i / 24.0;
        double earlier = -maxEarly * i / 24.0;
        if (ok(later)) { seed = later; found = true; }
        else if (maxEarly > 0.0 && ok(earlier)) { seed = earlier; found = true; }
    }
    if (!found) return win; // nothing survives: window of 0

    // 2. Walk outwards from it to find both edges (binary search, ~0.015 ms resolution).
    if (ok(maxShift)) { win.late = maxShift; win.capped = true; }
    else {
        double good = seed, bad = maxShift;
        for (int k = 0; k < 12; k++) { double mid = 0.5 * (good + bad); (ok(mid) ? good : bad) = mid; }
        win.late = good;
    }

    if (maxEarly <= 0.0) { win.early = std::min(seed, 0.0); }
    else if (ok(-maxEarly)) { win.early = -maxEarly; win.capped = true; }
    else {
        double good = seed, bad = -maxEarly;
        for (int k = 0; k < 12; k++) { double mid = 0.5 * (good + bad); (ok(mid) ? good : bad) = mid; }
        win.early = good;
    }

    win.seconds = std::max(0.0, win.late - win.early);
    return win;
}

// ---------------------------------------------------------------------------------------------
// One click, start to finish.
// ---------------------------------------------------------------------------------------------
struct ClickResult {
    bool counted = false;   // false: ignore this click (no hazard close ahead, nothing changed, ...)
    double seconds = 0.0;   // the window
    double early = 0.0, late = 0.0; // window edges relative to the real click (diagnostics)
    double reach = 0.0;     // how far ahead (units) the ghost had to survive
    bool capped = false;
    // Why it was not counted (for the diagnostic log): 0 counted, 1 motion invalid (not enough
    // history / not moving forward), 2 nothing changed (press ignored), 3 no hazard close ahead, 4 nothing limits the click.
    int reason = 1;
    int nearby = 0;         // how many hazards/blocks were within reach
};

struct MeasureSettings {
    double half = 5.0;            // half the player's hitbox size
    double hazardDistance = 60.0; // a hazard must be this close ahead (2 blocks) or the click is ignored
    double lookAhead = 300.0;     // how far ahead (units) the ghost has to survive at most
    double maxShift = 0.06;       // how far from the real click we search (seconds, each way)
    double stepUnits = 1.5;       // ghost step, in units of forward travel (coarser = faster)
};

// `requireChange`: for a press, we skip clicks that changed nothing (buffered / airborne press).
// For a release of a hold gamemode (ship/robot/wave/swing) the change can be a small bend, so the
// caller passes false and we trust that it mattered.
inline ClickResult measureClick(ShapeIndex const& index, History const& h, double clickTime,
                                bool requireChange, MeasureSettings const& cfg,
                                double horizonTime = 1e30) {
    ClickResult result;
    Motion m = motionFrom(h, clickTime);
    if (!m.valid) { result.reason = 1; return result; }
    if (requireChange && !m.changed) { result.reason = 2; return result; } // press that did nothing

    // How far ahead the ghost should care about. For a jump (the motion arcs back - acceleration
    // opposes the launch speed), only until it lands again; a hazard beyond that is cleared by a
    // LATER click, not this one, so counting it would make the window read far too small or large.
    // For a flat glide (wave/ship, little acceleration) we use the full look-ahead, and the
    // horizonTime (the next real click) keeps it honest.
    double reachDist = cfg.lookAhead;
    if (m.aAfter != 0.0 && (m.vyAfter / m.aAfter) < 0.0) {       // launches up/down then comes back
        double tLand = -2.0 * m.vyAfter / m.aAfter;             // time to return to launch height
        reachDist = std::min(cfg.lookAhead, std::max(50.0, m.vx * tLand * 1.15));
    }
    double xEnd = m.xc + reachDist;

    // Everything around the player that could matter.
    std::vector<Shape const*> around;
    index.query(m.xc - 120.0, m.yc - 600.0, m.xc + reachDist + 100.0, m.yc + 600.0, around);
    result.nearby = static_cast<int>(around.size());

    // Hazards that count: close (distance <= hazardDistance) and not already behind us.
    std::vector<Shape const*> close;
    for (Shape const* s : around) {
        double dx = std::max({s->minX - m.xc, 0.0, m.xc - s->maxX});
        double dy = std::max({s->minY - m.yc, 0.0, m.yc - s->maxY});
        if (std::hypot(dx, dy) <= cfg.hazardDistance && s->maxX + cfg.half > m.xc) close.push_back(s);
    }
    if (close.empty()) { result.reason = 3; return result; } // regular spam with nothing close

    double step = std::max(0.00015, cfg.stepUnits / m.vx); // fixed travel per step -> cheap and stable

    // Only the shapes the ghost can actually reach matter (the rest only slow it down).
    std::vector<Shape const*> path;
    double back = cfg.maxShift * m.vx + 20.0;
    for (Shape const* s : around) {
        if (s->maxX < m.xc - back || s->minX > xEnd + cfg.half + 1.0) continue;
        if (s->maxY < m.yc - 320.0 || s->minY > m.yc + 320.0) continue;
        path.push_back(s);
    }

    Window win = measureWindow(path, m, cfg.half, xEnd, cfg.maxShift, step, horizonTime);
    // Nothing survived, even though the player really did click and (usually) live: my model is a
    // little too strict about the hitbox. Try again with a slightly smaller hitbox before giving up
    // and calling it a true 0. This is what made normal clicks show up as "0 frames".
    if (win.seconds <= 0.0) {
        for (double shrink : {0.85, 0.7}) {
            Window retry = measureWindow(path, m, cfg.half * shrink, xEnd, cfg.maxShift, step, horizonTime);
            if (retry.seconds > 0.0) { win = retry; break; }
        }
    }
    // The ghost survived the whole search range both ways: nothing the mod can see limits this click
    // (typically slopes, the floor/ceiling, or objects it does not model). Showing the search limit
    // as if it were a real window is misleading, so such a click is not counted (reason 4).
    double maxEarlyAllowed = cfg.maxShift;
    if (win.capped && win.early <= -maxEarlyAllowed + 1e-9 && win.late >= cfg.maxShift - 1e-9) {
        result.reason = 4;
        result.nearby = result.nearby;
        return result;
    }
    result.reason = 0;
    result.counted = true;
    result.seconds = win.seconds;
    result.early = win.early;
    result.late = win.late;
    result.reach = reachDist;
    result.capped = win.capped;
    return result;
}

} // namespace fwc
