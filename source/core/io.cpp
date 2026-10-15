// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/io.hpp"

#include "core/path.hpp"

#include <algorithm>

namespace tz
{

const char* errcName(Errc code)
{
    switch (code)
    {
    case Errc::NotFound:
        return "not found";
    case Errc::InvalidData:
        return "invalid data";
    case Errc::TooLarge:
        return "too large";
    case Errc::Unsupported:
        return "unsupported";
    case Errc::Io:
        return "I/O error";
    case Errc::NoSpace:
        return "no space";
    case Errc::OutOfMemory:
        return "out of memory";
    case Errc::Cancelled:
        return "cancelled";
    case Errc::NoNetwork:
        return "no network";
    case Errc::Network:
        return "network error";
    case Errc::Tls:
        return "TLS error";
    case Errc::Http:
        return "HTTP error";
    case Errc::Busy:
        return "busy";
    case Errc::NotAllowed:
        return "not allowed";
    }
    return "error";
}

Expected<Bytes> Reader::readAll(uint64_t maxSize)
{
    const uint64_t length = size();
    if (length > maxSize)
        return fail(Errc::TooLarge, "file of " + std::to_string(length) + " bytes");
    Bytes data(static_cast<size_t>(length));
    TZ_CHECK(readAt(0, data));
    return data;
}

Expected<void> MemoryReader::readAt(uint64_t offset, MutableByteSpan out)
{
    if (offset > data_.size() || out.size() > data_.size() - offset)
        return fail(Errc::InvalidData, "read past the end");
    std::copy_n(data_.begin() + static_cast<ptrdiff_t>(offset), out.size(), out.begin());
    return {};
}

Expected<void> StreamReader::seek(uint64_t target)
{
    if (target < position())
        return fail(Errc::Unsupported, "this stream can't go back");
    uint8_t scratch[1024];
    while (position() < target)
    {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(sizeof(scratch), target - position()));
        const size_t got = TZ_TRY(read(MutableByteSpan(scratch, chunk)));
        if (got == 0)
            return fail(Errc::InvalidData, "seek past the end");
    }
    return {};
}

Expected<void> StreamReader::readExactly(MutableByteSpan out)
{
    size_t done = 0;
    while (done < out.size())
    {
        const size_t got = TZ_TRY(read(out.subspan(done)));
        if (got == 0)
            return fail(Errc::InvalidData, "stream ended early");
        done += got;
    }
    return {};
}

Expected<size_t> ReaderStream::read(MutableByteSpan out)
{
    const uint64_t total = reader_->size();
    if (position_ >= total)
        return size_t{0};
    const size_t length = static_cast<size_t>(std::min<uint64_t>(out.size(), total - position_));
    TZ_CHECK(reader_->readAt(position_, out.first(length)));
    position_ += length;
    return length;
}

Expected<void> ReaderStream::seek(uint64_t target)
{
    if (target > reader_->size())
        return fail(Errc::InvalidData, "seek past the end");
    position_ = target;
    return {};
}

Expected<Bytes> FileSystem::readFile(std::string_view path, uint64_t maxSize)
{
    auto file = TZ_TRY(open(path, OpenMode::Read));
    return file->readAll(maxSize);
}

Expected<void> FileSystem::writeFile(std::string_view path, ByteSpan data)
{
    if (exists(path))
        TZ_CHECK(removeFile(path));
    TZ_CHECK(createFile(path, data.size()));
    auto file = TZ_TRY(open(path, OpenMode::ReadWrite));
    if (!data.empty())
        TZ_CHECK(file->writeAt(0, data));
    return file->flush();
}

Expected<void> FileSystem::createDirectories(std::string_view path)
{
    std::string current;
    for (std::string_view part : path::split(path))
    {
        current += '/';
        current += part;
        TZ_CHECK(createDirectory(current));
    }
    return {};
}

} // namespace tz
