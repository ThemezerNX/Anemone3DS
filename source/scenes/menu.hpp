// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "ui/overlays.hpp"

#include <vector>

namespace tz::scenes
{

// The menu (START): what the current page offers, then the app's own: the settings, an update, About, and closing
// the app.
void openMenu(std::vector<ui::Action> pageActions);

void openSettings();
void openAbout();

} // namespace tz::scenes
