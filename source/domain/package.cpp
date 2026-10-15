// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "domain/package.hpp"

#include "core/path.hpp"

namespace tz
{

Expected<Package> Package::open(FileSystem& sd, std::string path, bool zip)
{
    Package package(sd, path);
    if (zip)
    {
        std::shared_ptr<Reader> file = TZ_TRY(sd.open(path, OpenMode::Read));
        package.zip_ = TZ_TRY(ZipArchive::open(std::move(file)));
    }
    else
    {
        package.listing_ = TZ_TRY(sd.list(path));
    }
    return package;
}

const DirEntry* Package::findInFolder(std::string_view name) const
{
    for (const DirEntry& entry : listing_)
        if (!entry.isDirectory && path::equalsIgnoreCase(entry.name, name))
            return &entry;
    return nullptr;
}

bool Package::has(std::string_view name) const
{
    return size(name).has_value();
}

std::optional<uint64_t> Package::size(std::string_view name) const
{
    if (zip_)
    {
        const ZipEntry* entry = zip_->find(name);
        return entry ? std::optional(entry->size) : std::nullopt;
    }
    const DirEntry* entry = findInFolder(name);
    return entry ? std::optional(entry->size) : std::nullopt;
}

std::optional<uint32_t> Package::knownCrc(std::string_view name) const
{
    if (!zip_)
        return std::nullopt;
    const ZipEntry* entry = zip_->find(name);
    return entry ? std::optional(entry->crc32) : std::nullopt;
}

Expected<Bytes> Package::read(std::string_view name, uint64_t maxSize) const
{
    if (zip_)
    {
        const ZipEntry* entry = zip_->find(name);
        if (!entry)
            return fail(Errc::NotFound, std::string(name));
        return zip_->read(*entry, maxSize);
    }
    const DirEntry* entry = findInFolder(name);
    if (!entry)
        return fail(Errc::NotFound, std::string(name));
    return sd_->readFile(path::join(path_, entry->name), maxSize);
}

Expected<std::unique_ptr<StreamReader>> Package::stream(std::string_view name) const
{
    if (zip_)
    {
        const ZipEntry* entry = zip_->find(name);
        if (!entry)
            return fail(Errc::NotFound, std::string(name));
        return zip_->openStream(*entry);
    }
    const DirEntry* entry = findInFolder(name);
    if (!entry)
        return fail(Errc::NotFound, std::string(name));
    std::shared_ptr<Reader> file = TZ_TRY(sd_->open(path::join(path_, entry->name), OpenMode::Read));
    return std::unique_ptr<StreamReader>(std::make_unique<ReaderStream>(std::move(file)));
}

std::vector<std::string> Package::files() const
{
    std::vector<std::string> names;
    if (zip_)
    {
        for (const ZipEntry& entry : zip_->entries())
            if (!entry.isDirectory())
                names.push_back(entry.name);
    }
    else
    {
        for (const DirEntry& entry : listing_)
            if (!entry.isDirectory)
                names.push_back(entry.name);
    }
    return names;
}

} // namespace tz
