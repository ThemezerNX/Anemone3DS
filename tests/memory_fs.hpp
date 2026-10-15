// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// A FileSystem held in memory, for testing the code that edits the SD card and the HOME Menu's storage.

#pragma once

#include "core/io.hpp"

#include <map>
#include <memory>
#include <set>

namespace test
{

class MemoryFs final : public tz::FileSystem
{
public:
    // Like extdata: files keep the size they were created with, writes past the end fail, and OpenMode::Create
    // is refused.
    explicit MemoryFs(bool fixedSize = false) : fixedSize_(fixedSize) {}

    tz::Expected<std::unique_ptr<tz::File>> open(std::string_view path, tz::OpenMode mode) override;
    tz::Expected<void> createFile(std::string_view path, uint64_t size) override;
    tz::Expected<void> removeFile(std::string_view path) override;
    tz::Expected<void> createDirectory(std::string_view path) override;
    tz::Expected<void> removeDirectory(std::string_view path, bool recursive) override;
    tz::Expected<void> rename(std::string_view from, std::string_view to) override;
    tz::Expected<std::vector<tz::DirEntry>> list(std::string_view path) override;
    tz::Expected<tz::DirEntry> stat(std::string_view path) override;

    // Direct access for tests.
    void put(const std::string& path, tz::Bytes data);
    tz::Bytes& data(const std::string& path) { return *files_.at(path); }
    bool hasFile(const std::string& path) const { return files_.contains(path); }
    size_t writes() const { return writes_; }

    // Makes the next writes fail, to test error handling.
    void failWritesAfter(size_t count) { failAfter_ = writes_ + count; }

private:
    friend class MemoryFile;
    bool fixedSize_;
    std::map<std::string, std::shared_ptr<tz::Bytes>> files_;
    std::set<std::string> directories_{"/"};
    size_t writes_ = 0;
    size_t failAfter_ = static_cast<size_t>(-1);
};

} // namespace test
