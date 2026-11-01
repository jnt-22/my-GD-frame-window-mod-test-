#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cmath>

#include <vector>

#include "Hud.hpp"
#include "WindowMeasure.hpp"
#include "WindowStats.hpp"

using namespace geode::prelude;

namespace {
    // Physics ticks per second (TPS) used to turn frames into milliseconds and back.
    // 240 is Geometry Dash's normal speed; set 480 to see how the same windows look at 480 TPS.
    double currentTps() {
        auto tps = Mod::get()->getSettingValue<int64_t>("tps");
        return tps > 0 ? static_cast<double>(tps) : 240.0;
    }

    // A click waiting to be measured. We measure a few ticks AFTER it so the motion the click
    // produced (the jump / flip / thrust) is already recorded.
    struct PendingClick {
        long tick = 0;          // history tick at the click
        double time = 0;        // history time at the click
        double half = 5.0;      // half the player's hitbox then (mini is smaller)
        bool requireChange = true; // presses must actually change the motion; releases are trusted
        CCPoint pos{0.f, 0.f};  // where the player was (for the circle)
    };

    // How many ticks to wait before measuring (the motion fit needs a handful of samples after).
    constexpr long kMeasureDelayTicks = 7;
}

// ---------------------------------------------------------------------------
// PlayLayer = the screen you play a level on.
// Owns the counter box and the numbers, and clears them every attempt.
// ---------------------------------------------------------------------------
class $modify(FWCPlayLayer, PlayLayer) {
    struct Fields {
        FrameWindowHUD* hud = nullptr;
        CCNode* markerLayer = nullptr; // holds the circles; lives inside the level so they scroll with it
        fwc::Stats stats;
        fwc::ShapeIndex shapes;        // every hazard and block of the level (built once)
        fwc::History history;          // where the player was on the last physics ticks
        std::vector<PendingClick> pending; // clicks waiting their few ticks before being measured
    };

    // Called once, when the level has finished loading.
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        auto hud = FrameWindowHUD::create();
        if (!hud) return;

        auto winSize = CCDirector::get()->getWinSize();
        hud->setPosition({8.f, winSize.height - 30.f}); // top-left corner
        hud->setID("hud"_spr);
        this->addChild(hud, 100);

        m_fields->hud = hud;

        // Layer for the click circles. It is inside the object layer (the part that scrolls),
        // so every circle stays exactly where the player was when you clicked.
        auto markers = CCNode::create();
        markers->setID("markers"_spr);
        m_objectLayer->addChild(markers, 1000);
        m_fields->markerLayer = markers;

        this->buildShapes();
        this->refreshHud();
    }

    // Reads every hazard and block of the level once, so clicks can be measured quickly later.
    // (Objects that move during the level, like ones moved by triggers, are treated as standing still.)
    void buildShapes() {
        auto& index = m_fields->shapes;
        index.clear();

        bool includeBlocks = Mod::get()->getSettingValue<bool>("include-blocks");
        size_t hazards = 0, blocks = 0;

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            if (obj->m_isNoTouch || obj->m_isDecoration || obj->m_isTrigger) continue;

            bool solid;
            if (obj->m_objectType == GameObjectType::Hazard) solid = false;
            else if (obj->m_objectType == GameObjectType::Solid && includeBlocks) solid = true;
            else continue; // slopes, portals, pads, decoration, ... are ignored

            // Rotated by something that is not a multiple of 90 degrees: use the rotated box.
            float rot = std::fmod(std::fabs(obj->getRotation()), 90.f);
            bool tilted = rot > 0.5f && rot < 89.5f;
            if (tilted) {
                if (auto box = obj->getOrientedBox()) {
                    std::array<fwc::Vec, 4> pts;
                    for (int i = 0; i < 4; i++) pts[i] = {box->m_corners[i].x, box->m_corners[i].y};
                    index.add(fwc::Shape::quad(pts, solid));
                    solid ? blocks++ : hazards++;
                    continue;
                }
            }

            CCRect const& r = obj->getObjectRect();
            if (r.size.width < 0.5f || r.size.height < 0.5f) continue; // no usable hitbox
            index.add(fwc::Shape::rect(r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), solid));
            solid ? blocks++ : hazards++;
        }

        log::info("Frame Window Counter: found {} hazards and {} blocks", hazards, blocks);
    }

    // Called every physics tick with the player's position (see the PlayerObject hook below).
    // Also the moment we measure any click that has now had enough ticks recorded after it.
    void pushSample(double dt, CCPoint pos) {
        m_fields->history.push(dt, pos.x, pos.y);
        this->measureReadyClicks();
    }

    // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing (only used for the log).
    static int p_mode(PlayerObject* p) {
        if (p->m_isShip) return 1;
        if (p->m_isBall) return 2;
        if (p->m_isBird) return 3;
        if (p->m_isDart) return 4;
        if (p->m_isRobot) return 5;
        if (p->m_isSpider) return 6;
        if (p->m_isSwing) return 7;
        return 0;
    }

    // Remember a click to measure a few ticks from now.
    void queueClick(bool requireChange) {
        auto player = m_player1;
        if (!player) return;
        auto& q = m_fields->pending;
        if (q.size() > 16) q.erase(q.begin()); // never let it grow without bound
        PendingClick pc;
        pc.tick = m_fields->history.ticks();
        pc.time = m_fields->history.now();
        pc.half = 0.5 * Mod::get()->getSettingValue<double>("wave-hitbox") * player->m_vehicleSize;
        pc.requireChange = requireChange;
        pc.pos = player->getPosition();

        // Calibration aid: write the game's own player rectangle to the log, once per gamemode/size.
        // (Geode log: Geode menu -> Logs, or the geode/logs folder.) Used to tune "Player hitbox size".
        {
            int mode = p_mode(player);
            int mini = player->m_vehicleSize < 0.9f ? 1 : 0;
            static int seen = 0;
            int bit = 1 << (mode * 2 + mini);
            if (!(seen & bit)) {
                seen |= bit;
                CCRect const& r = player->getObjectRect();
                log::info("Frame Window Counter: gamemode {} mini={} -> game player rect {} x {} (my half-size {})",
                          mode, mini, r.size.width, r.size.height, pc.half);
            }
        }
        q.push_back(pc);
    }

    // Measure every queued click that is old enough, then drop it.
    void measureReadyClicks() {
        auto& q = m_fields->pending;
        if (q.empty()) return;
        long nowTick = m_fields->history.ticks();

        fwc::MeasureSettings cfg;
        cfg.hazardDistance = Mod::get()->getSettingValue<double>("hazard-distance");

        for (size_t i = 0; i < q.size();) {
            if (nowTick - q[i].tick < kMeasureDelayTicks) { i++; continue; }
            PendingClick pc = q[i];
            q.erase(q.begin() + i);

            cfg.half = pc.half;
            auto result = fwc::measureClick(m_fields->shapes, m_fields->history, pc.time,
                                            pc.requireChange, cfg);
            // Diagnostic: one line per click so we can see where measuring stops.
            // reason: 0 counted, 1 motion invalid, 2 nothing changed, 3 no hazard close ahead.
            log::info("FWC click: shapes={} historySize={} reason={} nearby={} frames={:.2f} press={}",
                      m_fields->shapes.size(), m_fields->history.size(), result.reason,
                      result.nearby, result.seconds * currentTps(), pc.requireChange);
            if (result.counted) this->recordWindowFrames(result.seconds * currentTps(), pc.pos);
        }
    }

    // Called at the start of every attempt (after dying, restarting, etc).
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->stats.reset();
        m_fields->history.clear();
        m_fields->pending.clear();
        this->refreshHud();
        if (m_fields->hud) m_fields->hud->hidePopup();
        if (m_fields->markerLayer) m_fields->markerLayer->removeAllChildren(); // circles are per attempt too
    }

    // Our own helpers (not hooks).
    void refreshHud() {
        auto hud = m_fields->hud;
        if (!hud) return;

        auto mod = Mod::get();
        bool enabled = mod->getSettingValue<bool>("enabled");
        hud->setVisible(enabled);
        if (!enabled) return;

        hud->refresh(
            m_fields->stats,
            mod->getSettingValue<bool>("show-counts"),
            mod->getSettingValue<double>("hud-scale"),
            mod->getSettingValue<int64_t>("hud-opacity"),
            mod->getSettingValue<double>("row-spacing")
        );
    }

    // A ring plus the frame number, drawn at `pos` (level coordinates) in the color of the row.
    void addMarker(CCPoint pos, double frames) {
        auto layer = m_fields->markerLayer;
        if (!layer) return;

        auto c = fwc::kBucketColors[fwc::bucketForFrames(frames)];

        auto marker = CCNode::create();
        marker->setPosition(pos);

        auto ring = CCDrawNode::create();
        ring->drawCircle(
            CCPoint{0.f, 0.f}, 14.f,
            ccColor4F{0.f, 0.f, 0.f, 0.f},                          // no fill
            2.f,                                                    // line thickness
            ccColor4F{c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f}, // row color
            24
        );
        marker->addChild(ring);

        auto label = CCLabelBMFont::create(fwc::trimNumber(frames).c_str(), "bigFont.fnt");
        label->setScale(0.45f);
        label->setColor(ccColor3B{c.r, c.g, c.b});
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({18.f, 0.f});
        marker->addChild(label);

        layer->addChild(marker);

        // Keep it light: at most 300 circles at a time, oldest go first.
        while (layer->getChildrenCount() > 300) {
            layer->removeChild(static_cast<CCNode*>(layer->getChildren()->objectAtIndex(0)));
        }
    }

    // Adds one click whose window was `frames` frames (decimals allowed) at the current TPS setting.
    // `playerPos` is where the player was when you clicked.
    void recordWindowFrames(double frames, CCPoint playerPos) {
        double tps = currentTps();
        m_fields->stats.add(fwc::framesToMs(frames, tps), tps);
        this->refreshHud();

        if (Mod::get()->getSettingValue<bool>("enabled") && Mod::get()->getSettingValue<bool>("show-circles")) {
            this->addMarker(playerPos, frames);
        }

        // Show the time required to click, then fade it out. The next click restarts this.
        auto mod = Mod::get();
        if (m_fields->hud && mod->getSettingValue<bool>("enabled")) {
            m_fields->hud->popup(
                m_fields->stats.shownMs(mod->getSettingValue<bool>("show-hardest")),
                tps,
                mod->getSettingValue<bool>("show-hz"),
                mod->getSettingValue<double>("popup-hold"),
                mod->getSettingValue<double>("popup-fade"),
                mod->getSettingValue<bool>("popup-stay"),
                mod->getSettingValue<int64_t>("hud-opacity")
            );
        }
    }
};

// ---------------------------------------------------------------------------
// PlayerObject: once per physics tick, remember where the wave is.
// ---------------------------------------------------------------------------
class $modify(FWCPlayerObject, PlayerObject) {
    void update(float dt) {
        PlayerObject::update(dt);

        auto playLayer = PlayLayer::get();
        if (!playLayer || playLayer->m_player1 != this) return;
        static_cast<FWCPlayLayer*>(playLayer)->pushSample(dt, this->getPosition());
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer handles the inputs. We watch the jump button.
// ---------------------------------------------------------------------------
class $modify(FWCBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button != static_cast<int>(PlayerButton::Jump)) return;

        // Only count clicks in a real level, not in the editor.
        auto playLayer = PlayLayer::get();
        if (!playLayer) return;
        if (static_cast<GJBaseGameLayer*>(playLayer) != static_cast<GJBaseGameLayer*>(this)) return;

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;
        auto layer = static_cast<FWCPlayLayer*>(playLayer);

        if (!m_player1) return;
        if (!isPlayer1 && m_gameState.m_isDualMode) return; // measure player 1 only
        auto p = m_player1;
        if (p->m_isDead) return;

        // Hold gamemodes (ship, wave, robot, swing): letting go changes the motion too, so both the
        // press and the release are measured. This is where robot release timings come from.
        // Tap gamemodes (cube, ball, UFO, spider): only the press does anything.
        bool holdMode = p->m_isShip || p->m_isDart || p->m_isRobot || p->m_isSwing;
        if (!down && !holdMode) return; // release that does nothing: ignore

        // A press might do nothing (buffered, or pressing again while already airborne), so for a
        // press we only keep clicks that actually changed the motion. A release of a hold gamemode
        // is trusted to matter (its change can be a small bend we would otherwise miss).
        bool requireChange = down;
        layer->queueClick(requireChange);
    }
};
