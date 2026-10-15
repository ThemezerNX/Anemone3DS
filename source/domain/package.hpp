// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/io.hpp"
#include "core/zip.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tz
{

// A theme, splash or badge set on the SD card: a folder of files, or a zip of them. Files are found by name,
// case-insensitively; in a zip also inside a folder (themes are often zipped in one).
class Package
{
public:
    static Expected<Package> open(FileSystem& sd, std::string path, bool zip);

    bool isZip() const { return zip_.has_value(); }
    const std::string& path() const { return path_; }

    bool has(std::string_view name) const;
    // The size of a file without reading it.
    std::optional<uint64_t> size(std::string_view name) const;
    // The CRC-32 of a file without reading it: known for files in zips.
    std::optional<uint32_t> knownCrc(std::string_view name) const;

    Expected<Bytes> read(std::string_view name, uint64_t maxSize) const;
    Expected<std::unique_ptr<StreamReader>> stream(std::string_view name) const;

    // Every file in it, as a path inside the package.
    std::vector<std::string> files() const;

private:
    Package(FileSystem& sd, std::string path) : sd_(&sd), path_(std::move(path)) {}
    const DirEntry* findInFolder(std::string_view name) const;

    FileSystem* sd_;
    std::string path_;
    std::optional<ZipArchive> zip_;
    std::vector<DirEntry> listing_; // for a folder
};

} // namespace tz
