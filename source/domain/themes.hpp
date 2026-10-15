// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Installing themes into the HOME Menu's theme cache. See home_theme.hpp for the files.

#pragma once

#include "core/io.hpp"
#include "domain/home_theme.hpp"

#include <functional>
#include <vector>

namespace tz::themes
{

// A theme body's size and CRC-32, to recognise it again; zips have both in their directory, so recognising the
// installed theme costs nothing for them.
struct Fingerprint
{
    uint32_t size = 0;
    uint32_t crc = 0;

    bool operator==(const Fingerprint&) const = default;
};

// What the HOME Menu shows now.
struct Current
{
    home::ThemeDescriptor selected;
    bool shuffle = false;
    // The custom theme's body, or the body in each used shuffle slot.
    std::vector<Fingerprint> bodies;

    // The HOME Menu reloads its theme when the app closes, but with the settings it had: from a basic colour or a
    // shuffle, a newly installed theme only shows after a restart.
    bool restartToShow() const { return shuffle || selected.type == home::type::kBasic; }
    bool hasCustomTheme() const { return !bodies.empty(); }
};

Expected<Current> readCurrent(FileSystem& themeExt, FileSystem& homeExt);

// What the HOME Menu can take: an LZ11 body that fits the cache, a BCSTM that fits the music cache.
Expected<void> validateBody(ByteSpan body);
Expected<void> validateBgm(ByteSpan bgm);

// Creates the theme files the HOME Menu makes the first time a theme is chosen, for consoles where that never
// happened, all zero as the HOME Menu makes them. It refuses (InvalidData) when a file is missing or has the wrong
// size while a cached theme may be in use, since that theme would be lost.
Expected<void> ensureFiles(FileSystem& themeExt, FileSystem& homeExt);

// Installs a theme. An empty bgm installs it without music.
Expected<void> install(FileSystem& themeExt, FileSystem& homeExt, ByteSpan body, ByteSpan bgm);

// Replaces the music of the installed custom theme, keeping its body. NotFound when no custom theme is installed.
Expected<void> installMusic(FileSystem& themeExt, FileSystem& homeExt, ByteSpan bgm);

struct ThemeFiles
{
    Bytes body;
    Bytes bgm; // empty for no music
};

// Installs 2 to 10 themes as a shuffle. Themes are loaded one at a time, so all ten never sit in memory together.
// progress gets (themes done, count) and can cancel by returning false.
using ShuffleLoader = std::function<Expected<ThemeFiles>(int index)>;
using Progress = std::function<bool(int done, int total)>;
Expected<void> installShuffle(FileSystem& themeExt, FileSystem& homeExt, int count, const ShuffleLoader& load,
                              const Progress& progress);

// Switches the HOME Menu back to its default theme. The Theme Shop's list of downloaded themes is kept.
Expected<void> resetToDefault(FileSystem& themeExt, FileSystem& homeExt);

// The installed custom theme's files, to save it on the SD card. NotFound when there's none.
Expected<ThemeFiles> readInstalled(FileSystem& themeExt, FileSystem& homeExt);

// The music flag in a decompressed body (offset 5): whether the theme plays its BGM.
constexpr size_t kBodyBgmFlagOffset = 5;

} // namespace tz::themes
