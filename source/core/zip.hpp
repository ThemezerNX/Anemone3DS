// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Reads zip archives through their central directory: listing a zip costs one read of its end, and a file in it one
// read of its local header plus its data, inflated as it is read.

#pragma once

#include "core/io.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tz
{

struct ZipEntry
{
    std::string name; // the path inside the zip, '/' separated, UTF-8
    uint64_t compressedSize = 0;
    uint64_t size = 0; // uncompressed
    uint32_t crc32 = 0;
    uint16_t method = 0; // 0 stored, 8 deflated
    uint64_t localHeaderOffset = 0;

    bool isDirectory() const { return !name.empty() && name.back() == '/'; }
};

class ZipArchive
{
public:
    // Reads the central directory. Fails with InvalidData when there is none, and Unsupported for split archives.
    // Entries that use something other than stored or deflated data are listed, and fail when read.
    static Expected<ZipArchive> open(std::shared_ptr<Reader> reader);

    const std::vector<ZipEntry>& entries() const { return entries_; }

    // A file by name, case-insensitively: the exact path first, otherwise the least deeply nested file with that
    // name, since themes are often zipped inside a folder of their own. nullptr when there is none.
    const ZipEntry* find(std::string_view name) const;

    // The whole file, decompressed and checked against its CRC. TooLarge when it is bigger than maxSize.
    Expected<Bytes> read(const ZipEntry& entry, uint64_t maxSize) const;

    // The file as a stream, decompressed while it is read; for big files read a part at a time (music).
    // Seeking forward is supported, backward only to the start.
    Expected<std::unique_ptr<StreamReader>> openStream(const ZipEntry& entry) const;

private:
    std::shared_ptr<Reader> reader_;
    std::vector<ZipEntry> entries_;
};

// The CRC-32 zips use.
uint32_t crc32(ByteSpan data, uint32_t crc = 0);

} // namespace tz
