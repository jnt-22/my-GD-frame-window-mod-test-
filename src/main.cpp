#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cmath>
#include <random>

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

    // DEMO MODE ONLY (setting "Demo data"): a made-up window of 1 to 12 frames.
    double fakeWindowFrames() {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(1.0, 12.0);
        return std::round(dist(rng) * 100.0) / 100.0;
    }
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
    void pushSample(double dt, CCPoint pos) {
        m_fields->history.push(dt, pos.x, pos.y);
    }

    // Measures the click that just happened (wave only) and shows the result.
    void measureAndRecord(bool pressedDown) {
        auto mod = Mod::get();
        auto player = m_player1;
        if (!player) return;

        fwc::MeasureSettings cfg;
        cfg.half = 0.5 * mod->getSettingValue<double>("wave-hitbox") * player->m_vehicleSize;
        cfg.hazardDistance = mod->getSettingValue<double>("hazard-distance");

        bool mini = player->m_vehicleSize < 0.9f;
        auto result = fwc::measureClick(m_fields->shapes, m_fields->history, pressedDown,
                                        player->m_isUpsideDown, mini, cfg);
        if (!result.counted) return; // nothing close ahead, or not a turn: not counted

        this->recordWindowFrames(result.seconds * currentTps(), player->getPosition());
    }

    // Called at the start of every attempt (after dying, restarting, etc).
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->stats.reset();
        m_fields->history.clear();
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
            mod->getSettingValue<bool>("demo-mode"),
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

        // Demo data (for testing the display): fake windows on every press.
        if (mod->getSettingValue<bool>("demo-mode")) {
            if (!down) return;
            // A mouse/touch click can arrive as "player 2" even with only one player; player 2 then
            // sits somewhere else on the level. Outside dual mode it is always player 1.
            auto player = (isPlayer1 || !m_isDualMode) ? m_player1 : m_player2;
            if (!player) player = m_player1;
            layer->recordWindowFrames(fakeWindowFrames(), player->getPosition());
            return;
        }

        // Real measuring: the wave only. Both pressing and letting go turn the wave.
        if (!m_player1 || !m_player1->m_isDart) return;
        if (!isPlayer1 && m_isDualMode) return; // only player 1's wave
        layer->measureAndRecord(down);
    }
};
