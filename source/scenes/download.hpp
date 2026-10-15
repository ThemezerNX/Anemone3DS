// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Downloading a theme, splash or badge set to the SD card, and offering to install it.

#pragma once

#include "scenes/install.hpp"

#include <functional>
#include <string>

namespace tz::scenes
{

struct Saved
{
    ContentKind kind = ContentKind::Unknown;
    std::string path; // on the SD card
    uint64_t size = 0;
};

// Downloads a zip and saves it in the folder for what it turns out to be (/Themes, /Splashes or /Badges), under the
// server's name for it, else fallbackName. A name already taken gets a number. Shows the progress, and errors.
void downloadToSd(std::string url, std::string title, std::string fallbackName, std::function<void(Saved)> done);

// Asks whether to install what was just saved, and does.
void offerInstall(const Saved& saved, const std::string& name);

} // namespace tz::scenes
