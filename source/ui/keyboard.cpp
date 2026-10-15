// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "ui/keyboard.hpp"

#include <3ds.h>
#include <vector>

namespace tz::ui
{

std::optional<std::string> keyboard(const std::string& hint, const std::string& initial, int maxLength,
                                    const std::string& cancel, const std::string& confirm)
{
    SwkbdState state;
    swkbdInit(&state, SWKBD_TYPE_NORMAL, 2, maxLength);
    swkbdSetHintText(&state, hint.c_str());
    swkbdSetInitialText(&state, initial.c_str());
    swkbdSetButton(&state, SWKBD_BUTTON_LEFT, cancel.c_str(), false);
    swkbdSetButton(&state, SWKBD_BUTTON_RIGHT, confirm.c_str(), true);
    swkbdSetFeatures(&state, SWKBD_PREDICTIVE_INPUT);
    // the UTF-8 result can take up to four bytes for each character
    std::vector<char> text(static_cast<size_t>(maxLength) * 4 + 1, '\0');
    const SwkbdButton button = swkbdInputText(&state, text.data(), text.size());
    if (button != SWKBD_BUTTON_CONFIRM)
        return std::nullopt;
    return std::string(text.data());
}

} // namespace tz::ui
