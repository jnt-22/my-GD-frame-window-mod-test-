#pragma once

#include <Geode/Geode.hpp>
#include <array>
#include <cmath>
#include "WindowStats.hpp"

using namespace geode::prelude;

// The little rainbow box in the corner of the screen:
//
//   10+:   8      <- one row per bucket: label on the left, count on the right
//   8-10:  1
//   ...
//   4 FRAMES      <- popup: appears on every click (style copied from the reference video)
//   16.67 MS         fades out shortly after, next click restarts it
//   60Hz             (optional third line: frame-perfect tick rate)
class FrameWindowHUD : public CCNode {
protected:
    static constexpr float kRowHeight = 17.f;
    static constexpr float kCountColumnX = 82.f; // x of the right edge of the count column
    static constexpr float kFontScale = 0.4f;

    std::array<CCLabelBMFont*, fwc::kBucketCount> m_names{};
    std::array<CCLabelBMFont*, fwc::kBucketCount> m_counts{};
    CCLabelBMFont* m_framesLabel = nullptr;
    CCLabelBMFont* m_msLabel = nullptr;
    CCLabelBMFont* m_hzLabel = nullptr;

    CCLabelBMFont* makeLabel(char const* text, ccColor3B color, float x, float y, bool rightAligned) {
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(kFontScale);
        label->setColor(color);
        label->setAnchorPoint({rightAligned ? 1.f : 0.f, 0.5f});
        label->setPosition({x, y});
        this->addChild(label);
        return label;
    }

    bool init() override {
        if (!CCNode::init()) return false;

        ccColor3B const white{255, 255, 255};
        ccColor3B const gray{150, 150, 150};

        for (int i = 0; i < fwc::kBucketCount; i++) {
            float y = -static_cast<float>(i) * kRowHeight;
            auto c = fwc::kBucketColors[i];
            ccColor3B color{c.r, c.g, c.b};
            m_names[i] = makeLabel(fwc::kBucketLabels[i], color, 0.f, y, false);
            m_counts[i] = makeLabel("0", color, kCountColumnX, y, true);
        }

        float footerY = -static_cast<float>(fwc::kBucketCount) * kRowHeight - 6.f;
        m_framesLabel = makeLabel("0 FRAMES", white, 0.f, footerY, false);
        m_msLabel = makeLabel("0 MS", white, 0.f, footerY - kRowHeight, false);
        m_hzLabel = makeLabel("0Hz", gray, 0.f, footerY - 2.f * kRowHeight, false);

        // The popup starts hidden.
        m_framesLabel->setOpacity(0);
        m_msLabel->setOpacity(0);
        m_hzLabel->setOpacity(0);

        return true;
    }

public:
    static FrameWindowHUD* create() {
        auto ret = new FrameWindowHUD();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    static GLubyte toOpacity(int64_t percent) {
        return static_cast<GLubyte>(std::clamp<int64_t>(percent, 0, 100) * 255 / 100);
    }

    // Updates the rows (labels + counts). Does not touch the popup.
    // Moves every row (and the popup lines under them) so rows are `rowHeight` apart.
    void applyRowSpacing(float rowHeight) {
        for (int i = 0; i < fwc::kBucketCount; i++) {
            float y = -static_cast<float>(i) * rowHeight;
            m_names[i]->setPositionY(y);
            m_counts[i]->setPositionY(y);
        }
        float footerY = -static_cast<float>(fwc::kBucketCount) * rowHeight - rowHeight * 0.35f;
        m_framesLabel->setPositionY(footerY);
        m_msLabel->setPositionY(footerY - rowHeight);
        m_hzLabel->setPositionY(footerY - 2.f * rowHeight);
    }

    void refresh(fwc::Stats const& stats, bool showCounts, double scale, int64_t opacityPercent, double rowSpacing) {
        this->setScale(static_cast<float>(scale));
        this->applyRowSpacing(static_cast<float>(rowSpacing));
        GLubyte opacity = toOpacity(opacityPercent);

        for (int i = 0; i < fwc::kBucketCount; i++) {
            m_names[i]->setOpacity(opacity);
            m_counts[i]->setOpacity(opacity);
            m_counts[i]->setVisible(showCounts);
            m_counts[i]->setString(fmt::format("{}", stats.counts[i]).c_str());
        }
    }

    // Click happened: show "N FRAMES" and "X MS" (and optionally the Hz line) right away.
    // They stay for `holdSeconds`, then fade out over `fadeSeconds`. If `stay` is true they
    // stay until the next click instead. Calling this again (next click) restarts everything.
    void popup(double windowMs, double tps, bool showHz, double holdSeconds, double fadeSeconds, bool stay, int64_t opacityPercent) {
        GLubyte opacity = toOpacity(opacityPercent);
        m_framesLabel->setString(fwc::formatFramesLine(fwc::msToFrames(windowMs, tps)).c_str());
        m_msLabel->setString(fwc::formatMsLine(windowMs).c_str());
        m_hzLabel->setString(fwc::formatFpHz(windowMs).c_str());

        std::array<CCLabelBMFont*, 3> labels = {m_framesLabel, m_msLabel, m_hzLabel};
        for (size_t i = 0; i < labels.size(); i++) {
            auto label = labels[i];
            label->stopAllActions();          // interrupt the previous fade
            if (i == 2 && !showHz) {          // Hz line is optional
                label->setOpacity(0);
                continue;
            }
            label->setOpacity(opacity);
            if (stay) continue;
            label->runAction(CCSequence::create(
                CCDelayTime::create(static_cast<float>(std::max(holdSeconds, 0.0))),
                CCFadeTo::create(static_cast<float>(std::max(fadeSeconds, 0.0)), 0),
                nullptr
            ));
        }
    }

    // New attempt: hide the popup.
    void hidePopup() {
        for (auto label : {m_framesLabel, m_msLabel, m_hzLabel}) {
            label->stopAllActions();
            label->setOpacity(0);
        }
    }
};
