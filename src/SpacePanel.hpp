#pragma once

// A little glossy, space-themed panel (no spacecraft) that opens and closes with the "\" key.
// Lets you change the most useful settings without leaving the level:
//   TPS, Player hitbox size, and the Show circles / Show counts / Show Hz / Keep popup toggles.
//
// It is drawn from plain shapes (a rounded dark panel, a soft gloss highlight, a scatter of stars),
// so it needs no custom textures. Setting changes are written straight to the mod settings and the
// on-screen counter is refreshed through two small hand-off functions defined in main.cpp.

#include <Geode/Geode.hpp>
#include <array>
#include <cmath>

using namespace geode::prelude;

namespace fwc {
    // Defined in main.cpp (after the PlayLayer class), so the panel does not need to know about it.
    void onSettingsChanged(); // re-read settings and refresh the counter
    void requestClosePanel(); // close + forget the panel

    // TPS values the arrows step through.
    static const std::array<int, 11> kTpsPresets = {
        {60, 120, 240, 360, 480, 600, 720, 960, 1200, 2400, 3600}
    };
}

class SpacePanel : public cocos2d::CCLayerColor {
public:
    static SpacePanel* create() {
        auto ret = new SpacePanel();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() override {
        if (!CCLayerColor::initWithColor(ccColor4B{0, 0, 0, 165})) return false;

        this->setTouchEnabled(true);
        this->setTouchMode(kCCTouchesOneByOne); // swallow clicks so the game does not get them

        auto win = CCDirector::get()->getWinSize();

        m_content = CCNode::create();
        m_content->setPosition(win.width / 2.f, win.height / 2.f);
        this->addChild(m_content);

        const float w = 360.f, h = 272.f;
        this->drawPanel(w, h);

        auto title = CCLabelBMFont::create("FRAME WINDOW", "bigFont.fnt");
        title->setScale(0.62f);
        title->setPosition(0.f, h / 2.f - 26.f);
        title->setColor(ccColor3B{210, 190, 255});
        m_content->addChild(title);

        auto hint = CCLabelBMFont::create("press  \\  to close", "chatFont.fnt");
        hint->setScale(0.55f);
        hint->setOpacity(150);
        hint->setPosition(0.f, -h / 2.f + 16.f);
        hint->setColor(ccColor3B{170, 160, 210});
        m_content->addChild(hint);

        m_menu = CCMenu::create();
        m_menu->setPosition(0.f, 0.f);
        m_content->addChild(m_menu);

        // Rows, top to bottom.
        float y = 78.f;
        m_tpsVal  = this->numberRow("TPS", y, 1, 2);            y -= 34.f;
        m_hitVal  = this->numberRow("Hitbox", y, 3, 4);         y -= 34.f;
        m_btnCircles = this->toggleRow("Circles", y, 10);       y -= 30.f;
        m_btnCounts  = this->toggleRow("Counts",  y, 11);       y -= 30.f;
        m_btnHz      = this->toggleRow("Hz line", y, 12);       y -= 30.f;
        m_btnPopup   = this->toggleRow("Keep popup", y, 13);

        // Close X in the top-right corner.
        auto x = CCLabelBMFont::create("x", "bigFont.fnt");
        x->setScale(0.7f);
        auto closeItem = CCMenuItemSpriteExtra::create(x, this, menu_selector(SpacePanel::onButton));
        closeItem->setTag(99);
        closeItem->setPosition(w / 2.f - 20.f, h / 2.f - 20.f);
        m_menu->addChild(closeItem);

        this->refresh();
        return true;
    }

    // Re-reads the settings and updates every value shown.
    void refresh() {
        auto mod = Mod::get();
        if (m_tpsVal) m_tpsVal->setString(numToStr(mod->getSettingValue<int64_t>("tps")).c_str());
        if (m_hitVal) m_hitVal->setString(numToStr((int)std::lround(mod->getSettingValue<double>("player-hitbox"))).c_str());
        setToggle(m_btnCircles, mod->getSettingValue<bool>("show-circles"));
        setToggle(m_btnCounts,  mod->getSettingValue<bool>("show-counts"));
        setToggle(m_btnHz,      mod->getSettingValue<bool>("show-hz"));
        setToggle(m_btnPopup,   mod->getSettingValue<bool>("popup-stay"));
    }

    // Every button comes here; the tag says which one.
    void onButton(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        auto mod = Mod::get();

        switch (tag) {
            case 1: stepTps(-1); break;
            case 2: stepTps(+1); break;
            case 3: stepHitbox(-1.0); break;
            case 4: stepHitbox(+1.0); break;
            case 10: flip("show-circles"); break;
            case 11: flip("show-counts"); break;
            case 12: flip("show-hz"); break;
            case 13: flip("popup-stay"); break;
            case 99: fwc::requestClosePanel(); return;
            default: break;
        }
        (void)mod;
        this->refresh();
        fwc::onSettingsChanged();
    }

    // Swallow any click that is not on a button, so it does not reach the game.
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return true; }

private:
    CCNode* m_content = nullptr;
    CCMenu* m_menu = nullptr;
    CCLabelBMFont* m_tpsVal = nullptr;
    CCLabelBMFont* m_hitVal = nullptr;
    ButtonSprite* m_btnCircles = nullptr;
    ButtonSprite* m_btnCounts = nullptr;
    ButtonSprite* m_btnHz = nullptr;
    ButtonSprite* m_btnPopup = nullptr;

    static std::string numToStr(long v) { return std::to_string(v); }

    void flip(char const* key) {
        Mod::get()->setSettingValue<bool>(key, !Mod::get()->getSettingValue<bool>(key));
    }

    void stepTps(int dir) {
        long cur = Mod::get()->getSettingValue<int64_t>("tps");
        // Find the closest preset, then move one step.
        int best = 0; long bestd = 1L << 60;
        for (int i = 0; i < (int)fwc::kTpsPresets.size(); i++) {
            long d = std::labs((long)fwc::kTpsPresets[i] - cur);
            if (d < bestd) { bestd = d; best = i; }
        }
        int ni = std::min((int)fwc::kTpsPresets.size() - 1, std::max(0, best + dir));
        Mod::get()->setSettingValue<int64_t>("tps", fwc::kTpsPresets[ni]);
    }

    void stepHitbox(double dir) {
        double v = Mod::get()->getSettingValue<double>("player-hitbox") + dir;
        v = std::min(30.0, std::max(4.0, v));
        Mod::get()->setSettingValue<double>("player-hitbox", v);
    }

    static void setToggle(ButtonSprite* b, bool on) {
        if (!b) return;
        b->setString(on ? "ON" : "OFF");
        b->setColor(on ? ccColor3B{120, 230, 140} : ccColor3B{230, 120, 120});
    }

    // --- building blocks -----------------------------------------------------------------------

    // A row with a name on the left, a value in the middle, and two arrows. Returns the value label.
    CCLabelBMFont* numberRow(char const* name, float y, int tagMinus, int tagPlus) {
        auto label = CCLabelBMFont::create(name, "goldFont.fnt");
        label->setScale(0.6f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition(-162.f, y);
        m_content->addChild(label);

        auto value = CCLabelBMFont::create("-", "bigFont.fnt");
        value->setScale(0.6f);
        value->setPosition(70.f, y);
        m_content->addChild(value);

        addArrow("<", 118.f, y, tagMinus);
        addArrow(">", 156.f, y, tagPlus);
        return value;
    }

    void addArrow(char const* glyph, float x, float y, int tag) {
        auto spr = ButtonSprite::create(glyph);
        spr->setScale(0.7f);
        auto item = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SpacePanel::onButton));
        item->setTag(tag);
        item->setPosition(x, y);
        m_menu->addChild(item);
    }

    // A row with a name on the left and an ON/OFF button. Returns the button sprite (to update it).
    ButtonSprite* toggleRow(char const* name, float y, int tag) {
        auto label = CCLabelBMFont::create(name, "goldFont.fnt");
        label->setScale(0.6f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition(-162.f, y);
        m_content->addChild(label);

        auto spr = ButtonSprite::create("OFF");
        spr->setScale(0.6f);
        auto item = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SpacePanel::onButton));
        item->setTag(tag);
        item->setPosition(130.f, y);
        m_menu->addChild(item);
        return spr;
    }

    // The dark glossy panel with a border, a gloss highlight and a scatter of stars.
    void drawPanel(float w, float h) {
        auto draw = CCDrawNode::create();
        m_content->addChild(draw, -2);

        ccColor4F deep   = {0.08f, 0.05f, 0.16f, 0.97f}; // deep space
        ccColor4F border = {0.55f, 0.45f, 0.95f, 1.0f};  // violet edge
        ccColor4F gloss  = {1.0f, 1.0f, 1.0f, 0.10f};    // soft highlight

        this->roundedRect(draw, w, h, 16.f, deep, 1.6f, border);

        // Gloss across the top third.
        this->roundedRect(draw, w - 14.f, (h - 14.f) * 0.42f, 12.f, gloss, 0.f, gloss,
                          h / 2.f - 7.f - (h - 14.f) * 0.21f);

        // Stars (deterministic so they do not jump around on redraw).
        unsigned int seed = 1234567u;
        auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32767.f; };
        for (int i = 0; i < 46; i++) {
            float px = (rnd() - 0.5f) * (w - 24.f);
            float py = (rnd() - 0.5f) * (h - 24.f);
            float r = 0.5f + rnd() * 1.4f;
            float a = 0.25f + rnd() * 0.65f;
            draw->drawDot({px, py}, r, ccColor4F{1.f, 1.f, 1.f, a});
        }
        // A few brighter "glow" stars with a faint tint.
        for (int i = 0; i < 6; i++) {
            float px = (rnd() - 0.5f) * (w - 40.f);
            float py = (rnd() - 0.5f) * (h - 40.f);
            draw->drawDot({px, py}, 2.6f, ccColor4F{0.8f, 0.85f, 1.f, 0.5f});
            draw->drawDot({px, py}, 1.2f, ccColor4F{1.f, 1.f, 1.f, 0.95f});
        }
    }

    // Draws a filled, bordered rounded rectangle centred at (0, cy) into `draw`.
    void roundedRect(CCDrawNode* draw, float w, float h, float r,
                     ccColor4F fill, float borderW, ccColor4F border, float cy = 0.f) {
        r = std::min(r, std::min(w, h) * 0.5f);
        const int seg = 6; // points per corner
        std::vector<CCPoint> pts;
        pts.reserve((seg + 1) * 4);
        float hw = w / 2.f, hh = h / 2.f;
        // centres of the four corner arcs
        CCPoint c[4] = {{hw - r, hh - r}, {-hw + r, hh - r}, {-hw + r, -hh + r}, {hw - r, -hh + r}};
        float start[4] = {0.f, (float)M_PI_2, (float)M_PI, (float)(3.0 * M_PI_2)};
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i <= seg; i++) {
                float a = start[k] + (float)M_PI_2 * (float)i / (float)seg;
                pts.push_back({c[k].x + r * std::cos(a), cy + c[k].y + r * std::sin(a)});
            }
        }
        draw->drawPolygon(pts.data(), (unsigned int)pts.size(), fill, borderW, border);
    }
};
