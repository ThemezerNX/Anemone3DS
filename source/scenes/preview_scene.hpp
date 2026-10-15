// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/io.hpp"
#include "scenes/previews.hpp"
#include "ui/scene.hpp"

#include <functional>
#include <memory>
#include <string>

namespace tz::scenes
{

// What a full-size preview shows.
struct PreviewSource
{
    std::string title;
    uint64_t key = 0;
    Previews* previews = nullptr; // the page's previews; it outlives the scene
    Previews::Loader loader;
    Lane lane = Lane::Io;
    // Opens the music, on a worker; empty without music.
    std::function<Expected<std::unique_ptr<StreamReader>>()> music;
    Lane musicLane = Lane::Io;
};

// A preview on both screens at full size, as the HOME Menu would show it, with the theme's music playing.
std::unique_ptr<ui::Scene> makePreviewScene(PreviewSource source);

} // namespace tz::scenes
