// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/io.hpp"

#include <3ds.h>

namespace tz
{

// A FileSystem on an FS archive. All its methods may be called from any thread.
class Archive final : public FileSystem
{
public:
    // The SD card.
    static Expected<std::unique_ptr<Archive>> openSdmc();
    // An extdata archive on the SD card.
    static Expected<std::unique_ptr<Archive>> openExtdata(u32 extdataId);

    ~Archive() override;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    Expected<std::unique_ptr<File>> open(std::string_view path, OpenMode mode) override;
    Expected<void> createFile(std::string_view path, uint64_t size) override;
    Expected<void> removeFile(std::string_view path) override;
    Expected<void> createDirectory(std::string_view path) override;
    Expected<void> removeDirectory(std::string_view path, bool recursive) override;
    Expected<void> rename(std::string_view from, std::string_view to) override;
    Expected<std::vector<DirEntry>> list(std::string_view path) override;
    Expected<DirEntry> stat(std::string_view path) override;

    // When a file on the SD card was last changed, in milliseconds since 2000; 0 when unknown.
    uint64_t modifiedTime(std::string_view path);

    FS_Archive handle() const { return archive_; }

private:
    explicit Archive(FS_Archive archive, bool sdmc) : archive_(archive), sdmc_(sdmc) {}

    FS_Archive archive_;
    bool sdmc_;
};

// The error for a failed FS call.
Error fsError(Result result, std::string_view what);

// Creates an extdata archive the way the system would for a title that has none yet.
Result createExtdata(u32 extdataId, u32 maxDirectories, u32 maxFiles);

} // namespace tz
