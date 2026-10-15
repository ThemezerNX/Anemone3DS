// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The themes and splashes on the SD card: each a folder or a zip in /Themes or /Splashes.

#pragma once

#include "core/io.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz::library
{

enum class Kind : uint8_t
{
    Theme,
    Splash,
};

constexpr const char* kThemesRoot = "/Themes";
constexpr const char* kSplashesRoot = "/Splashes";
constexpr const char* kBadgesRoot = "/Badges";

const char* root(Kind kind);

// Where an item is: enough to find it again and to tell when it has changed.
struct Source
{
    std::string path;     // "/Themes/Name.zip"
    std::string fileName; // "Name.zip"
    bool zip = false;
    uint64_t size = 0; // of a zip; 0 for a folder

    bool operator==(const Source&) const = default;
};

// A file's size and CRC-32. crc is 0 while unknown (files in folders, until read).
struct FileId
{
    uint32_t size = 0;
    uint32_t crc = 0;
    bool operator==(const FileId&) const = default;
};

using Icon = std::array<uint16_t, 48 * 48>; // tiled RGB565, as in SMDH files

// What the item's files say.
struct Info
{
    std::string name;
    std::string author;
    std::string description;
    std::shared_ptr<const Icon> icon; // null without one
    bool hasSmdh = false;
    bool hasPreview = false;
    bool hasMusic = false; // themes: bgm.bcstm
    // themes: body_LZ.bin; splashes: splash.bin and splashbottom.bin. size 0 when the file isn't there.
    FileId body;
    FileId top;
    FileId bottom;

    // A theme needs a body, a splash at least one screen.
    bool usable(Kind kind) const { return kind == Kind::Theme ? body.size != 0 : (top.size != 0 || bottom.size != 0); }
};

// Themezer names its downloads "<name> Theme by <author> (Themezer-<id>)", or Splash or Badge Set for the others:
// the name and the author in such a file name (without .zip), for items whose files don't name them.
struct DownloadName
{
    std::string name;
    std::string author;
};
std::optional<DownloadName> parseDownloadName(std::string_view stem);

struct Item
{
    Source source;
    std::optional<Info> info; // until it's loaded, the item shows its file name

    // The name to show: from its info, else from a Themezer file name, else the file name without ".zip".
    std::string displayName() const;
    // The author from its info, else from a Themezer file name; empty when unknown.
    std::string author() const;
};

// The folders and zips in root, unsorted. Hidden entries (starting with '.') are skipped. A missing root is made.
Expected<std::vector<Source>> scan(FileSystem& sd, std::string_view root);

// Reads an item's info.smdh and looks at which files it has. language picks the SMDH title (CFG_Language).
Expected<Info> readInfo(FileSystem& sd, const Source& source, Kind kind, int language);

// A file's CRC-32 when it isn't known yet (themes and splashes in folders), for recognising installed ones.
Expected<uint32_t> computeCrc(FileSystem& sd, const Source& source, std::string_view name);

// The cache of item infos, so the library shows names and icons at once on the next start.
// Entries match items by path, zip-ness and size.
std::vector<std::pair<Source, Info>> loadCache(FileSystem& sd, std::string_view path, Kind kind);
Expected<void> saveCache(FileSystem& sd, std::string_view path, Kind kind, const std::vector<Item>& items);

enum class Sort : uint8_t
{
    Name,
    Author,
    FileName,
};

void sort(std::vector<Item>& items, Sort sort);

// Natural order: "Theme 2" before "Theme 10", ignoring case.
int compareNatural(std::string_view a, std::string_view b);

} // namespace tz::library
