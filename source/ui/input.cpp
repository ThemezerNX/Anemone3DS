// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "ui/input.hpp"

#include <cmath>

namespace tz::ui
{

namespace
{
constexpr u32 kDirections[4] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
constexpr float kRepeatDelay = 0.32f;
constexpr float kRepeatInterval = 0.075f;
// held longer, the repeat speeds up for long lists
constexpr float kFastAfter = 1.4f;
constexpr float kFastInterval = 0.035f;
// a touch that moves less than this is a tap
constexpr float kTapSlop = 8.0f;
} // namespace

RawInput readHardware()
{
    hidScanInput();
    RawInput raw;
    raw.held = hidKeysHeld();
    if (raw.held & KEY_TOUCH)
    {
        touchPosition touch;
        hidTouchRead(&touch);
        raw.touching = true;
        raw.x = touch.px;
        raw.y = touch.py;
    }
    return raw;
}

Input InputReader::read(const RawInput& raw, float dt)
{
    Input input;
    input.held = raw.held;
    input.down = raw.held & ~lastHeld_;
    input.repeat = input.down;
    lastHeld_ = raw.held;

    for (int i = 0; i < 4; ++i)
    {
        const u32 key = kDirections[i];
        if (input.down & key)
        {
            heldTime_[i] = 0;
            nextRepeat_[i] = kRepeatDelay;
        }
        else if (input.held & key)
        {
            heldTime_[i] += dt;
            if (heldTime_[i] >= nextRepeat_[i])
            {
                input.repeat |= key;
                nextRepeat_[i] += heldTime_[i] > kFastAfter ? kFastInterval : kRepeatInterval;
            }
        }
    }

    if (raw.touching)
    {
        const float x = raw.x;
        const float y = raw.y;
        if (!touching_)
        {
            touching_ = true;
            moved_ = false;
            startX_ = lastX_ = x;
            startY_ = lastY_ = y;
            velocity_ = 0;
            input.touchStarted = true;
        }
        if (!moved_ && (std::fabs(x - startX_) > kTapSlop || std::fabs(y - startY_) > kTapSlop))
            moved_ = true;
        input.dx = x - lastX_;
        input.dy = y - lastY_;
        if (dt > 0)
            velocity_ = velocity_ * 0.6f + (input.dy / dt) * 0.4f;
        lastX_ = x;
        lastY_ = y;
        input.touching = true;
        input.x = x;
        input.y = y;
    }
    else if (touching_)
    {
        touching_ = false;
        input.touchEnded = true;
        input.x = lastX_;
        input.y = lastY_;
        input.tap = !moved_;
        input.velocityY = moved_ ? velocity_ : 0;
    }
    input.dragging = touching_ && moved_;
    input.startX = startX_;
    input.startY = startY_;
    return input;
}

} // namespace tz::ui
