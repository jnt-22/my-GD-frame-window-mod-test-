// Quick check of the counting logic. Build and run on any computer:
//   g++ -std=c++20 -Isrc tests/test_stats.cpp -o /tmp/test_stats && /tmp/test_stats
#include <cstdio>
#include <cmath>
#include "WindowStats.hpp"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main() {
    using namespace fwc;

    // Bucket edges (lower edge included).
    CHECK(bucketForFrames(0.0) == 8);
    CHECK(bucketForFrames(0.99) == 8);
    CHECK(bucketForFrames(1.0) == 7);
    CHECK(bucketForFrames(1.99) == 7);
    CHECK(bucketForFrames(2.0) == 6);
    CHECK(bucketForFrames(3.5) == 5);
    CHECK(bucketForFrames(4.0) == 4);
    CHECK(bucketForFrames(5.99) == 3);
    CHECK(bucketForFrames(6.0) == 2);
    CHECK(bucketForFrames(7.99) == 2);
    CHECK(bucketForFrames(8.0) == 1);
    CHECK(bucketForFrames(9.99) == 1);
    CHECK(bucketForFrames(10.0) == 0);
    CHECK(bucketForFrames(500.0) == 0);

    // Your examples: frames -> time required -> frame-perfect Hz.
    CHECK(formatMs(framesToMs(10, 240)) == "41.67MS");
    CHECK(formatFpHz(framesToMs(10, 240)) == "24Hz");
    CHECK(formatFpHz(framesToMs(10, 480)) == "48Hz");
    CHECK(formatMs(framesToMs(10, 480)) == "20.83MS");
    CHECK(formatMs(framesToMs(9, 240)) == "37.50MS");
    CHECK(formatFpHz(framesToMs(9, 240)) == "27~Hz");   // 26.67
    CHECK(formatFpHz(framesToMs(9, 480)) == "53~Hz");   // 53.33
    CHECK(formatFpHz(framesToMs(1, 240)) == "240Hz");
    CHECK(formatFpHz(95.83) == "10~Hz");                // screenshot value, 10.43
    // Video-style popup lines (frames + ms, trailing zeros removed).
    CHECK(trimNumber(4.0) == "4");
    CHECK(trimNumber(13.80) == "13.8");
    CHECK(trimNumber(16.6667) == "16.67");
    CHECK(formatFramesLine(4.0) == "4 FRAMES");
    CHECK(formatMsLine(framesToMs(4.0, 240)) == "16.67 MS");
    CHECK(formatFramesLine(2.1875) == "2.19 FRAMES");
    CHECK(formatMsLine(framesToMs(3.3125, 240)) == "13.8 MS");    // video: 3.31 FRAMES / 13.8 MS
    CHECK(formatMsLine(framesToMs(3.125, 240)) == "13.02 MS");    // video: 3.13 FRAMES / 13.02 MS
    // A window of 0 (no click time would have worked) is counted in the 0-1 row.
    CHECK(formatFpHz(0.0) == "--Hz");
    CHECK(formatMsLine(0.0) == "0 MS");
    CHECK(formatFramesLine(0.0) == "0 FRAMES");
    {
        Stats z;
        z.add(0.0, 240.0);
        CHECK(z.hasData && z.counts[8] == 1);
    }
    // Round trip.
    CHECK(std::fabs(msToFrames(framesToMs(7, 240), 240) - 7.0) < 1e-9);
    // Same click, different TPS: 9 frames @240 is 18 frames @480 -> different bucket.
    CHECK(bucketForFrames(msToFrames(framesToMs(9, 240), 240)) == 1);   // 8-10
    CHECK(bucketForFrames(msToFrames(framesToMs(9, 240), 480)) == 0);   // 10+

    // Stats: counting, hardest window, reset.
    Stats s;
    CHECK(!s.hasData);
    s.add(framesToMs(6, 240), 240);   // 6 frames  -> row 2
    s.add(framesToMs(12, 240), 240);  // 12 frames -> row 0
    s.add(framesToMs(3, 240), 240);   // 3 frames  -> row 5 (latest click)
    CHECK(s.counts[2] == 1 && s.counts[0] == 1 && s.counts[5] == 1);
    CHECK(s.hasData);
    CHECK(std::fabs(s.shownMs(false) - framesToMs(3, 240)) < 1e-9);  // latest
    s.add(framesToMs(8, 240), 240);   // 8 frames -> row 1, no longer the latest-smallest
    CHECK(std::fabs(s.shownMs(false) - framesToMs(8, 240)) < 1e-9);
    CHECK(std::fabs(s.shownMs(true) - framesToMs(3, 240)) < 1e-9);   // hardest
    s.add(-5.0, 60.0);   // garbage is ignored
    s.add(10.0, 0.0);
    int total = 0;
    for (int c : s.counts) total += c;
    CHECK(total == 4);
    s.reset();
    CHECK(!s.hasData);
    total = 0;
    for (int c : s.counts) total += c;
    CHECK(total == 0);

    if (failures == 0) std::printf("All checks passed.\n");
    return failures == 0 ? 0 : 1;
}
