// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "memory_fs.hpp"

#include "core/path.hpp"

#include <algorithm>

namespace test
{

using tz::Errc;
using tz::fail;

class MemoryFile final : public tz::File
{
public:
    MemoryFile(MemoryFs& fs, std::shared_ptr<tz::Bytes> data, bool writable)
        : fs_(fs), data_(std::move(data)), writable_(writable)
    {
    }

    uint64_t size() const override { return data_->size(); }

    tz::Expected<void> readAt(uint64_t offset, tz::MutableByteSpan out) override
    {
        if (offset > data_->size() || out.size() > data_->size() - offset)
            return fail(Errc::Io, "read past the end");
        std::copy_n(data_->begin() + static_cast<ptrdiff_t>(offset), out.size(), out.begin());
        return {};
    }

    tz::Expected<void> writeAt(uint64_t offset, tz::ByteSpan in) override
    {
        if (!writable_)
            return fail(Errc::NotAllowed, "not opened for writing");
        if (fs_.writes_++ >= fs_.failAfter_)
            return fail(Errc::Io, "injected write failure");
        if (offset + in.size() > data_->size())
        {
            if (fs_.fixedSize_)
                return fail(Errc::Io, "write past the end of a fixed-size file");
            data_->resize(static_cast<size_t>(offset + in.size()));
        }
        std::copy(in.begin(), in.end(), data_->begin() + static_cast<ptrdiff_t>(offset));
        return {};
    }

    tz::Expected<void> flush() override { return {}; }

private:
    MemoryFs& fs_;
    std::shared_ptr<tz::Bytes> data_;
    bool writable_;
};

tz::Expected<std::unique_ptr<tz::File>> MemoryFs::open(std::string_view path, tz::OpenMode mode)
{
    auto it = files_.find(std::string(path));
    if (it == files_.end())
    {
        if (mode != tz::OpenMode::Create)
            return fail(Errc::NotFound, std::string(path));
        if (fixedSize_)
            return fail(Errc::NotAllowed, "files can't be created by opening them here");
        if (!directories_.contains(std::string(tz::path::parent(path))))
            return fail(Errc::NotFound, "no directory for " + std::string(path));
        it = files_.emplace(std::string(path), std::make_shared<tz::Bytes>()).first;
    }
    return std::make_unique<MemoryFile>(*this, it->second, mode != tz::OpenMode::Read);
}

tz::Expected<void> MemoryFs::createFile(std::string_view path, uint64_t size)
{
    if (files_.contains(std::string(path)))
        return fail(Errc::InvalidData, "already exists");
    if (!directories_.contains(std::string(tz::path::parent(path))))
        return fail(Errc::NotFound, "no directory for " + std::string(path));
    files_.emplace(std::string(path), std::make_shared<tz::Bytes>(static_cast<size_t>(size), 0));
    return {};
}

tz::Expected<void> MemoryFs::removeFile(std::string_view path)
{
    if (!files_.erase(std::string(path)))
        return fail(Errc::NotFound, std::string(path));
    return {};
}

tz::Expected<void> MemoryFs::createDirectory(std::string_view path)
{
    if (!directories_.contains(std::string(tz::path::parent(path))))
        return fail(Errc::NotFound, "no parent for " + std::string(path));
    directories_.insert(std::string(path));
    return {};
}

tz::Expected<void> MemoryFs::removeDirectory(std::string_view path, bool recursive)
{
    const std::string prefix = std::string(path) + "/";
    const bool hasChildren =
        std::any_of(files_.begin(), files_.end(), [&](const auto& f) { return f.first.starts_with(prefix); }) ||
        std::any_of(directories_.begin(), directories_.end(), [&](const auto& d) { return d.starts_with(prefix); });
    if (hasChildren && !recursive)
        return fail(Errc::InvalidData, "not empty");
    std::erase_if(files_, [&](const auto& f) { return f.first.starts_with(prefix); });
    std::erase_if(directories_, [&](const auto& d) { return d.starts_with(prefix); });
    if (!directories_.erase(std::string(path)))
        return fail(Errc::NotFound, std::string(path));
    return {};
}

tz::Expected<void> MemoryFs::rename(std::string_view from, std::string_view to)
{
    auto it = files_.find(std::string(from));
    if (it == files_.end())
        return fail(Errc::NotFound, std::string(from));
    if (files_.contains(std::string(to)))
        return fail(Errc::InvalidData, "target exists");
    files_[std::string(to)] = it->second;
    files_.erase(it);
    return {};
}

tz::Expected<std::vector<tz::DirEntry>> MemoryFs::list(std::string_view path)
{
    if (!directories_.contains(std::string(path)))
        return fail(Errc::NotFound, std::string(path));
    std::vector<tz::DirEntry> entries;
    for (const auto& [name, data] : files_)
        if (tz::path::parent(name) == path)
            entries.push_back({std::string(tz::path::filename(name)), false, data->size()});
    for (const std::string& dir : directories_)
        if (dir != "/" && tz::path::parent(dir) == path)
            entries.push_back({std::string(tz::path::filename(dir)), true, 0});
    return entries;
}

tz::Expected<tz::DirEntry> MemoryFs::stat(std::string_view path)
{
    if (auto it = files_.find(std::string(path)); it != files_.end())
        return tz::DirEntry{std::string(tz::path::filename(path)), false, it->second->size()};
    if (directories_.contains(std::string(path)))
        return tz::DirEntry{std::string(tz::path::filename(path)), true, 0};
    return fail(Errc::NotFound, std::string(path));
}

void MemoryFs::put(const std::string& path, tz::Bytes data)
{
    std::string dir(tz::path::parent(path));
    // make the directories on the way
    std::string current;
    for (std::string_view part : tz::path::split(dir))
    {
        current += '/';
        current += part;
        directories_.insert(current);
    }
    files_[path] = std::make_shared<tz::Bytes>(std::move(data));
}

} // namespace test
