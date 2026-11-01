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
    // Average length of one recorded step (seconds); 0 when there are fewer than two samples.
    double meanDt() const {
        if (m_samples.size() < 2) return 0.0;
        return (m_samples.back().t - m_samples.front().t) / static_cast<double>(m_samples.size() - 1);
    }
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

    static constexpr double kKeepSeconds = 0.85;

private:
    std::deque<Sample> m_samples;
    double m_time = 0.0;
    long m_ticks = 0;
};


// =============================================================================================
// Window measuring by the REAL per-frame step.
//
// Geometry Dash moves the player a FIXED horizontal distance every physics tick (the "step"), e.g.
// about 1.3 units per tick at normal speed and 240 TPS. A click only matters through WHERE the
// player is when it happens, and the jump/flip it produces is always the same shape. So "clicking
// one frame earlier/later" is the same as taking the player's REAL recorded path and sliding it by
// exactly one step. We slide the real path by whole steps (-K .. +K), check which slid paths still
// clear the hazards, and the count of surviving steps IS the window in frames. Nothing is guessed:
// the path and the step both come straight from the recording, so the same gap always gives the
// same whole number.
// =============================================================================================

struct Trajectory {
    bool valid = false;
    bool changed = false;        // did the click actually change the motion? (buffered press -> false)
    double tc = 0, xc = 0, yc = 0;
    double dx = 0;               // horizontal step per tick (units) - the "one frame" distance
    double vyPre = 0;            // vertical step per tick just before the click
    std::vector<Vec> arc;        // real positions: arc[0] at the click, arc[j] j ticks after
};

// Read the real path around the click out of the recording.
inline Trajectory trajectoryFrom(History const& h, double clickTime, double lookAhead) {
    Trajectory t;
    int n = static_cast<int>(h.size());
    if (n < 6) return t;
    int c = h.indexAt(clickTime);
    if (c < 2 || c > n - 3) return t;          // need a little before and some after

    t.tc = h.sample(c).t; t.xc = h.sample(c).x; t.yc = h.sample(c).y;

    // Horizontal step: the median of the per-tick x advances (constant, so this is robust).
    std::vector<double> dxs;
    for (int k = std::max(1, c - 4); k < n; k++) {
        double d = h.sample(k).x - h.sample(k - 1).x;
        if (d > 0.0) dxs.push_back(d);
    }
    if (dxs.empty()) return t;
    std::sort(dxs.begin(), dxs.end());
    t.dx = dxs[dxs.size() / 2];
    if (!(t.dx > 0.1)) return t;               // not moving forward: nothing to measure

    // Vertical step just before the click (where the player would drift if it clicked later).
    t.vyPre = h.sample(c).y - h.sample(c - 1).y;

    // The real path from the click forward, one point per tick, until it has gone `lookAhead` ahead.
    t.arc.push_back({t.xc, t.yc});
    for (int k = c + 1; k < n; k++) {
        Vec p{h.sample(k).x, h.sample(k).y};
        t.arc.push_back(p);
        if (p.x - t.xc > lookAhead) break;
    }
    if (t.arc.size() < 3) return t;

    // Stop the path at the landing: once the player comes back to (roughly) where it launched and
    // flattens out, this click's influence is over and anything beyond is the NEXT click's business.
    // (For hold modes - ship/wave - there is no landing and the path just runs to lookAhead.)
    double vy0 = t.arc[1].y - t.arc[0].y;      // vertical step right after the click
    if (std::fabs(vy0) > 0.05) {
        int sign = vy0 > 0 ? 1 : -1;           // which way the jump first went
        for (size_t j = 2; j + 1 < t.arc.size(); j++) {
            double back = (t.arc[j].y - t.yc) * sign;      // how far above the launch, in jump dir
            double stepY = (t.arc[j + 1].y - t.arc[j].y) * sign;
            if (back <= 0.5 && stepY <= 0.05) {            // returned to launch height and flattened
                t.arc.resize(j + 1);
                break;
            }
        }
    }

    // Did the click change the motion? A real jump snaps the vertical step; a buffered press that did
    // nothing just keeps the pre-click drift.
    double vyAfter = t.arc[1].y - t.arc[0].y;
    t.changed = std::fabs(vyAfter - t.vyPre) > 0.6;        // > ~0.6 units/tick snap

    t.valid = true;
    return t;
}

// Does the real path, slid by K whole steps, clear every shape? (K>0 = clicking later = path slides
// forward/along the pre-click drift; K<0 = earlier.)
inline bool arcSurvivesShift(std::vector<Shape const*> const& shapes, Trajectory const& t,
                             double half, int K) {
    double ox = K * t.dx, oy = K * t.vyPre;
    // Sub-steps between path points so thin spikes between two ticks are not skipped.
    int sub = std::max(1, static_cast<int>(std::ceil(t.dx / 3.0)));
    Vec prev{t.arc[0].x + ox, t.arc[0].y + oy};
    for (size_t j = 0; j < t.arc.size(); j++) {
        Vec cur{t.arc[j].x + ox, t.arc[j].y + oy};
        for (int s = 1; s <= sub; s++) {
            double f = static_cast<double>(s) / sub;
            double x = prev.x + (cur.x - prev.x) * f;
            double y = prev.y + (cur.y - prev.y) * f;
            for (Shape const* sh : shapes) {
                if (!overlaps(*sh, x, y, half)) continue;
                if (sh->solid) return false;   // crashing into a block face kills you
                return false;                  // any hazard touch kills you
            }
        }
        prev = cur;
    }
    return true;
}

struct ClickResult {
    bool counted = false;   // false: ignore this click (no hazard close ahead, nothing changed, ...)
    double seconds = 0.0;   // the window (frames * one tick)
    double early = 0.0, late = 0.0; // window edges in frames (diagnostics): e.g. -3 .. +4
    double reach = 0.0;     // how far ahead (units) the path was followed
    bool capped = false;    // the window is at least this big (hit the search limit)
    // Why it was not counted (for the diagnostic log): 0 counted, 1 motion invalid (not enough
    // history / not moving forward), 2 nothing changed (buffered press), 3 no hazard close ahead,
    // 5 the model disagrees with the game (the real click is not inside the measured window).
    int reason = 1;
    int nearby = 0;         // how many hazards/blocks were around
    double dx = 0;          // the per-frame step used (diagnostics)
};

struct MeasureSettings {
    double half = 5.0;            // half the player's hitbox size
    double hazardDistance = 60.0; // a hazard must be this close ahead (2 blocks) or the click is ignored
    double lookAhead = 150.0;     // how far ahead (units) the path is followed at most
    int    maxSteps = 16;         // how many frames each way we slide (window caps near here)
};

// `requireChange`: for a press, skip clicks that changed nothing (buffered / airborne press). For a
// release of a hold gamemode (ship/wave/robot/swing) the caller passes false (a release is trusted).
inline ClickResult measureClick(ShapeIndex const& index, History const& h, double clickTime,
                                bool requireChange, MeasureSettings const& cfg) {
    ClickResult result;
    Trajectory t = trajectoryFrom(h, clickTime, cfg.lookAhead);
    if (!t.valid) { result.reason = 1; return result; }
    result.dx = t.dx;
    if (requireChange && !t.changed) { result.reason = 2; return result; }

    double reach = t.arc.back().x - t.xc;
    result.reach = reach;

    // Everything around the path that could matter.
    std::vector<Shape const*> around;
    double slide = cfg.maxSteps * t.dx + 10.0;
    index.query(t.xc - slide - 40.0, t.yc - 400.0, t.xc + reach + slide + 40.0, t.yc + 400.0, around);
    result.nearby = static_cast<int>(around.size());

    // A hazard must be close ahead (within hazardDistance) or this is just spam with nothing near.
    bool haveClose = false;
    for (Shape const* s : around) {
        double ddx = std::max({s->minX - t.xc, 0.0, t.xc - s->maxX});
        double ddy = std::max({s->minY - t.yc, 0.0, t.yc - s->maxY});
        if (std::hypot(ddx, ddy) <= cfg.hazardDistance && s->maxX + cfg.half > t.xc) { haveClose = true; break; }
    }
    if (!haveClose) { result.reason = 3; return result; }

    // Only the shapes the (slid) path can actually reach.
    std::vector<Shape const*> path;
    for (Shape const* s : around) {
        if (s->maxX < t.xc - slide || s->minX > t.xc + reach + slide) continue;
        if (s->maxY < t.yc - 360.0 || s->minY > t.yc + 360.0) continue;
        path.push_back(s);
    }

    auto survives = [&](double half, int K) { return arcSurvivesShift(path, t, half, K); };

    // Find the run of surviving shifts around 0. The real click survived, so K=0 should survive; if
    // my hitbox is a touch too big it might not, so shrink it until K=0 works (as the game says it must).
    double half = cfg.half;
    if (!survives(half, 0)) {
        bool fixed = false;
        for (double shrink : {0.85, 0.7, 0.55, 0.4}) {
            if (survives(cfg.half * shrink, 0)) { half = cfg.half * shrink; fixed = true; break; }
        }
        if (!fixed) { result.reason = 5; return result; } // model disagrees with the game: skip
    }

    int hi = 0, lo = 0;
    while (hi < cfg.maxSteps && survives(half, hi + 1)) hi++;
    while (lo > -cfg.maxSteps && survives(half, lo - 1)) lo--;

    int frames = hi - lo + 1;
    result.counted = true;
    result.reason = 0;
    result.early = lo;
    result.late = hi;
    result.capped = (hi == cfg.maxSteps) || (lo == -cfg.maxSteps);
    // One tick of time per frame of window, from the real step and the recorded tick length.
    double dtick = h.meanDt();
    if (!(dtick > 0.0005 && dtick < 0.05)) dtick = 1.0 / 240.0;
    result.seconds = frames * dtick;
    return result;
}

} // namespace fwc
