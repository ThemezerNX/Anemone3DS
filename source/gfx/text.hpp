// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Text in the console's own font, which covers every language the console shows plus its button symbols.

#pragma once

#include "gfx/palette.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tz::gfx
{

enum class Align : uint8_t
{
    Left,
    Center,
    Right,
};

void initText();
void exitText();
void beginTextFrame();

float textWidth(std::string_view text, float size);
float lineHeight(float size);
// The y to draw a line at so the middle of its capitals is at middle: what centres text in a box and lines it up
// with icons centred on the same middle.
float textTop(float middle, float size);
// The middle of the capitals of a line drawn at top.
float textMiddle(float top, float size);

// One line, with y at its top, cut short with "…" to fit maxWidth (0 for no limit). x is the left edge, the centre
// or the right edge depending on align. It's drawn from a whole pixel, which keeps it sharp. Returns the width drawn.
float drawText(std::string_view text, float x, float y, float size, Color color, Align align = Align::Left,
               float maxWidth = 0);

// Wrapped to width, at most maxLines lines (0 for no limit; the last one ends in "…" when cut).
// Returns the height drawn.
float drawWrapped(std::string_view text, float x, float y, float width, float size, Color color, int maxLines = 0,
                  Align align = Align::Left, float lineSpacing = 1.0f);

std::string ellipsize(std::string_view text, float maxWidth, float size);
std::vector<std::string> wrap(std::string_view text, float width, float size, int maxLines = 0);

// The console font's button symbols, for hints. It has none for START and SELECT.
namespace glyph
{
constexpr const char* kA = "\uE000";
constexpr const char* kB = "\uE001";
constexpr const char* kX = "\uE002";
constexpr const char* kY = "\uE003";
constexpr const char* kL = "\uE004";
constexpr const char* kR = "\uE005";
constexpr const char* kDpad = "\uE006";
constexpr const char* kDpadUp = "\uE079";
constexpr const char* kDpadDown = "\uE07A";
constexpr const char* kDpadLeft = "\uE07B";
constexpr const char* kDpadRight = "\uE07C";
constexpr const char* kDpadUpDown = "\uE07D";
constexpr const char* kDpadLeftRight = "\uE07E";
constexpr const char* kCirclePad = "\uE077";
constexpr const char* kHome = "\uE073";
} // namespace glyph

} // namespace tz::gfx
