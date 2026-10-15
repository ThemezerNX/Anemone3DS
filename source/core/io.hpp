// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tz
{

// Something to read from at any offset: a file, a file inside a zip, a buffer.
class Reader
{
public:
    virtual ~Reader() = default;
    virtual uint64_t size() const = 0;
    // Reads exactly out.size() bytes, or fails.
    virtual Expected<void> readAt(uint64_t offset, MutableByteSpan out) = 0;

    Expected<Bytes> readAll(uint64_t maxSize);
};

class MemoryReader final : public Reader
{
public:
    explicit MemoryReader(Bytes data) : data_(std::move(data)) {}
    uint64_t size() const override { return data_.size(); }
    Expected<void> readAt(uint64_t offset, MutableByteSpan out) override;

private:
    Bytes data_;
};

// Something read from start to end: a file inside a zip, a download still in progress.
class StreamReader
{
public:
    virtual ~StreamReader() = default;
    // The total size, when known up front; 0 otherwise.
    virtual uint64_t size() const = 0;
    // Reads up to out.size() bytes, blocking until some are there. Returns 0 at the end.
    virtual Expected<size_t> read(MutableByteSpan out) = 0;
    // Skips ahead (or back, when the source allows). The default reads and drops.
    virtual Expected<void> seek(uint64_t position);
    virtual uint64_t position() const = 0;

    // Fills out completely, or fails (InvalidData when the stream ends first).
    Expected<void> readExactly(MutableByteSpan out);
};

// A StreamReader over a Reader, which can seek freely.
class ReaderStream final : public StreamReader
{
public:
    explicit ReaderStream(std::shared_ptr<Reader> reader) : reader_(std::move(reader)) {}
    uint64_t size() const override { return reader_->size(); }
    Expected<size_t> read(MutableByteSpan out) override;
    Expected<void> seek(uint64_t position) override;
    uint64_t position() const override { return position_; }

private:
    std::shared_ptr<Reader> reader_;
    uint64_t position_ = 0;
};

// A file that can be written in place, for the fixed-size files in the HOME Menu's storage.
class File : public Reader
{
public:
    virtual Expected<void> writeAt(uint64_t offset, ByteSpan data) = 0;
    virtual Expected<void> flush() = 0;
};

enum class OpenMode : uint8_t
{
    Read,
    ReadWrite,
    // read and write, creating an empty file first if there is none (not on extdata)
    Create,
};

struct DirEntry
{
    std::string name;
    bool isDirectory = false;
    uint64_t size = 0;
};

// One archive: the SD card, or one of the HOME Menu's extdata archives. Paths are absolute within it, in UTF-8,
// separated by '/'.
class FileSystem
{
public:
    virtual ~FileSystem() = default;

    virtual Expected<std::unique_ptr<File>> open(std::string_view path, OpenMode mode) = 0;
    // Creates a file of this size; extdata files can only be made this way, and never grow.
    virtual Expected<void> createFile(std::string_view path, uint64_t size) = 0;
    virtual Expected<void> removeFile(std::string_view path) = 0;
    // Succeeds when the directory is already there.
    virtual Expected<void> createDirectory(std::string_view path) = 0;
    virtual Expected<void> removeDirectory(std::string_view path, bool recursive) = 0;
    virtual Expected<void> rename(std::string_view from, std::string_view to) = 0;
    virtual Expected<std::vector<DirEntry>> list(std::string_view path) = 0;
    // NotFound when there is nothing there.
    virtual Expected<DirEntry> stat(std::string_view path) = 0;

    bool exists(std::string_view path) { return stat(path).has_value(); }

    // Whole small files.
    Expected<Bytes> readFile(std::string_view path, uint64_t maxSize);
    // Replaces the file with exactly this content (creating it).
    Expected<void> writeFile(std::string_view path, ByteSpan data);
    // Creates every missing directory along the path.
    Expected<void> createDirectories(std::string_view path);
};

} // namespace tz
