// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "sys/fs.hpp"

#include "core/path.hpp"
#include "core/utf.hpp"

#include <array>
#include <cstring>

namespace tz
{

namespace
{

// FS result descriptions (the low 10 bits) that mean "already there".
constexpr u32 kAlreadyExists[] = {190, 185};

bool alreadyExists(Result result)
{
    for (u32 description : kAlreadyExists)
        if (static_cast<u32>(R_DESCRIPTION(result)) == description)
            return true;
    return false;
}

struct Utf16Path
{
    std::u16string text;
    FS_Path fs() const
    {
        return FS_Path{PATH_UTF16, static_cast<u32>((text.size() + 1) * sizeof(char16_t)), text.c_str()};
    }
};

Utf16Path makePath(std::string_view path)
{
    return Utf16Path{utf::toUtf16(path.empty() ? std::string_view("/") : path)};
}

class ArchiveFile final : public File
{
public:
    ArchiveFile(Handle handle, uint64_t size) : handle_(handle), size_(size) {}
    ~ArchiveFile() override { FSFILE_Close(handle_); }

    uint64_t size() const override { return size_; }

    Expected<void> readAt(uint64_t offset, MutableByteSpan out) override
    {
        size_t done = 0;
        while (done < out.size())
        {
            u32 read = 0;
            const Result result = FSFILE_Read(handle_, &read, offset + done, out.data() + done, out.size() - done);
            if (R_FAILED(result))
                return fail(fsError(result, "read"));
            if (read == 0)
                return fail(Errc::InvalidData, "file ended early");
            done += read;
        }
        return {};
    }

    Expected<void> writeAt(uint64_t offset, ByteSpan data) override
    {
        size_t done = 0;
        while (done < data.size())
        {
            u32 written = 0;
            const Result result =
                FSFILE_Write(handle_, &written, offset + done, data.data() + done, data.size() - done, 0);
            if (R_FAILED(result))
                return fail(fsError(result, "write"));
            if (written == 0)
                return fail(Errc::Io, "nothing written");
            done += written;
        }
        if (offset + data.size() > size_)
            size_ = offset + data.size();
        return {};
    }

    Expected<void> flush() override
    {
        const Result result = FSFILE_Flush(handle_);
        if (R_FAILED(result))
            return fail(fsError(result, "flush"));
        return {};
    }

private:
    Handle handle_;
    uint64_t size_;
};

} // namespace

Error fsError(Result result, std::string_view what)
{
    // FS's "in use": a file another process has open, as the HOME Menu has its badges when the app runs as an applet
    constexpr int kFileInUse = 230;
    if (R_MODULE(result) == RM_FS && R_DESCRIPTION(result) == kFileInUse)
        return Error{Errc::Busy, static_cast<int32_t>(result), std::string(what)};
    Errc code = Errc::Io;
    switch (R_SUMMARY(result))
    {
    case RS_NOTFOUND:
        code = Errc::NotFound;
        break;
    case RS_OUTOFRESOURCE:
        code = Errc::NoSpace;
        break;
    case RS_NOTSUPPORTED:
        code = Errc::Unsupported;
        break;
    case RS_INVALIDSTATE:
    case RS_STATUSCHANGED:
        code = Errc::Busy;
        break;
    case RS_WRONGARG:
    case RS_INVALIDARG:
        code = Errc::InvalidData;
        break;
    default:
        break;
    }
    return Error{code, static_cast<int32_t>(result), std::string(what)};
}

Result createExtdata(u32 extdataId, u32 maxDirectories, u32 maxFiles)
{
    // an all-zero SMDH, as other homebrew that creates extdata uses
    static u8 smdh[0x36C0];
    FS_ExtSaveDataInfo info{};
    info.mediaType = MEDIATYPE_SD;
    info.saveId = extdataId;
    return FSUSER_CreateExtSaveData(info, maxDirectories, maxFiles, U64_MAX, sizeof(smdh), smdh);
}

Expected<std::unique_ptr<Archive>> Archive::openSdmc()
{
    FS_Archive archive;
    const Result result = FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(result))
        return fail(fsError(result, "open the SD card"));
    return std::unique_ptr<Archive>(new Archive(archive, true));
}

Expected<std::unique_ptr<Archive>> Archive::openExtdata(u32 extdataId)
{
    const u32 path[3] = {MEDIATYPE_SD, extdataId, 0};
    FS_Archive archive;
    const Result result = FSUSER_OpenArchive(&archive, ARCHIVE_EXTDATA, FS_Path{PATH_BINARY, sizeof(path), path});
    if (R_FAILED(result))
        return fail(fsError(result, "open extdata"));
    return std::unique_ptr<Archive>(new Archive(archive, false));
}

Archive::~Archive()
{
    FSUSER_CloseArchive(archive_);
}

Expected<std::unique_ptr<File>> Archive::open(std::string_view path, OpenMode mode)
{
    u32 flags = FS_OPEN_READ;
    if (mode != OpenMode::Read)
        flags |= FS_OPEN_WRITE;
    if (mode == OpenMode::Create)
        flags |= FS_OPEN_CREATE;

    const Utf16Path fsPath = makePath(path);
    Handle handle;
    Result result = FSUSER_OpenFile(&handle, archive_, fsPath.fs(), flags, 0);
    if (R_FAILED(result))
        return fail(fsError(result, std::string("open ") + std::string(path)));

    u64 size = 0;
    result = FSFILE_GetSize(handle, &size);
    if (R_FAILED(result))
    {
        FSFILE_Close(handle);
        return fail(fsError(result, "size"));
    }
    return std::make_unique<ArchiveFile>(handle, size);
}

Expected<void> Archive::createFile(std::string_view path, uint64_t size)
{
    const Result result = FSUSER_CreateFile(archive_, makePath(path).fs(), 0, size);
    if (R_FAILED(result))
        return fail(fsError(result, std::string("create ") + std::string(path)));
    return {};
}

Expected<void> Archive::removeFile(std::string_view path)
{
    const Result result = FSUSER_DeleteFile(archive_, makePath(path).fs());
    if (R_FAILED(result))
        return fail(fsError(result, std::string("delete ") + std::string(path)));
    return {};
}

Expected<void> Archive::createDirectory(std::string_view path)
{
    if (path.empty() || path == "/")
        return {};
    const Result result = FSUSER_CreateDirectory(archive_, makePath(path).fs(), 0);
    if (R_FAILED(result) && !alreadyExists(result))
        return fail(fsError(result, std::string("create folder ") + std::string(path)));
    return {};
}

Expected<void> Archive::removeDirectory(std::string_view path, bool recursive)
{
    const Result result = recursive ? FSUSER_DeleteDirectoryRecursively(archive_, makePath(path).fs())
                                    : FSUSER_DeleteDirectory(archive_, makePath(path).fs());
    if (R_FAILED(result))
        return fail(fsError(result, std::string("delete folder ") + std::string(path)));
    return {};
}

Expected<void> Archive::rename(std::string_view from, std::string_view to)
{
    auto entry = TZ_TRY(stat(from));
    const Utf16Path source = makePath(from);
    const Utf16Path target = makePath(to);
    const Result result = entry.isDirectory
                              ? FSUSER_RenameDirectory(archive_, source.fs(), archive_, target.fs())
                              : FSUSER_RenameFile(archive_, source.fs(), archive_, target.fs());
    if (R_FAILED(result))
        return fail(fsError(result, "rename"));
    return {};
}

Expected<std::vector<DirEntry>> Archive::list(std::string_view path)
{
    Handle dir;
    Result result = FSUSER_OpenDirectory(&dir, archive_, makePath(path).fs());
    if (R_FAILED(result))
        return fail(fsError(result, std::string("open folder ") + std::string(path)));

    std::vector<DirEntry> entries;
    std::array<FS_DirectoryEntry, 32> batch;
    for (;;)
    {
        u32 count = 0;
        result = FSDIR_Read(dir, &count, batch.size(), batch.data());
        if (R_FAILED(result) || count == 0)
            break;
        for (u32 i = 0; i < count; ++i)
        {
            const FS_DirectoryEntry& entry = batch[i];
            size_t length = 0;
            while (length < std::size(entry.name) && entry.name[length])
                ++length;
            entries.push_back(DirEntry{
                utf::fromUtf16(std::span(reinterpret_cast<const char16_t*>(entry.name), length)),
                (entry.attributes & FS_ATTRIBUTE_DIRECTORY) != 0,
                entry.fileSize,
            });
        }
    }
    FSDIR_Close(dir);
    if (R_FAILED(result))
        return fail(fsError(result, "read folder"));
    return entries;
}

Expected<DirEntry> Archive::stat(std::string_view path)
{
    const Utf16Path fsPath = makePath(path);
    Handle handle;
    Result result = FSUSER_OpenFile(&handle, archive_, fsPath.fs(), FS_OPEN_READ, 0);
    if (R_SUCCEEDED(result))
    {
        u64 size = 0;
        FSFILE_GetSize(handle, &size);
        FSFILE_Close(handle);
        return DirEntry{std::string(path::filename(path)), false, size};
    }
    Handle dir;
    const Result dirResult = FSUSER_OpenDirectory(&dir, archive_, fsPath.fs());
    if (R_SUCCEEDED(dirResult))
    {
        FSDIR_Close(dir);
        return DirEntry{std::string(path::filename(path)), true, 0};
    }
    return fail(fsError(result, std::string("stat ") + std::string(path)));
}

uint64_t Archive::modifiedTime(std::string_view path)
{
    if (!sdmc_)
        return 0;
    // ARCHIVE_ACTION_GET_TIMESTAMP takes the path as UTF-16 with its terminator, and returns the time
    const Utf16Path fsPath = makePath(path);
    u64 timestamp = 0;
    const Result result =
        FSUSER_ControlArchive(archive_, ARCHIVE_ACTION_GET_TIMESTAMP, const_cast<char16_t*>(fsPath.text.c_str()),
                              (fsPath.text.size() + 1) * sizeof(char16_t), &timestamp, sizeof(timestamp));
    return R_SUCCEEDED(result) ? timestamp : 0;
}

} // namespace tz
