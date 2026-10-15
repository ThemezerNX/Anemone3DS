// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// What every page shares: the blurred backdrop and the top screen's status bar.

#pragma once

#include "gfx/draw.hpp"
#include "gfx/palette.hpp"
#include "gfx/texture.hpp"

#include <memory>
#include <string_view>

namespace tz::scenes
{

// A soft blur of the selected item's picture behind both screens, fading from one item to the next, as the Themezer
// website shows behind its pages. Without a picture, a dark wash of the page's accent.
class Backdrop
{
public:
    void set(std::shared_ptr<gfx::Picture> blur, gfx::Color accent);
    void tick(float dt);
    // How dark the picture is made: more on the bottom screen, where there is more to read.
    void draw(float dim) const;

private:
    struct Layer
    {
        std::shared_ptr<gfx::Picture> picture;
        gfx::Color accent = gfx::color::kPrimary;
    };
    void drawLayer(const Layer& layer, float alpha) const;

    Layer current_;
    Layer previous_;
    float fade_ = 1; // 0 shows the previous layer, 1 the current one
};

// The top screen's bar: Themezer's logo on the left, the time, Wi-Fi and battery on the right. Left out while a
// preview fills the top screen, since a theme's preview has the HOME Menu's own bar.
void drawStatusBar(float alpha = 1.0f);

// The lists' rows, the same on every page: a 36-pixel icon on the left, the name over a dimmer line, and what the
// item is (in use, has music...) at the right edge.
namespace row
{
constexpr float kIconSize = 36;
constexpr float kIconX = 14;
constexpr float kTextX = kIconX + kIconSize + 10;
constexpr float kRight = 12; // the margin at the right edge

// The selected row: a lighter band, with a bar in the page's accent.
void highlight(float x, float y, float w, float h, gfx::Color accent);
// Where an icon of the row goes; a placeholder of the item's kind until the icon is there.
float iconTop(float y, float h);
void placeholder(float x, float y, float h, gfx::Icon kind);
// The two lines, centred in the row and cut to end at right; the name alone is centred.
void lines(std::string_view name, std::string_view detail, float x, float y, float h, float right,
           bool dimmed = false);
// At the right edge of the row, from right leftwards: a status (an icon and a word), or an icon alone. Both return
// where the next thing to their left ends.
float status(gfx::Icon icon, std::string_view text, gfx::Color color, float right, float y, float h);
float mark(gfx::Icon icon, gfx::Color color, float right, float y, float h);
} // namespace row

// A page with nothing to show: a large icon, what's the matter, and what to do, centred between top and bottom.
void drawEmptyState(gfx::Icon icon, gfx::Color iconColor, std::string_view title, std::string_view message, float top,
                    float bottom);

} // namespace tz::scenes
