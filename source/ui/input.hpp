// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <3ds.h>

namespace tz::ui
{

// One frame of input. The D-pad and the circle pad count as the same directions.
struct Input
{
    u32 down = 0;   // pressed this frame
    u32 held = 0;   // held now
    u32 repeat = 0; // pressed this frame, or repeating while held (directions)

    // The touch screen, in bottom screen pixels.
    bool touching = false;
    float x = 0;
    float y = 0;
    bool touchStarted = false;
    bool touchEnded = false;
    float startX = 0;
    float startY = 0;
    // Released without having moved: a press at (x, y).
    bool tap = false;
    // Moved since it started: a drag, not a tap. dx/dy is this frame's movement.
    bool dragging = false;
    float dx = 0;
    float dy = 0;
    // Pixels a second when the touch ended, for flinging lists.
    float velocityY = 0;

    bool pressed(u32 keys) const { return (down & keys) != 0; }
    bool repeated(u32 keys) const { return (repeat & keys) != 0; }
    bool tapIn(float left, float top, float width, float height) const
    {
        return tap && x >= left && x < left + width && y >= top && y < top + height;
    }

    // Acts as if keys were pressed, for buttons on the screen that do what a key does.
    void press(u32 keys)
    {
        down |= keys;
        repeat |= keys;
    }
    // Once something has used the touch, the rest of the frame shouldn't.
    void consumeTouch()
    {
        tap = false;
        touchStarted = false;
    }
    void clear() { *this = Input{}; }
};

// The buttons held and the touch screen, as the hardware reports them.
struct RawInput
{
    u32 held = 0;
    bool touching = false;
    float x = 0;
    float y = 0;
};

RawInput readHardware();

// Turns the raw state of each frame into presses, repeats, taps and drags.
class InputReader
{
public:
    Input read(const RawInput& raw, float dt);

private:
    u32 lastHeld_ = 0;
    float heldTime_[4] = {};   // up, down, left, right
    float nextRepeat_[4] = {};
    bool touching_ = false;
    float startX_ = 0;
    float startY_ = 0;
    float lastX_ = 0;
    float lastY_ = 0;
    bool moved_ = false;
    float velocity_ = 0;
};

} // namespace tz::ui
