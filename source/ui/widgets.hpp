// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "gfx/draw.hpp"
#include "ui/input.hpp"

#include <string>
#include <vector>

namespace tz::ui
{

// A button and what it does, along the bottom of the bottom screen. Tapping a hint presses its button.
struct Hint
{
    u32 key = 0;
    std::string label;
};

class HintBar
{
public:
    // Turns taps on the hints into their buttons. Call before handling input.
    void update(Input& input, const std::vector<Hint>& hints);
    void draw(const std::vector<Hint>& hints, float alpha = 1.0f);

private:
    struct Spot
    {
        float x, width;
        u32 key;
    };
    std::vector<Spot> spots_;
    float y_ = 0;
    float height_ = 0;
};

// The symbol for a button in the console's font, or "START"/"SELECT", which the font hasn't got.
const char* buttonGlyph(u32 key);
// How wide drawButton draws a button.
float buttonWidth(u32 key, float size);
// Draws a button as the hints do, with y at the top of the line. Returns its width.
float drawButton(u32 key, float x, float y, float size, gfx::Color color);
// A button and what it does, centred on middle: the button at metric::kButtonGlyph, the label at labelSize. Returns
// its width.
float hintWidth(u32 key, std::string_view label, float labelSize = gfx::metric::kTextNormal);
float drawHint(u32 key, std::string_view label, float x, float middle, gfx::Color buttonColor, gfx::Color labelColor,
               float labelSize = gfx::metric::kTextNormal);

// Tabs in a pill-shaped bar, as the website's switchers look: the selected one in its accent. Two buttons switch them,
// drawn at the bar's ends: L and R for the main tabs.
struct Tab
{
    std::string label; // empty for a tab that's only its icon
    gfx::Accent accent;
    gfx::Icon icon = gfx::Icon::Count;
};

class TabBar
{
public:
    explicit TabBar(u32 previousKey = KEY_L, u32 nextKey = KEY_R) : previousKey_(previousKey), nextKey_(nextKey) {}
    void setTabs(std::vector<Tab> tabs) { tabs_ = std::move(tabs); }
    int selected() const { return selected_; }
    void select(int index) { selected_ = index; }
    // The two buttons (which then do nothing else), or a tap on a tab. Returns whether the tab changed.
    bool update(Input& input);
    // In the box from (x, y); the names at textSize, or smaller when they don't fit.
    void draw(float x, float y, float width, float height, float textSize);

private:
    struct Slot
    {
        float x, width;
    };
    float contentWidth(const Tab& tab, float size) const;

    u32 previousKey_;
    u32 nextKey_;
    std::vector<Tab> tabs_;
    std::vector<Slot> slots_; // where the last draw put the tabs, for taps
    float top_ = 0;
    float bottom_ = 0;
    int selected_ = 0;
    float highlightX_ = -1; // the highlight slides to the selected tab
    float highlightWidth_ = 0;
};

} // namespace tz::ui
