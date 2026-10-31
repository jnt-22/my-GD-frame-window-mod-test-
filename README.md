# Frame Window Counter (Geode, GD 2.2081)

A counter in the top-left corner of the screen that sorts your clicks into frame-window buckets
(`10+`, `8-10`, `6-8`, `5-6`, `4-5`, `3-4`, `2-3`, `1-2`, `0-1`) and **resets every attempt**.

On every click a popup appears under the buckets in the style of the reference video: `4 FRAMES` over
`16.67 MS` (optional third line: frame-perfect Hz). It stays 0.3 s, then fades out; the next click restarts it.
(Turn on "Keep popup until next click" to make it stay like in the video.)

Every click also draws a **circle with the frame number** on the level, exactly where the player was, in the
color of its row in the counter. The circles stay for the attempt and are cleared on the next one.
Everything depends on the **TPS** setting:

| Window | @ 240 TPS | @ 480 TPS |
|--------|-----------|-----------|
| 10 frames | `41.67MS` / `24Hz` | `20.83MS` / `48Hz` |
| 9 frames  | `37.50MS` / `27~Hz` | `18.75MS` / `53~Hz` |

(`~` means the Hz had to be rounded.)

## Status: v0.2 (real measuring, experimental)

* **Real, live measuring for the wave.** On every click the mod asks: "if this click had come a bit
  earlier or later, would the wave still get past the hazards in front of it?" The range of click
  times that still work is the window (ms), times the TPS is the frame count.
* **Only clicks with a hazard close ahead count.** A spike, saw or block must be at most 60 units
  (2 blocks) ahead of the wave ("Hazard distance" setting). Regular spam with nothing close is ignored.
* Both pressing and letting go count, because both turn the wave.
* Other gamemodes are not measured yet (nothing is counted outside the wave).
* Turn on **Demo data** to get fake random windows instead (only for testing the display).

### Known limits (v0.2)
* Only the hazards right ahead are checked, not your future clicks, so a window can be larger than
  what a long spam sequence really allows.
* Objects moved by triggers are treated as standing still. Slopes are ignored.
* The wave hitbox size (10 units, scaled for mini) is my best estimate: if your numbers differ from
  what you expect, change **Wave hitbox size**.

## How to build it without installing anything (GitHub Actions)

1. Make a free account on github.com and create a new empty repository.
2. Upload everything in this folder (keep the `.github` folder, it is the build recipe).
3. Open the **Actions** tab, pick **Build Geode Mod**, press **Run workflow** (it also runs on every upload).
4. When it finishes (a few minutes), open the run and download **Build Output**.
   Inside is `erik.frame-window-counter.geode`.
5. Put that file in your Geode `mods` folder (or install it from the Geode menu in-game) and restart GD.
   On Android the folder is usually `Android/media/com.geode.launcher/game/geode/mods`.

If a build fails, open the failed step, copy the red error text and send it to me.

## Settings

Enabled, Hazard distance (60), Count blocks as hazards, Wave hitbox size, Show counts (right-hand number column), TPS (240 normal, 480 double, any number works),
Show hardest click (popup shows the smallest window instead of the latest click), Show circles on the level, Show Hz line, Keep popup until next click, popup time shown (0.3 s) and fade time (0.2 s), Row spacing (13 default, smaller = more compact), Size, Opacity, Demo data.

## Files

* `src/WindowStats.hpp` – the counting logic (buckets, number formatting). Tests in `tests/test_stats.cpp`.
* `src/WindowMeasure.hpp` – the wave measuring (ghost wave, hazards, 60 unit rule). Tests in `tests/test_measure.cpp`.
* `src/Hud.hpp` – the rainbow box on screen.
* `src/main.cpp` – hooks into the game: adds the box, resets it each attempt, watches clicks.
* `mod.json` – the mod's name, GD version and settings.
