#pragma once

// Real window measuring for the WAVE. Pure geometry: no Geometry Dash code in here, so it can be
// tested on any computer (see tests/test_measure.cpp).
//
// The idea ("ghost wave"):
//   When you click, we take the wave's position and speed from the last few physics ticks and ask:
//   "If the click had happened a bit earlier or a bit later (and nothing else changed), would the
//    wave still get past the hazards in front of it?"
//   The range of click times that still survive is the window. Its length in seconds is the time
//   you had to click (ms); times the TPS gives frames.
//
// Simplifications (v0.2): only the wave, only hazards and blocks that stay where they are (no
// moving objects), slopes are ignored, only the hazards right ahead are checked (not future clicks).

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
    bool solid = false;        // block: the wave slides along top/bottom. Hazard: any touch kills.

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
};

// Does the wave's square hitbox (centre cx,cy, half-size `half`) overlap the shape?
// The square is shrunk by a hair so just touching a surface is not a hit.
inline bool overlaps(Shape const& s, double cx, double cy, double half) {
    double e = std::max(half - 0.01, half * 0.5);
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
// History: where the player was on the last few physics ticks.
// ---------------------------------------------------------------------------------------------
struct Sample {
    double t = 0, x = 0, y = 0;
};

class History {
public:
    void clear() { m_samples.clear(); m_time = 0.0; }
    size_t size() const { return m_samples.size(); }

    // Call once per physics tick with the tick length and the player's position after the tick.
    void push(double dt, double x, double y) {
        if (!(dt > 0.0)) return;
        m_time += dt;
        m_samples.push_back({m_time, x, y});
        while (m_samples.size() > 2 && m_samples.back().t - m_samples.front().t > kKeepSeconds) {
            m_samples.pop_front();
        }
    }

    Sample const& last() const { return m_samples.back(); }
    Sample const& at(size_t fromEnd) const { return m_samples[m_samples.size() - 1 - fromEnd]; }
    double span() const { return m_samples.size() < 2 ? 0.0 : m_samples.back().t - m_samples.front().t; }

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

    static constexpr double kKeepSeconds = 0.25;

private:
    std::deque<Sample> m_samples;
    double m_time = 0.0;
};

// ---------------------------------------------------------------------------------------------
// The wave's motion at the moment of a click.
// ---------------------------------------------------------------------------------------------
struct WaveInfo {
    bool valid = false;
    double x0 = 0, y0 = 0;   // position at the click
    double vx = 0;           // horizontal speed (units per second)
    double vyBefore = 0;     // vertical speed just before the click (signed; 0 when sliding flat)
    double slope = 0;        // size of the wave's vertical speed (units per second)
};

inline WaveInfo waveInfo(History const& h, bool mini) {
    WaveInfo w;
    if (h.size() < 2) return w;

    Sample const& a = h.at(1);
    Sample const& b = h.at(0);
    double dt = b.t - a.t;
    if (!(dt > 0.0)) return w;

    w.x0 = b.x;
    w.y0 = b.y;
    w.vx = (b.x - a.x) / dt;
    w.vyBefore = (b.y - a.y) / dt;
    if (!(w.vx > 1.0)) return w; // not moving forward: nothing to measure

    // The wave's vertical speed is the biggest we saw lately (it is smaller on the ticks where
    // the wave slid along a block).
    double best = 0.0;
    size_t n = std::min<size_t>(h.size() - 1, 12);
    for (size_t i = 0; i < n; i++) {
        Sample const& p = h.at(i + 1);
        Sample const& q = h.at(i);
        double d = q.t - p.t;
        if (d > 0.0) best = std::max(best, std::fabs((q.y - p.y) / d));
    }
    // Nothing to learn from yet (always flat): normal wave is 45 degrees, mini is steeper.
    if (best < 1e-3) best = w.vx * (mini ? 2.0 : 1.0);
    w.slope = best;

    w.valid = true;
    return w;
}

// ---------------------------------------------------------------------------------------------
// The ghost wave.
// ---------------------------------------------------------------------------------------------

// Would the wave survive if the click had happened `delta` seconds later (delta > 0) or
// earlier (delta < 0) than it really did? It flies on until it has passed x = xEnd.
inline bool ghostSurvives(std::vector<Shape const*> const& shapes, History const& h, WaveInfo const& w,
                          double half, double vyAfter, double xEnd, double delta, double step) {
    double x, y;
    double timeInOldDirection;
    if (delta >= 0.0) {
        x = w.x0; y = w.y0;
        timeInOldDirection = delta;
    }
    else {
        Vec p = h.positionAt(h.last().t + delta);
        x = p.x; y = p.y;
        timeInOldDirection = 0.0;
    }

    // Hard stop so a bad input can never loop forever.
    for (int guard = 0; x < xEnd && guard < 20000; guard++) {
        double dt = step;
        double vy = vyAfter;
        if (timeInOldDirection > 1e-12) {
            dt = std::min(step, timeInOldDirection);
            timeInOldDirection -= dt;
            vy = w.vyBefore;
        }

        // Move forward. Touching ANYTHING from the side is death.
        double nx = x + w.vx * dt;
        for (Shape const* s : shapes) {
            if (overlaps(*s, nx, y, half)) return false;
        }
        x = nx;

        // Move up/down. Hazards kill, blocks stop the movement (the wave slides along them).
        double ny = y + vy * dt;
        bool blocked = false;
        for (Shape const* s : shapes) {
            if (!overlaps(*s, x, ny, half)) continue;
            if (!s->solid) return false;
            blocked = true;
        }
        if (!blocked) y = ny;
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
inline Window measureWindow(std::vector<Shape const*> const& shapes, History const& h, WaveInfo const& w,
                            double half, double vyAfter, double xEnd, double maxShift, double step) {
    Window win;
    double maxEarly = std::min(maxShift, h.span());   // we can only look back as far as we remember

    auto ok = [&](double delta) { return ghostSurvives(shapes, h, w, half, vyAfter, xEnd, delta, step); };

    // The search for a first surviving click time can be a bit coarser (it only has to find ANY).
    auto okCoarse = [&](double delta) { return ghostSurvives(shapes, h, w, half, vyAfter, xEnd, delta, step * 2.0); };

    // 1. Find any click time that survives, starting with the real one.
    double seed = 0.0;
    bool found = ok(0.0);
    for (int i = 1; !found && i <= 40; i++) {
        double later = maxShift * i / 40.0;
        double earlier = -maxEarly * i / 40.0;
        if (okCoarse(later) && ok(later)) { seed = later; found = true; }
        else if (maxEarly > 0.0 && okCoarse(earlier) && ok(earlier)) { seed = earlier; found = true; }
    }
    if (!found) return win; // nothing survives: window of 0

    // 2. Walk outwards from it to find both edges.
    double late;
    if (ok(maxShift)) { late = maxShift; win.capped = true; }
    else {
        double good = seed, bad = maxShift;
        for (int i = 0; i < 30; i++) {
            double mid = 0.5 * (good + bad);
            (ok(mid) ? good : bad) = mid;
        }
        late = good;
    }

    double early;
    if (maxEarly <= 0.0) { early = std::min(seed, 0.0); }
    else if (ok(-maxEarly)) { early = -maxEarly; win.capped = true; }
    else {
        double good = seed, bad = -maxEarly;
        for (int i = 0; i < 30; i++) {
            double mid = 0.5 * (good + bad);
            (ok(mid) ? good : bad) = mid;
        }
        early = good;
    }

    win.early = early;
    win.late = late;
    win.seconds = std::max(0.0, late - early);
    return win;
}

// ---------------------------------------------------------------------------------------------
// One click, start to finish.
// ---------------------------------------------------------------------------------------------
struct ClickResult {
    bool counted = false;   // false: ignore this click (no hazard close ahead, not a turn, ...)
    double seconds = 0.0;   // the window
    bool capped = false;
};

struct MeasureSettings {
    double half = 5.0;          // half the wave's hitbox size
    double hazardDistance = 60.0; // a hazard must be this close ahead (2 blocks) or the click is ignored
    double lookAhead = 150.0;   // how far ahead (units) the ghost has to survive at most
    double maxShift = 0.06;     // how far from the real click we search (seconds, each way)
    double step = 0.0005;       // ghost simulation step (seconds)
};

inline ClickResult measureClick(ShapeIndex const& index, History const& h, bool pressedDown,
                                bool upsideDown, bool mini, MeasureSettings const& cfg) {
    ClickResult result;
    WaveInfo w = waveInfo(h, mini);
    if (!w.valid) return result;

    // Holding sends the wave up (down when gravity is flipped), letting go sends it down.
    double sign = pressedDown ? 1.0 : -1.0;
    if (upsideDown) sign = -sign;
    double vyAfter = sign * w.slope;
    if (w.vyBefore * vyAfter > 0.0) return result; // already going that way: not a turn

    // Everything around the wave that could matter.
    std::vector<Shape const*> around;
    index.query(w.x0 - 120.0, w.y0 - 500.0, w.x0 + cfg.lookAhead + 100.0, w.y0 + 500.0, around);

    // Hazards that count: close (distance <= hazardDistance) and not already behind us.
    std::vector<Shape const*> close;
    for (Shape const* s : around) {
        double dx = std::max({s->minX - w.x0, 0.0, w.x0 - s->maxX});
        double dy = std::max({s->minY - w.y0, 0.0, w.y0 - s->maxY});
        if (std::hypot(dx, dy) <= cfg.hazardDistance && s->maxX + cfg.half > w.x0) close.push_back(s);
    }
    if (close.empty()) return result; // regular spam with nothing close: not counted

    // The ghost has to get past the closest "wall" of hazards (all that sit at the same spot).
    double firstKey = std::numeric_limits<double>::infinity();
    for (Shape const* s : close) firstKey = std::min(firstKey, std::max(s->minX, w.x0 - cfg.half));
    double wallEnd = -std::numeric_limits<double>::infinity();
    for (Shape const* s : close) {
        if (std::max(s->minX, w.x0 - cfg.half) <= firstKey + 1.0) wallEnd = std::max(wallEnd, s->maxX);
    }
    for (Shape const* s : close) { // anything else that overlaps the wall's width belongs to it
        if (s->minX <= wallEnd) wallEnd = std::max(wallEnd, s->maxX);
    }
    double xEnd = std::min(wallEnd + cfg.half, w.x0 + cfg.lookAhead);

    Window win = measureWindow(around, h, w, cfg.half, vyAfter, xEnd, cfg.maxShift, cfg.step);
    result.counted = true;
    result.seconds = win.seconds;
    result.capped = win.capped;
    return result;
}

} // namespace fwc
