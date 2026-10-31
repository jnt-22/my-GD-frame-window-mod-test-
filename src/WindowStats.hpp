#pragma once

// Pure logic: no Geometry Dash code in here, so it can be tested on any computer.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace fwc {

constexpr int kBucketCount = 9;

// Row labels, top to bottom, exactly as they appear on screen.
constexpr std::array<const char*, kBucketCount> kBucketLabels = {
    "10+:", "8-10:", "6-8:", "5-6:", "4-5:", "3-4:", "2-3:", "1-2:", "0-1:"
};

// Rainbow colors, same order as the labels. Used by the counter rows AND by the circles
// drawn on the level, so a click's circle always matches its row.
struct Rgb { unsigned char r, g, b; };
constexpr std::array<Rgb, kBucketCount> kBucketColors = {{
    {192, 96, 255},  // 10+
    {140, 110, 255}, // 8-10
    {80, 140, 255},  // 6-8
    {80, 200, 255},  // 5-6
    {120, 230, 100}, // 4-5
    {255, 230, 80},  // 3-4
    {255, 160, 60},  // 2-3
    {255, 100, 80},  // 1-2
    {255, 100, 150}, // 0-1
}};

// Which row a window of `frames` frames belongs to.
// Lower edge is included: exactly 8 frames is "8-10", exactly 1 frame is "1-2".
inline int bucketForFrames(double frames) {
    if (frames >= 10.0) return 0;
    if (frames >= 8.0)  return 1;
    if (frames >= 6.0)  return 2;
    if (frames >= 5.0)  return 3;
    if (frames >= 4.0)  return 4;
    if (frames >= 3.0)  return 5;
    if (frames >= 2.0)  return 6;
    if (frames >= 1.0)  return 7;
    return 8;
}

// A window of `frames` frames at `tps` ticks per second lasts this many milliseconds.
//   10 frames @ 240 TPS = 41.67 ms,   10 frames @ 480 TPS = 20.83 ms
inline double framesToMs(double frames, double tps) {
    return tps > 0.0 ? frames * 1000.0 / tps : 0.0;
}

// The other way round: a window of `windowMs` at `tps` ticks per second is this many frames.
inline double msToFrames(double windowMs, double tps) {
    return windowMs * tps / 1000.0;
}

// The tick rate at which a window of `windowMs` is exactly 1 frame (frame perfect).
//   41.67 ms -> 24 Hz,   37.5 ms -> 26.67 Hz
inline double frameperfectHz(double windowMs) {
    return windowMs > 0.0 ? 1000.0 / windowMs : 0.0;
}

// Up to `decimals` decimals, trailing zeros removed: 4 -> "4", 13.80 -> "13.8", 16.6667 -> "16.67".
inline std::string trimNumber(double value, int decimals = 2) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    std::string text = buf;
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text;
}

// The two popup lines, in the style of your video: "4 FRAMES" and "16.67 MS".
inline std::string formatFramesLine(double frames) { return trimNumber(frames) + " FRAMES"; }
inline std::string formatMsLine(double windowMs)   { return trimNumber(windowMs) + " MS"; }

// "41.67MS"
inline std::string formatMs(double windowMs) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2fMS", windowMs);
    return buf;
}

// "24Hz" when it is a whole number, "27~Hz" when it had to be rounded.
inline std::string formatFpHz(double windowMs) {
    if (!(windowMs > 0.0)) return "--Hz"; // a window of 0 is never frame perfect at any rate
    double hz = frameperfectHz(windowMs);
    double rounded = std::round(hz);
    char buf[48];
    if (std::fabs(hz - rounded) < 0.05) std::snprintf(buf, sizeof(buf), "%.0fHz", rounded);
    else std::snprintf(buf, sizeof(buf), "%.0f~Hz", rounded);
    return buf;
}

// Everything the counter shows. Cleared at the start of every attempt.
struct Stats {
    std::array<int, kBucketCount> counts{};
    double lastMs = 0.0;                                         // window of the latest click
    double hardestMs = std::numeric_limits<double>::infinity();  // smallest window so far
    bool hasData = false;

    void reset() { *this = Stats{}; }

    // One click whose window lasted `windowMs`, counted at `tps` ticks per second.
    void add(double windowMs, double tps) {
        if (!(windowMs >= 0.0) || !(tps > 0.0)) return; // ignore garbage (a window of 0 is real: no timing works)
        counts[bucketForFrames(msToFrames(windowMs, tps))]++;
        lastMs = windowMs;
        hardestMs = std::min(hardestMs, windowMs);
        hasData = true;
    }

    double shownMs(bool hardest) const { return hardest ? hardestMs : lastMs; }
};

} // namespace fwc
