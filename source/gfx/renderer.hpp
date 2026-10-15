// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "gfx/palette.hpp"

#ifdef TZ_DEVTOOLS
#include "core/image.hpp"
#endif

namespace tz::gfx
{

enum class Screen : uint8_t
{
    Top,
    Bottom,
};

bool init();
void exit();

// A frame: beginFrame, then each screen's drawing between beginScreen calls, then endFrame. The frame waits for
// the screens' refresh, so the app runs at 60 frames a second.
void beginFrame();
void beginScreen(Screen screen, Color clear = color::kBackground);
void endFrame();

Screen currentScreen();
float screenWidth();

// Drawing outside the rectangle is cut off until popClip. Clips nest (each is cut to the one before).
void pushClip(float x, float y, float width, float height);
void popClip();

// Seconds since start, for animations.
float time();

#ifdef TZ_DEVTOOLS
// The screens as the last frame left them, laid out as on the console: the top screen, the bottom one centred under
// it. Call it after beginFrame, which waits for that frame to reach the screens.
Image captureScreens();
#endif

} // namespace tz::gfx
