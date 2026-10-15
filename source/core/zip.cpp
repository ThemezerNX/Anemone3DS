// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The zip format is PKWARE's APPNOTE.TXT. A zip ends with the end of central directory record (and before it, in a
// zip64 archive, a locator pointing at the zip64 version of that record), which says where the central directory is.
// The directory lists every file with its sizes, CRC, compression method and the offset of its local header; the
// file's data follows its local header. Everything is little endian.

#include "core/zip.hpp"

#include "core/path.hpp"
#include "core/utf.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace tz
{

namespace
{

constexpr uint32_t kEndSignature = 0x06054B50;
constexpr uint32_t kEnd64Signature = 0x06064B50;
constexpr uint32_t kEnd64LocatorSignature = 0x07064B50;
constexpr uint32_t kCentralSignature = 0x02014B50;
constexpr uint32_t kLocalSignature = 0x04034B50;

constexpr size_t kEndSize = 22;
constexpr size_t kEnd64Size = 56;
constexpr size_t kEnd64LocatorSize = 20;
constexpr size_t kCentralSize = 46;
constexpr size_t kLocalSize = 30;
constexpr size_t kMaxComment = 0xFFFF;

// A central directory this big would list hundreds of thousands of files; no zip of themes or badges comes close.
constexpr uint64_t kMaxDirectorySize = 16u << 20;
// Compressed data is read this much at a time.
constexpr size_t kChunk = 0x10000;
// Deflate can't do better than about 1032:1, so a bigger stated size is a corrupt directory, not a reason to allocate.
constexpr uint64_t kMaxDeflateRatio = 1032;

constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;

constexpr uint16_t kFlagEncrypted = 1 << 0;
constexpr uint16_t kFlagUtf8 = 1 << 11;

// Extra fields: zip64's 64-bit sizes and offset, and Info-ZIP's UTF-8 path for an entry whose name is in CP437.
constexpr uint16_t kExtraZip64 = 0x0001;
constexpr uint16_t kExtraUnicodePath = 0x7075;

// The MS-DOS directory attribute, in the low byte of the external attributes whichever system made the zip.
constexpr uint32_t kDosDirectory = 0x10;

// Code page 437 from 0x80 on, the encoding of names in zips that don't flag them as UTF-8.
constexpr char16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC,
    0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2,
    0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310,
    0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C,
    0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF,
    0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2,
    0x25A0, 0x00A0,
};

bool isAscii(ByteSpan text)
{
    return std::all_of(text.begin(), text.end(), [](uint8_t c) { return c < 0x80; });
}

bool isValidUtf8(ByteSpan text)
{
    const std::string_view view(reinterpret_cast<const char*>(text.data()), text.size());
    size_t index = 0;
    while (index < view.size())
    {
        const size_t start = index;
        if (utf::decodeNext(view, index) == 0xFFFD &&
            !(index - start == 3 && view.compare(start, 3, "\xEF\xBF\xBD") == 0))
            return false;
    }
    return true;
}

std::string decodeName(ByteSpan raw, bool flaggedUtf8)
{
    std::string name;
    name.reserve(raw.size());
    // Without the flag a name is CP437, except that macOS's Archive Utility writes UTF-8 without saying so; a name
    // with bytes above 0x7F that happens to be valid UTF-8 is far more likely to be that than CP437 box drawing.
    if (flaggedUtf8 || isAscii(raw) || isValidUtf8(raw))
    {
        const std::string_view view(reinterpret_cast<const char*>(raw.data()), raw.size());
        size_t index = 0;
        while (index < view.size())
            utf::appendUtf8(name, utf::decodeNext(view, index));
    }
    else
    {
        for (uint8_t c : raw)
            utf::appendUtf8(name, c < 0x80 ? char32_t{c} : char32_t{kCp437[c - 0x80]});
    }
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

std::string_view withoutLeadingSlashes(std::string_view name)
{
    while (!name.empty() && name.front() == '/')
        name.remove_prefix(1);
    return name;
}

// The resource forks macOS adds to the zips it makes, which aren't part of what was zipped.
bool isMacMetadata(std::string_view name)
{
    name = withoutLeadingSlashes(name);
    const size_t slash = name.find('/');
    return slash != std::string_view::npos && path::equalsIgnoreCase(name.substr(0, slash), "__MACOSX");
}

// zlib's raw inflate (zips have no zlib header), ended when it goes out of scope.
class Inflater
{
public:
    Inflater() = default;
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    ~Inflater()
    {
        if (ready_)
            inflateEnd(&stream_);
    }

    Expected<void> init()
    {
        if (inflateInit2(&stream_, -MAX_WBITS) != Z_OK)
            return fail(Errc::OutOfMemory, "inflateInit2");
        ready_ = true;
        return {};
    }

    Expected<void> reset()
    {
        if (inflateReset(&stream_) != Z_OK)
            return fail(Errc::InvalidData, "inflateReset");
        return {};
    }

    z_stream& stream() { return stream_; }

private:
    z_stream stream_{};
    bool ready_ = false;
};

Error inflateError(int status)
{
    if (status == Z_MEM_ERROR)
        return Error{Errc::OutOfMemory, status, "inflate"};
    return Error{Errc::InvalidData, status, "corrupt deflate data"};
}

// Where an entry's data starts: after its local header, whose name and extra field can differ in length from the
// central directory's.
Expected<uint64_t> dataOffset(Reader& reader, const ZipEntry& entry)
{
    if (entry.method != kMethodStored && entry.method != kMethodDeflated)
        return fail(Errc::Unsupported, "compression method " + std::to_string(entry.method));

    const uint64_t fileSize = reader.size();
    if (entry.localHeaderOffset > fileSize || fileSize - entry.localHeaderOffset < kLocalSize)
        return fail(Errc::InvalidData, "local header past the end of the zip");
    uint8_t header[kLocalSize];
    TZ_CHECK(reader.readAt(entry.localHeaderOffset, MutableByteSpan(header, sizeof(header))));
    if (readU32(header) != kLocalSignature)
        return fail(Errc::InvalidData, "no local header for " + entry.name);
    if (readU16(header + 6) & kFlagEncrypted)
        return fail(Errc::Unsupported, "encrypted zip entry");

    const uint64_t offset = entry.localHeaderOffset + kLocalSize + readU16(header + 26) + readU16(header + 28);
    if (offset > fileSize || fileSize - offset < entry.compressedSize)
        return fail(Errc::InvalidData, "zip entry data past the end of the zip");
    if (entry.method == kMethodStored && entry.compressedSize != entry.size)
        return fail(Errc::InvalidData, "stored zip entry with two sizes");
    if (entry.method == kMethodDeflated && entry.size > entry.compressedSize * kMaxDeflateRatio + 64)
        return fail(Errc::InvalidData, "impossible deflate ratio");
    return offset;
}

// An entry read as a stream: stored data straight from the zip, deflated data inflated a chunk at a time.
class EntryStream final : public StreamReader
{
public:
    EntryStream(std::shared_ptr<Reader> reader, const ZipEntry& entry, uint64_t offset)
        : reader_(std::move(reader)), offset_(offset), compressedSize_(entry.compressedSize), size_(entry.size),
          expectedCrc_(entry.crc32), deflated_(entry.method == kMethodDeflated)
    {
    }

    Expected<void> init()
    {
        if (deflated_)
        {
            TZ_CHECK(inflater_.init());
            input_.resize(static_cast<size_t>(std::min<uint64_t>(kChunk, std::max<uint64_t>(compressedSize_, 1))));
        }
        return {};
    }

    uint64_t size() const override { return size_; }
    uint64_t position() const override { return position_; }

    Expected<size_t> read(MutableByteSpan out) override
    {
        // zlib counts in 32 bits
        const auto wanted = static_cast<size_t>(std::min<uint64_t>({out.size(), size_ - position_, uint64_t{1} << 30}));
        if (wanted == 0)
            return size_t{0};

        size_t produced;
        if (deflated_)
        {
            produced = TZ_TRY(inflateInto(out.first(wanted)));
        }
        else
        {
            TZ_CHECK(reader_->readAt(offset_ + position_, out.first(wanted)));
            produced = wanted;
        }

        crc_ = crc32(out.first(produced), crc_);
        position_ += produced;
        if (position_ == size_ && crcValid_ && crc_ != expectedCrc_)
            return fail(Errc::InvalidData, "CRC mismatch");
        return produced;
    }

    Expected<void> seek(uint64_t target) override
    {
        if (target > size_)
            return fail(Errc::InvalidData, "seek past the end");
        if (!deflated_)
        {
            // the CRC only means something for a file read from start to end
            if (target != position_)
                crcValid_ = target == 0;
            if (target == 0)
                crc_ = 0;
            position_ = target;
            return {};
        }
        if (target < position_)
            TZ_CHECK(restart());
        uint8_t scratch[4096];
        while (position_ < target)
        {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(sizeof(scratch), target - position_));
            TZ_CHECK(read(MutableByteSpan(scratch, chunk)));
        }
        return {};
    }

private:
    Expected<void> restart()
    {
        TZ_CHECK(inflater_.reset());
        z_stream& stream = inflater_.stream();
        stream.next_in = nullptr;
        stream.avail_in = 0;
        consumed_ = 0;
        position_ = 0;
        crc_ = 0;
        crcValid_ = true;
        ended_ = false;
        return {};
    }

    // Fills out completely from the deflate stream; it was sized to end at the entry's size at most.
    Expected<size_t> inflateInto(MutableByteSpan out)
    {
        z_stream& stream = inflater_.stream();
        stream.next_out = out.data();
        stream.avail_out = static_cast<uInt>(out.size());
        while (stream.avail_out > 0 && !ended_)
        {
            if (stream.avail_in == 0)
            {
                if (consumed_ >= compressedSize_)
                    return fail(Errc::InvalidData, "deflate data ends early");
                const auto chunk = static_cast<size_t>(std::min<uint64_t>(input_.size(), compressedSize_ - consumed_));
                TZ_CHECK(reader_->readAt(offset_ + consumed_, MutableByteSpan(input_.data(), chunk)));
                consumed_ += chunk;
                stream.next_in = input_.data();
                stream.avail_in = static_cast<uInt>(chunk);
            }
            const int status = inflate(&stream, Z_NO_FLUSH);
            if (status == Z_STREAM_END)
                ended_ = true;
            else if (status != Z_OK && status != Z_BUF_ERROR)
                return fail(inflateError(status));
        }
        const size_t produced = out.size() - stream.avail_out;
        if (produced < out.size())
            return fail(Errc::InvalidData, "deflate data ends before the file does");
        return produced;
    }

    std::shared_ptr<Reader> reader_;
    uint64_t offset_;
    uint64_t compressedSize_;
    uint64_t size_;
    uint32_t expectedCrc_;
    bool deflated_;

    Inflater inflater_;
    Bytes input_;
    uint64_t consumed_ = 0; // compressed bytes handed to zlib
    bool ended_ = false;
    uint64_t position_ = 0;
    uint32_t crc_ = 0;
    bool crcValid_ = true;
};

struct EndRecord
{
    uint64_t entries = 0;
    uint64_t directorySize = 0;
    uint64_t directoryOffset = 0;
    // where the central directory has to end: at the zip64 end record when there is one, else at the end record
    uint64_t directoryEnd = 0;
};

// Finds the end of central directory record in the last 64 KiB of the zip (the most its comment can push it back),
// and the zip64 record when there is one.
Expected<EndRecord> readEndRecord(Reader& reader, Bytes& tail, uint64_t& tailStart)
{
    const uint64_t fileSize = reader.size();
    if (fileSize < kEndSize)
        return fail(Errc::InvalidData, "too small for a zip");
    const auto tailSize =
        static_cast<size_t>(std::min<uint64_t>(fileSize, kMaxComment + kEndSize + kEnd64LocatorSize));
    tailStart = fileSize - tailSize;
    tail.resize(tailSize);
    TZ_CHECK(reader.readAt(tailStart, tail));

    // The record is followed only by its comment. Prefer one whose comment reaches exactly to the end of the file;
    // otherwise accept one followed by something extra (padding some tools add).
    size_t found = SIZE_MAX;
    size_t fallback = SIZE_MAX;
    for (size_t i = tailSize - kEndSize + 1; i-- > 0;)
    {
        if (readU32(tail.data() + i) != kEndSignature)
            continue;
        const size_t commentEnd = i + kEndSize + readU16(tail.data() + i + 20);
        if (commentEnd == tailSize)
        {
            found = i;
            break;
        }
        if (commentEnd < tailSize && fallback == SIZE_MAX)
            fallback = i;
    }
    if (found == SIZE_MAX)
        found = fallback;
    if (found == SIZE_MAX)
        return fail(Errc::InvalidData, "no zip end of central directory");

    const uint8_t* end = tail.data() + found;
    uint32_t disk = readU16(end + 4);
    uint32_t directoryDisk = readU16(end + 6);
    uint64_t diskEntries = readU16(end + 8);
    EndRecord record;
    record.entries = readU16(end + 10);
    record.directorySize = readU32(end + 12);
    record.directoryOffset = readU32(end + 16);
    record.directoryEnd = tailStart + found;

    // Zip64 puts a locator right before the end record. Even small zips have one when they were written by a tool
    // that didn't know the sizes in advance.
    if (found >= kEnd64LocatorSize && readU32(end - kEnd64LocatorSize) == kEnd64LocatorSignature)
    {
        const uint8_t* locator = end - kEnd64LocatorSize;
        const uint64_t recordOffset = readU64(locator + 8);
        if (readU32(locator + 4) != 0 || readU32(locator + 16) > 1)
            return fail(Errc::Unsupported, "split zip");
        const uint64_t locatorOffset = tailStart + found - kEnd64LocatorSize;
        if (recordOffset > locatorOffset || locatorOffset - recordOffset < kEnd64Size)
            return fail(Errc::InvalidData, "zip64 end record out of place");

        uint8_t record64[kEnd64Size];
        if (recordOffset >= tailStart)
            std::memcpy(record64, tail.data() + (recordOffset - tailStart), kEnd64Size);
        else
            TZ_CHECK(reader.readAt(recordOffset, MutableByteSpan(record64, kEnd64Size)));
        if (readU32(record64) != kEnd64Signature)
            return fail(Errc::InvalidData, "no zip64 end record");

        disk = readU32(record64 + 16);
        directoryDisk = readU32(record64 + 20);
        diskEntries = readU64(record64 + 24);
        record.entries = readU64(record64 + 32);
        record.directorySize = readU64(record64 + 40);
        record.directoryOffset = readU64(record64 + 48);
        record.directoryEnd = recordOffset;
    }

    if (disk != 0 || directoryDisk != 0 || diskEntries != record.entries)
        return fail(Errc::Unsupported, "split zip");
    if (record.directorySize > record.directoryEnd || record.directorySize / kCentralSize < record.entries)
        return fail(Errc::InvalidData, "zip central directory doesn't fit");
    if (record.directorySize > kMaxDirectorySize)
        return fail(Errc::TooLarge, "zip central directory of " + std::to_string(record.directorySize) + " bytes");
    return record;
}

// Reads the zip64 extra field: only the values whose 32-bit fields are all ones are there, in this order.
void applyZip64(ByteSpan field, uint64_t& size, uint64_t& compressedSize, uint64_t& localHeaderOffset)
{
    size_t at = 0;
    for (uint64_t* value : {&size, &compressedSize, &localHeaderOffset})
    {
        if (*value != 0xFFFFFFFF)
            continue;
        if (at + 8 > field.size())
            return;
        *value = readU64(field.data() + at);
        at += 8;
    }
}

} // namespace

Expected<ZipArchive> ZipArchive::open(std::shared_ptr<Reader> reader)
{
    if (!reader)
        return fail(Errc::InvalidData, "no zip");

    Bytes tail;
    uint64_t tailStart = 0;
    const EndRecord end = TZ_TRY(readEndRecord(*reader, tail, tailStart));

    // A zip with something in front of it (a self-extractor) has offsets that are off by the size of that.
    const uint64_t expectedOffset = end.directoryEnd - end.directorySize;
    uint64_t shift = 0;
    if (end.directoryOffset > expectedOffset)
        return fail(Errc::InvalidData, "zip central directory overlaps its end");
    if (end.directoryOffset < expectedOffset && end.entries > 0)
    {
        uint8_t signature[4];
        TZ_CHECK(reader->readAt(expectedOffset, MutableByteSpan(signature, 4)));
        if (readU32(signature) == kCentralSignature)
            shift = expectedOffset - end.directoryOffset;
    }
    const uint64_t directoryOffset = end.directoryOffset + shift;

    Bytes buffer;
    ByteSpan directory;
    if (directoryOffset >= tailStart && directoryOffset + end.directorySize <= tailStart + tail.size())
    {
        directory = ByteSpan(tail).subspan(static_cast<size_t>(directoryOffset - tailStart),
                                           static_cast<size_t>(end.directorySize));
    }
    else
    {
        buffer.resize(static_cast<size_t>(end.directorySize));
        TZ_CHECK(reader->readAt(directoryOffset, buffer));
        directory = buffer;
    }

    ZipArchive archive;
    archive.reader_ = std::move(reader);
    archive.entries_.reserve(static_cast<size_t>(end.entries));
    size_t at = 0;
    for (uint64_t i = 0; i < end.entries; ++i)
    {
        if (directory.size() - at < kCentralSize || readU32(directory.data() + at) != kCentralSignature)
            return fail(Errc::InvalidData, "broken zip central directory");
        const uint8_t* header = directory.data() + at;
        const uint16_t flags = readU16(header + 8);
        const size_t nameLength = readU16(header + 28);
        const size_t extraLength = readU16(header + 30);
        const size_t commentLength = readU16(header + 32);
        const size_t recordSize = kCentralSize + nameLength + extraLength + commentLength;
        if (directory.size() - at < recordSize)
            return fail(Errc::InvalidData, "broken zip central directory");

        ZipEntry entry;
        entry.method = readU16(header + 10);
        entry.crc32 = readU32(header + 16);
        entry.compressedSize = readU32(header + 20);
        entry.size = readU32(header + 24);
        entry.localHeaderOffset = readU32(header + 42);
        const ByteSpan rawName(header + kCentralSize, nameLength);

        std::string unicodePath;
        ByteSpan extra(header + kCentralSize + nameLength, extraLength);
        while (extra.size() >= 4)
        {
            const uint16_t id = readU16(extra.data());
            const size_t length = std::min<size_t>(readU16(extra.data() + 2), extra.size() - 4);
            const ByteSpan field = extra.subspan(4, length);
            if (id == kExtraZip64)
            {
                applyZip64(field, entry.size, entry.compressedSize, entry.localHeaderOffset);
            }
            else if (id == kExtraUnicodePath && field.size() >= 5 && field[0] == 1 &&
                     readU32(field.data() + 1) == crc32(rawName))
            {
                // only when it was written for this very name, not left behind by a tool that renamed the entry
                unicodePath = decodeName(field.subspan(5), true);
            }
            extra = extra.subspan(4 + length);
        }

        entry.name = !unicodePath.empty() ? std::move(unicodePath) : decodeName(rawName, flags & kFlagUtf8);
        if ((readU32(header + 38) & kDosDirectory) && !entry.name.empty() && entry.name.back() != '/')
            entry.name += '/';
        entry.localHeaderOffset += shift;
        archive.entries_.push_back(std::move(entry));
        at += recordSize;
    }
    return archive;
}

const ZipEntry* ZipArchive::find(std::string_view name) const
{
    name = withoutLeadingSlashes(name);
    if (name.empty())
        return nullptr;

    const ZipEntry* best = nullptr;
    size_t bestDepth = SIZE_MAX;
    for (const ZipEntry& entry : entries_)
    {
        if (entry.isDirectory() || isMacMetadata(entry.name))
            continue;
        const std::string_view entryName = withoutLeadingSlashes(entry.name);
        if (path::equalsIgnoreCase(entryName, name))
            return &entry;
        // the same name in a folder: "Theme/body_LZ.bin" for "body_LZ.bin"
        if (entryName.size() > name.size() && entryName[entryName.size() - name.size() - 1] == '/' &&
            path::equalsIgnoreCase(entryName.substr(entryName.size() - name.size()), name))
        {
            const size_t depth = static_cast<size_t>(std::count(entryName.begin(), entryName.end(), '/'));
            if (depth < bestDepth)
            {
                best = &entry;
                bestDepth = depth;
            }
        }
    }
    return best;
}

Expected<Bytes> ZipArchive::read(const ZipEntry& entry, uint64_t maxSize) const
{
    if (entry.size > maxSize)
        return fail(Errc::TooLarge, entry.name + " is " + std::to_string(entry.size) + " bytes");
    const uint64_t offset = TZ_TRY(dataOffset(*reader_, entry));

    Bytes data(static_cast<size_t>(entry.size));
    if (entry.method == kMethodStored)
    {
        if (!data.empty())
            TZ_CHECK(reader_->readAt(offset, data));
    }
    else if (!data.empty())
    {
        Inflater inflater;
        TZ_CHECK(inflater.init());
        z_stream& stream = inflater.stream();
        Bytes input(static_cast<size_t>(std::min<uint64_t>(kChunk, std::max<uint64_t>(entry.compressedSize, 1))));
        uint64_t consumed = 0;
        size_t produced = 0;
        bool ended = false;
        while (!ended)
        {
            if (stream.avail_in == 0)
            {
                if (consumed >= entry.compressedSize)
                    return fail(Errc::InvalidData, "deflate data ends early");
                const auto chunk =
                    static_cast<size_t>(std::min<uint64_t>(input.size(), entry.compressedSize - consumed));
                TZ_CHECK(reader_->readAt(offset + consumed, MutableByteSpan(input.data(), chunk)));
                consumed += chunk;
                stream.next_in = input.data();
                stream.avail_in = static_cast<uInt>(chunk);
            }
            // zlib counts in 32 bits
            const size_t room = std::min<size_t>(data.size() - produced, size_t{1} << 30);
            stream.next_out = data.data() + produced;
            stream.avail_out = static_cast<uInt>(room);
            const int status = inflate(&stream, Z_NO_FLUSH);
            produced += room - stream.avail_out;
            if (status == Z_STREAM_END)
                ended = true;
            else if (status == Z_BUF_ERROR && produced == data.size())
                return fail(Errc::InvalidData, "deflate data longer than its file");
            else if (status != Z_OK && status != Z_BUF_ERROR)
                return fail(inflateError(status));
        }
        if (produced != data.size())
            return fail(Errc::InvalidData, "deflate data shorter than its file");
    }

    if (crc32(data) != entry.crc32)
        return fail(Errc::InvalidData, "CRC mismatch in " + entry.name);
    return data;
}

Expected<std::unique_ptr<StreamReader>> ZipArchive::openStream(const ZipEntry& entry) const
{
    const uint64_t offset = TZ_TRY(dataOffset(*reader_, entry));
    auto stream = std::make_unique<EntryStream>(reader_, entry, offset);
    TZ_CHECK(stream->init());
    return std::unique_ptr<StreamReader>(std::move(stream));
}

uint32_t crc32(ByteSpan data, uint32_t crc)
{
    const uint8_t* bytes = data.data();
    size_t left = data.size();
    uLong value = crc;
    while (left > 0)
    {
        const size_t chunk = std::min<size_t>(left, size_t{1} << 30);
        value = ::crc32(value, bytes, static_cast<uInt>(chunk));
        bytes += chunk;
        left -= chunk;
    }
    return static_cast<uint32_t>(value);
}

} // namespace tz
