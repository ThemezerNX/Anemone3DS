// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The app's colours and sizes, after the Themezer website: its dark mode's stone greys, frosted surfaces over a
// blurred picture of what's selected, its primary green, and the accent colour of each kind of content (themes green,
// splashes orange, badge sets pink).

#pragma once

#include <cstdint>

namespace tz::gfx
{

using Color = uint32_t;

// The GPU's colour order, which C2D_Color32 also makes.
constexpr Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return static_cast<Color>(r) | (static_cast<Color>(g) << 8) | (static_cast<Color>(b) << 16) |
           (static_cast<Color>(a) << 24);
}

constexpr Color withAlpha(Color color, float alpha)
{
    const float a = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha);
    return (color & 0x00FFFFFF) | (static_cast<Color>(static_cast<float>(color >> 24) * a) << 24);
}

Color mix(Color a, Color b, float t);

namespace color
{
// The page under everything: stone 950, which the website's frosted cards are made of too.
constexpr Color kBackground = rgba(12, 10, 9);
// Frosted surfaces over the blurred picture: the website's cards (stone 950 at 32 %) and its denser panels.
constexpr Color kFrosted = rgba(12, 10, 9, 82);
constexpr Color kSurface = rgba(12, 10, 9, 163);
constexpr Color kSurfaceSolid = rgba(28, 25, 23);   // stone 900
constexpr Color kSurfaceRaised = rgba(41, 37, 36);  // stone 800
constexpr Color kSurfaceHigh = rgba(68, 64, 60);    // stone 700
constexpr Color kDivider = rgba(255, 255, 255, 20);
constexpr Color kSelection = rgba(255, 255, 255, 30); // a selected row or segment, as on the website

constexpr Color kText = rgba(255, 255, 255);
constexpr Color kTextDim = rgba(168, 162, 158);   // stone 400, the website's muted text
constexpr Color kTextFaint = rgba(120, 113, 108); // stone 500

constexpr Color kPrimary = rgba(10, 179, 121);      // Themezer's green (primary 500)
constexpr Color kPrimaryLight = rgba(47, 216, 152); // primary 400
constexpr Color kSecondary = rgba(255, 50, 204);    // the pink of Themezer's logo
constexpr Color kOrange = rgba(249, 115, 22);       // orange 500
constexpr Color kOrangeLight = rgba(251, 146, 60);  // orange 400
constexpr Color kPink = rgba(236, 72, 153);         // pink 500
constexpr Color kPinkLight = rgba(244, 114, 182);   // pink 400
// text on a solid accent, as the website's selected buttons have it
constexpr Color kOnAccent = rgba(12, 10, 9);
constexpr Color kDanger = rgba(239, 68, 68);
constexpr Color kWarning = rgba(251, 191, 36);

constexpr Color kScrim = rgba(12, 10, 9, 170); // behind dialogs
constexpr Color kShadow = rgba(0, 0, 0, 100);
} // namespace color

// Each kind of content has the accent the website gives it (CONTENT_KIND_ACCENTS in the webapp): themes the primary
// green, splashes orange, badge sets pink. solid fills selected buttons (with kOnAccent text); light is for icons and
// text on the dark surfaces.
struct Accent
{
    Color solid;
    Color light;
};

namespace accent
{
constexpr Accent kThemes{color::kPrimary, color::kPrimaryLight};
constexpr Accent kSplashes{color::kOrange, color::kOrangeLight};
constexpr Accent kBadges{color::kPink, color::kPinkLight};
constexpr Accent kApp = kThemes;
} // namespace accent

namespace metric
{
constexpr float kTopWidth = 400;
constexpr float kBottomWidth = 320;
constexpr float kHeight = 240;

constexpr float kStatusBarHeight = 26; // the top screen's bar, with the logo
constexpr float kTabBarHeight = 30;
constexpr float kHintBarHeight = 26;
constexpr float kRowHeight = 44;
constexpr float kPadding = 8;
constexpr float kRadius = 8; // cards
constexpr float kRadiusSmall = 5;

// Icons are drawn at their own pixel size, next to text, or large on empty pages and in messages.
constexpr float kIcon = 16;
constexpr float kIconLarge = 48;

// The console font at these sizes stays readable on the screens' large pixels; nothing is smaller than kTextSmall.
constexpr float kTextLarge = 0.6f;  // titles: capitals 11 pixels high
constexpr float kTextNormal = 0.5f; // most text: capitals 9.5 pixels high
constexpr float kTextSmall = 0.45f; // second lines: capitals 8.5 pixels high
// The console font's button symbols next to text, a size up so they read at a glance: 14 pixels.
constexpr float kButtonGlyph = 0.6f;
} // namespace metric

} // namespace tz::gfx
