// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "ui/input.hpp"

// The autopilot plays a script of button presses and touches from the SD card and saves screenshots, so the screens
// can be looked at in an emulator, or on a console, with no one at the buttons. It's only in builds made with
// `make DEVTOOLS=1`; docs/DEVELOPMENT.md lists the script's commands.
namespace tz::autopilot
{

#ifdef TZ_DEVTOOLS
// Reads the script, when there is one.
void start();
// Adds the script's buttons and touches to the frame's.
void drive(ui::RawInput& input);
// Once the frame has begun: saves the screenshots the script asked for, of the frame before.
void frameStarted();
// The script has ended with quit.
bool quitting();
#else
inline void start() {}
inline void drive(ui::RawInput&) {}
inline void frameStarted() {}
inline bool quitting()
{
    return false;
}
#endif

} // namespace tz::autopilot
