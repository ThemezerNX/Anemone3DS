// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Installing a theme or a splash from the SD card, with its progress shown and its errors explained. Used by the
// library pages and after downloads.

#pragma once

#include "domain/library.hpp"
#include "domain/splashes.hpp"

#include <functional>
#include <string>

namespace tz::scenes
{

enum class ThemeMode : uint8_t
{
    Full,
    NoMusic,
    MusicOnly,
};

// done(true, the installed body) once installed; done(false, {}) after the error was shown.
void installTheme(const library::Source& source, const std::string& name, ThemeMode mode,
                  std::function<void(bool, library::FileId)> done);
void installSplash(const library::Source& source, const std::string& name, splashes::Screens screens,
                   std::function<void(bool)> done);

// What kind of thing a zip is, from the files in it.
enum class ContentKind : uint8_t
{
    Theme,
    Splash,
    Badges,
    Unknown,
};
ContentKind kindOf(const std::vector<std::string>& files);

} // namespace tz::scenes
