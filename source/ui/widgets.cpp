// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "ui/widgets.hpp"

#include "gfx/renderer.hpp"
#include "gfx/text.hpp"

#include <algorithm>
#include <cmath>

namespace tz::ui
{

using namespace gfx;

namespace
{

constexpr float kHintGap = 12;
constexpr float kHintMargin = 8;

bool isWordButton(u32 key)
{
    return key == KEY_START || key == KEY_SELECT;
}

// START and SELECT: a small pill with the word, as the buttons are printed
struct WordButton
{
    float textSize;
    float width;
    float height;
};

WordButton wordButton(u32 key, float size)
{
    const float textSize = size * 0.62f;
    return {textSize, std::round(textWidth(buttonGlyph(key), textSize) + 8), std::round(lineHeight(size) * 0.72f)};
}

} // namespace

const char* buttonGlyph(u32 key)
{
    switch (key)
    {
    case KEY_A:
        return glyph::kA;
    case KEY_B:
        return glyph::kB;
    case KEY_X:
        return glyph::kX;
    case KEY_Y:
        return glyph::kY;
    case KEY_L:
        return glyph::kL;
    case KEY_R:
        return glyph::kR;
    case KEY_START:
        return "START";
    case KEY_SELECT:
        return "SELECT";
    default:
        break;
    }
    const bool up = key & KEY_UP;
    const bool down = key & KEY_DOWN;
    const bool left = key & KEY_LEFT;
    const bool right = key & KEY_RIGHT;
    if ((up || down) && !left && !right)
        return up && down ? glyph::kDpadUpDown : (up ? glyph::kDpadUp : glyph::kDpadDown);
    if ((left || right) && !up && !down)
        return left && right ? glyph::kDpadLeftRight : (left ? glyph::kDpadLeft : glyph::kDpadRight);
    return glyph::kDpad;
}

float buttonWidth(u32 key, float size)
{
    return isWordButton(key) ? wordButton(key, size).width : textWidth(buttonGlyph(key), size);
}

float drawButton(u32 key, float x, float y, float size, Color color)
{
    if (!isWordButton(key))
        return drawText(buttonGlyph(key), x, y, size, color);
    const WordButton button = wordButton(key, size);
    const float top = std::round(textMiddle(y, size) - button.height / 2);
    x = std::round(x);
    roundedRect(x, top, button.width, button.height, button.height / 2, withAlpha(color, 0.9f));
    drawText(buttonGlyph(key), x + button.width / 2, textTop(top + button.height / 2, button.textSize),
             button.textSize, color::kOnAccent, Align::Center);
    return button.width;
}

float hintWidth(u32 key, std::string_view label, float labelSize)
{
    return buttonWidth(key, metric::kButtonGlyph) + 4 + textWidth(label, labelSize);
}

float drawHint(u32 key, std::string_view label, float x, float middle, Color buttonColor, Color labelColor,
               float labelSize)
{
    const float button = drawButton(key, x, textTop(middle, metric::kButtonGlyph), metric::kButtonGlyph, buttonColor);
    return button + 4 + drawText(label, x + button + 4, textTop(middle, labelSize), labelSize, labelColor);
}

void HintBar::update(Input& input, const std::vector<Hint>& hints)
{
    if (!input.tap || input.y < y_ || input.y >= y_ + height_)
        return;
    for (size_t i = 0; i < spots_.size() && i < hints.size(); ++i)
    {
        if (input.x >= spots_[i].x && input.x < spots_[i].x + spots_[i].width)
        {
            input.press(spots_[i].key);
            input.consumeTouch();
            return;
        }
    }
}

void HintBar::draw(const std::vector<Hint>& hints, float alpha)
{
    height_ = metric::kHintBarHeight;
    y_ = metric::kHeight - height_;
    spots_.clear();
    if (hints.empty() || alpha <= 0)
        return;
    rect(0, y_, metric::kBottomWidth, height_, withAlpha(color::kSurfaceSolid, 0.94f * alpha));
    rect(0, y_, metric::kBottomWidth, 1, withAlpha(color::kDivider, alpha));

    // closer together, then smaller labels, when they don't fit
    float labelSize = metric::kTextNormal;
    float gap = kHintGap;
    auto total = [&] {
        float sum = 2 * kHintMargin - gap;
        for (const Hint& hint : hints)
            sum += hintWidth(hint.key, hint.label, labelSize) + gap;
        return sum;
    };
    if (total() > metric::kBottomWidth)
        gap = 8;
    if (total() > metric::kBottomWidth)
        labelSize = metric::kTextSmall;

    const float middle = y_ + 1 + (height_ - 1) / 2;
    float x = kHintMargin;
    for (const Hint& hint : hints)
    {
        const float width = hintWidth(hint.key, hint.label, labelSize);
        if (x + width > metric::kBottomWidth)
            break;
        drawHint(hint.key, hint.label, x, middle, withAlpha(color::kText, alpha), withAlpha(color::kTextDim, alpha),
                 labelSize);
        spots_.push_back({x - gap / 2, width + gap, hint.key});
        x += width + gap;
    }
}

bool TabBar::update(Input& input)
{
    if (tabs_.empty())
        return false;
    const int count = static_cast<int>(tabs_.size());
    const int before = selected_;
    if (previousKey_ && input.pressed(previousKey_))
        selected_ = (selected_ + count - 1) % count;
    else if (nextKey_ && input.pressed(nextKey_))
        selected_ = (selected_ + 1) % count;
    else if (input.tap && input.y >= top_ && input.y < bottom_ && !slots_.empty())
    {
        // a tab, or the buttons' symbols at the ends, which go to the tab before or after
        if (input.x < slots_.front().x)
            selected_ = (selected_ + count - 1) % count;
        else if (input.x >= slots_.back().x + slots_.back().width)
            selected_ = (selected_ + 1) % count;
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            if (input.x >= slots_[i].x && input.x < slots_[i].x + slots_[i].width)
                selected_ = static_cast<int>(i);
        }
        input.consumeTouch();
    }
    // the buttons switch tabs here, and do nothing else (the D-pad doesn't also page the list)
    input.down &= ~(previousKey_ | nextKey_);
    input.repeat &= ~(previousKey_ | nextKey_);
    return selected_ != before;
}

float TabBar::contentWidth(const Tab& tab, float size) const
{
    return tab.label.empty() ? metric::kIcon : textWidth(tab.label, size);
}

void TabBar::draw(float x, float y, float width, float height, float textSize)
{
    if (tabs_.empty())
        return;
    top_ = y;
    bottom_ = y + height;
    const float middle = y + height / 2;
    roundedRect(x + 3, y + 3, width - 6, height - 6, (height - 6) / 2, color::kSurface);

    // the buttons that switch the tabs, at the ends
    float left = x + 8;
    float right = x + width - 8;
    if (previousKey_ && nextKey_)
    {
        const float glyphTop = textTop(middle, metric::kButtonGlyph);
        left += drawText(buttonGlyph(previousKey_), left, glyphTop, metric::kButtonGlyph, color::kTextDim) + 4;
        right -= textWidth(buttonGlyph(nextKey_), metric::kButtonGlyph);
        drawText(buttonGlyph(nextKey_), right, glyphTop, metric::kButtonGlyph, color::kTextDim);
        right -= 4;
    }

    // each tab as wide as its name (or its icon), with what's left shared out evenly
    const size_t count = tabs_.size();
    float size = textSize;
    float names = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        names = 0;
        for (const Tab& tab : tabs_)
            names += contentWidth(tab, size);
        if ((right - left - names) / static_cast<float>(count) >= 12 || size <= metric::kTextSmall)
            break;
        size = metric::kTextSmall;
    }
    const float padding = std::max(0.0f, (right - left - names) / static_cast<float>(2 * count));
    slots_.clear();
    float at = left;
    for (const Tab& tab : tabs_)
    {
        const float slotWidth = contentWidth(tab, size) + 2 * padding;
        slots_.push_back({at, slotWidth});
        at += slotWidth;
    }

    // the highlight slides to the selected tab
    const Slot& target = slots_[static_cast<size_t>(selected_)];
    if (highlightX_ < 0)
    {
        highlightX_ = target.x;
        highlightWidth_ = target.width;
    }
    highlightX_ += (target.x - highlightX_) * 0.3f;
    highlightWidth_ += (target.width - highlightWidth_) * 0.3f;
    if (std::fabs(target.x - highlightX_) < 0.5f)
    {
        highlightX_ = target.x;
        highlightWidth_ = target.width;
    }
    const float pillHeight = height - 10;
    roundedRect(std::round(highlightX_), y + 5, std::round(highlightWidth_), pillHeight, pillHeight / 2,
                withAlpha(tabs_[static_cast<size_t>(selected_)].accent.solid, 0.35f));

    const float labelTop = textTop(middle, size);
    for (size_t i = 0; i < count; ++i)
    {
        const Tab& tab = tabs_[i];
        const Color tint = static_cast<int>(i) == selected_ ? color::kText : color::kTextDim;
        const float centre = slots_[i].x + slots_[i].width / 2;
        if (tab.label.empty())
            icon(tab.icon, centre - metric::kIcon / 2, std::round(middle - metric::kIcon / 2), metric::kIcon, tint);
        else
            drawText(tab.label, centre, labelTop, size, tint, Align::Center);
    }
}

} // namespace tz::ui
