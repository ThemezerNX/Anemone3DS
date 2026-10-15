// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The settings, in /3ds/Themezer/config.json.

#pragma once

#include "core/io.hpp"
#include "domain/library.hpp"

namespace tz
{

constexpr const char* kAppFolder = "/3ds/Themezer";
constexpr const char* kConfigPath = "/3ds/Themezer/config.json";
constexpr const char* kCacheFolder = "/3ds/Themezer/cache";
constexpr const char* kBadgeBackupFolder = "/3ds/Themezer/badge-backup";

struct Config
{
    bool backgroundMusic = true;
    bool checkForUpdates = true;
    library::Sort themeSort = library::Sort::Name;
    library::Sort splashSort = library::Sort::Name;
    int lastTab = 0;

    // Missing or broken settings keep their defaults.
    static Config load(FileSystem& sd);
    Expected<void> save(FileSystem& sd) const;
};

} // namespace tz
