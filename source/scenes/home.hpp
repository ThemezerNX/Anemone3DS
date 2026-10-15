// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "ui/scene.hpp"

#include <memory>

namespace tz::scenes
{

// The app's main scene: tabs for the themes, splashes and badges on the SD card and for Themezer.
std::unique_ptr<ui::Scene> makeHome();

} // namespace tz::scenes
