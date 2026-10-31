#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <cmath>
#include <random>

#include "Hud.hpp"
#include "WindowStats.hpp"

using namespace geode::prelude;

namespace {
    // Physics ticks per second (TPS) used to turn frames into milliseconds and back.
    // 240 is Geometry Dash's normal speed; set 480 to see how the same windows look at 480 TPS.
    double currentTps() {
        auto tps = Mod::get()->getSettingValue<int64_t>("tps");
        return tps > 0 ? static_cast<double>(tps) : 240.0;
    }

    // DEMO ONLY: a made-up window of 1 to 12 frames, with decimals like real windows have.
    // This is replaced by real measuring in the next version.
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

        this->refreshHud();
    }

    // Called at the start of every attempt (after dying, restarting, etc).
    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->stats.reset();
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
            mod->getSettingValue<int64_t>("hud-opacity")
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
// GJBaseGameLayer handles the inputs. We watch for the jump button going down.
// ---------------------------------------------------------------------------
class $modify(FWCBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!down || button != static_cast<int>(PlayerButton::Jump)) return;

        // Only count clicks in a real level, not in the editor.
        auto playLayer = PlayLayer::get();
        if (!playLayer) return;
        if (static_cast<GJBaseGameLayer*>(playLayer) != static_cast<GJBaseGameLayer*>(this)) return;

        if (!Mod::get()->getSettingValue<bool>("enabled")) return;

        // v0.1: real window measuring is not built yet, so only demo data is fed in.
        if (Mod::get()->getSettingValue<bool>("demo-mode")) {
            // Which player clicked (player 2 only exists in dual mode).
            auto player = isPlayer1 ? m_player1 : m_player2;
            if (!player) player = m_player1;
            static_cast<FWCPlayLayer*>(playLayer)->recordWindowFrames(fakeWindowFrames(), player->getPosition());
        }
    }
};
