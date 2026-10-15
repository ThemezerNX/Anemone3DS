// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "gfx/palette.hpp"

#include <cstdint>

namespace tz::gfx
{

// The app's icons: Lucide's, as on the Themezer website (gfx/icons, made by tools/make_icons.py), drawn tinted.
enum class Icon : uint16_t
{
    Theme,
    Splash,
    Badge,
    Download,
    Install,
    Check,
    CheckCircle,
    Music,
    MusicOnly,
    NoMusic,
    Shuffle,
    Trash,
    Eye,
    Search,
    SearchEmpty,
    Qr,
    Settings,
    Info,
    Refresh,
    Restore,
    Sort,
    Globe,
    Wifi,
    WifiOff,
    Warning,
    Error,
    Update,
    Save,
    Power,
    Screens,
    ScreenTop,
    ScreenBottom,
    Square,
    SquareCheck,
    Battery,
    BatteryCharging,
    Images,
    Count,
};

bool loadIcons();
void freeIcons();
// Sharpest at metric::kIcon and metric::kIconLarge, the sizes they're drawn in; from a whole pixel.
void icon(Icon which, float x, float y, float size, Color color);

// Themezer's logo (gfx/brand, made by tools/make_brand.mjs), drawn at its own size.
enum class Brand : uint8_t
{
    Logo, // the controller and the wordmark, 22 pixels high
    Icon, // the controller, 64 pixels high
};
void brand(Brand which, float x, float y, float alpha = 1.0f);
float brandWidth(Brand which);
float brandHeight(Brand which);

void rect(float x, float y, float w, float h, Color color);
void gradientV(float x, float y, float w, float h, Color top, Color bottom);
void gradientH(float x, float y, float w, float h, Color left, Color right);
void roundedRect(float x, float y, float w, float h, float radius, Color color);
void circle(float cx, float cy, float radius, Color color);
void line(float x0, float y0, float x1, float y1, float width, Color color);

// A soft shadow around a rectangle.
void shadow(float x, float y, float w, float h, float size);
// A turning arc, for things that are loading.
void spinner(float cx, float cy, float radius, Color color);
void progressBar(float x, float y, float w, float h, float fraction, Color track, Color fill);
// Covers the whole current screen.
void fill(Color color);

} // namespace tz::gfx
