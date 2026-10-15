// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/zip.hpp"
#include "test.hpp"

#include <zlib.h>

#include <random>
#include <string>

using namespace tz;

namespace
{

// A small zip writer for the tests: stored or deflated files, with the knobs the reader has to cope with.
struct FileSpec
{
    std::string name;
    Bytes data;
    bool deflate = true;
    uint16_t flags = 0;
    uint16_t method = 0xFFFF;  // overrides the method written, keeping the data as it is
    bool zip64Extra = false;   // sizes and offset as 0xFFFFFFFF with the real ones in a zip64 extra field
    Bytes extra;               // more extra fields in the central directory
    uint32_t externalAttributes = 0;
    bool badCrc = false;
};

struct ZipOptions
{
    std::string comment;
    size_t prefix = 0;  // bytes before the zip, as in a self-extracting archive
    bool zip64End = false;
    uint16_t disk = 0;
    std::string trailer; // after the comment
};

void put16(Bytes& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(Bytes& out, uint32_t v)
{
    put16(out, static_cast<uint16_t>(v));
    put16(out, static_cast<uint16_t>(v >> 16));
}

void put64(Bytes& out, uint64_t v)
{
    put32(out, static_cast<uint32_t>(v));
    put32(out, static_cast<uint32_t>(v >> 32));
}

Bytes deflateRaw(const Bytes& data)
{
    z_stream stream{};
    deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&stream, static_cast<uLong>(data.size())) + 16);
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

Bytes makeZip(const std::vector<FileSpec>& files, const ZipOptions& options = {})
{
    Bytes zip(options.prefix, 0x42);
    Bytes directory;
    for (const FileSpec& file : files)
    {
        const Bytes stored = file.deflate ? deflateRaw(file.data) : file.data;
        const uint16_t method = file.method != 0xFFFF ? file.method : (file.deflate ? 8 : 0);
        uint32_t crc = static_cast<uint32_t>(::crc32(0, file.data.data(), static_cast<uInt>(file.data.size())));
        if (file.badCrc)
            crc ^= 1;
        const uint64_t offset = zip.size() - options.prefix; // offsets are from the start of the zip itself

        put32(zip, 0x04034B50);
        put16(zip, 20);
        put16(zip, file.flags);
        put16(zip, method);
        put32(zip, 0);
        put32(zip, crc);
        put32(zip, static_cast<uint32_t>(stored.size()));
        put32(zip, static_cast<uint32_t>(file.data.size()));
        put16(zip, static_cast<uint16_t>(file.name.size()));
        put16(zip, 4); // a local extra field the central directory doesn't have
        zip.insert(zip.end(), file.name.begin(), file.name.end());
        put32(zip, 0x0004CAFE);
        zip.insert(zip.end(), stored.begin(), stored.end());

        Bytes extra;
        if (file.zip64Extra)
        {
            put16(extra, 0x0001);
            put16(extra, 24);
            put64(extra, file.data.size());
            put64(extra, stored.size());
            put64(extra, offset);
        }
        extra.insert(extra.end(), file.extra.begin(), file.extra.end());

        put32(directory, 0x02014B50);
        put16(directory, 0x031E);
        put16(directory, 20);
        put16(directory, file.flags);
        put16(directory, method);
        put32(directory, 0);
        put32(directory, crc);
        put32(directory, file.zip64Extra ? 0xFFFFFFFF : static_cast<uint32_t>(stored.size()));
        put32(directory, file.zip64Extra ? 0xFFFFFFFF : static_cast<uint32_t>(file.data.size()));
        put16(directory, static_cast<uint16_t>(file.name.size()));
        put16(directory, static_cast<uint16_t>(extra.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, file.externalAttributes);
        put32(directory, file.zip64Extra ? 0xFFFFFFFF : static_cast<uint32_t>(offset));
        directory.insert(directory.end(), file.name.begin(), file.name.end());
        directory.insert(directory.end(), extra.begin(), extra.end());
    }

    const uint64_t directoryOffset = zip.size() - options.prefix;
    zip.insert(zip.end(), directory.begin(), directory.end());
    if (options.zip64End)
    {
        const uint64_t recordOffset = zip.size() - options.prefix;
        put32(zip, 0x06064B50);
        put64(zip, 44);
        put16(zip, 45);
        put16(zip, 45);
        put32(zip, 0);
        put32(zip, 0);
        put64(zip, files.size());
        put64(zip, files.size());
        put64(zip, directory.size());
        put64(zip, directoryOffset);
        put32(zip, 0x07064B50);
        put32(zip, 0);
        put64(zip, recordOffset);
        put32(zip, 1);
    }
    put32(zip, 0x06054B50);
    put16(zip, options.disk);
    put16(zip, options.disk);
    put16(zip, options.zip64End ? 0xFFFF : static_cast<uint16_t>(files.size()));
    put16(zip, options.zip64End ? 0xFFFF : static_cast<uint16_t>(files.size()));
    put32(zip, options.zip64End ? 0xFFFFFFFF : static_cast<uint32_t>(directory.size()));
    put32(zip, options.zip64End ? 0xFFFFFFFF : static_cast<uint32_t>(directoryOffset));
    put16(zip, static_cast<uint16_t>(options.comment.size()));
    zip.insert(zip.end(), options.comment.begin(), options.comment.end());
    zip.insert(zip.end(), options.trailer.begin(), options.trailer.end());
    return zip;
}

Bytes text(std::string_view s)
{
    return Bytes(s.begin(), s.end());
}

Bytes compressible(size_t size, uint32_t seed)
{
    std::mt19937 rng(seed);
    Bytes data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<uint8_t>(rng() % 16 == 0 ? rng() : i / 64);
    return data;
}

Expected<ZipArchive> openZip(Bytes zip)
{
    return ZipArchive::open(std::make_shared<MemoryReader>(std::move(zip)));
}

} // namespace

TEST(zip_crc32)
{
    CHECK_EQ(crc32(text("123456789")), uint32_t(0xCBF43926));
    CHECK_EQ(crc32(text("6789"), crc32(text("12345"))), uint32_t(0xCBF43926));
    CHECK_EQ(crc32({}), uint32_t(0));
}

TEST(zip_read_files)
{
    const Bytes body = compressible(200000, 1);
    const Bytes zip = makeZip({
        {"info.smdh", text("stored file"), false},
        {"Theme/", {}, false},
        {"Theme/body_LZ.bin", body},
        {"Theme/Deeper/body_LZ.bin", text("too deep")},
        {"empty.txt", {}, true},
    });
    auto archive = openZip(zip);
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 5);
    CHECK_EQ(archive->entries()[0].name, std::string("info.smdh"));
    CHECK_EQ(archive->entries()[0].method, uint16_t(0));
    CHECK(archive->entries()[1].isDirectory());
    CHECK_EQ(archive->entries()[2].size, uint64_t(body.size()));
    CHECK(archive->entries()[2].compressedSize < body.size());

    auto smdh = archive->read(archive->entries()[0], 100);
    REQUIRE(smdh.has_value());
    CHECK(*smdh == text("stored file"));

    // by name: exactly (ignoring case), else the least deeply nested file of that name
    const ZipEntry* found = archive->find("BODY_lz.BIN");
    REQUIRE(found != nullptr);
    CHECK_EQ(found->name, std::string("Theme/body_LZ.bin"));
    auto data = archive->read(*found, body.size());
    REQUIRE(data.has_value());
    CHECK(*data == body);
    CHECK(archive->find("/INFO.smdh") == &archive->entries()[0]);
    CHECK(archive->find("Deeper/body_LZ.bin") == &archive->entries()[3]);
    CHECK(archive->find("theme") == nullptr); // directories aren't files
    CHECK(archive->find("dy_LZ.bin") == nullptr);
    CHECK(archive->find("") == nullptr);

    auto empty = archive->read(*archive->find("empty.txt"), 0);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    CHECK(archive->read(*found, body.size() - 1).error().code == Errc::TooLarge);
}

TEST(zip_find_skips_mac_metadata)
{
    const Bytes zip = makeZip({
        {"__MACOSX/body_LZ.bin", text("resource fork")},
        {"__MACOSX/Theme/._body_LZ.bin", text("resource fork")},
        {"Theme/body_LZ.bin", text("the body")},
    });
    auto archive = openZip(zip);
    REQUIRE(archive.has_value());
    const ZipEntry* found = archive->find("body_LZ.bin");
    REQUIRE(found != nullptr);
    CHECK_EQ(found->name, std::string("Theme/body_LZ.bin"));
}

TEST(zip_names)
{
    Bytes unicodePath;
    const std::string original = "old name.png";
    const std::string better = "t\xC3\xA9l\xC3\xA9phone.png"; // téléphone
    put16(unicodePath, 0x7075);
    put16(unicodePath, static_cast<uint16_t>(5 + better.size()));
    unicodePath.push_back(1);
    put32(unicodePath, crc32(text(original)));
    unicodePath.insert(unicodePath.end(), better.begin(), better.end());

    Bytes staleUnicodePath = unicodePath;
    staleUnicodePath[5] ^= 0xFF; // the CRC of another name

    const Bytes zip = makeZip({
        // CP437: 0x82 is é, 0xC9 is ╔, 0xE1 is ß
        {"caf\x82 \xC9\xE1.png", text("1")},
        // flagged UTF-8
        {"\xE3\x83\x86\xE3\x83\xBC\xE3\x83\x9E.png", text("2"), true, 1 << 11},
        // UTF-8 without the flag, as macOS writes it
        {"Pok\xC3\xA9mon.png", text("3")},
        {original, text("4"), true, 0, 0xFFFF, false, unicodePath},
        {original, text("5"), true, 0, 0xFFFF, false, staleUnicodePath},
        {"Windows\\folder\\file.png", text("6")},
        {"folder by attribute", {}, false, 0, 0xFFFF, false, {}, 0x10},
    });
    auto archive = openZip(zip);
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 7);
    CHECK_EQ(archive->entries()[0].name, std::string("caf\xC3\xA9 \xE2\x95\x94\xC3\x9F.png"));
    CHECK_EQ(archive->entries()[1].name, std::string("\xE3\x83\x86\xE3\x83\xBC\xE3\x83\x9E.png"));
    CHECK_EQ(archive->entries()[2].name, std::string("Pok\xC3\xA9mon.png"));
    CHECK_EQ(archive->entries()[3].name, better);
    CHECK_EQ(archive->entries()[4].name, original);
    CHECK_EQ(archive->entries()[5].name, std::string("Windows/folder/file.png"));
    CHECK(archive->entries()[6].isDirectory());
    CHECK(archive->find("file.png") == &archive->entries()[5]);
}

TEST(zip_comment_and_prefix)
{
    ZipOptions options;
    options.comment = std::string(60000, 'c') + "PK\x05\x06 not a record";
    options.prefix = 1000;
    const Bytes zip = makeZip({{"a.txt", text("alpha")}, {"b.txt", text("beta"), false}}, options);
    auto archive = openZip(zip);
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 2);
    auto b = archive->read(*archive->find("b.txt"), 100);
    REQUIRE(b.has_value());
    CHECK(*b == text("beta"));

    // padding after the zip
    ZipOptions padded;
    padded.trailer = std::string(100, '\0');
    auto archive2 = openZip(makeZip({{"a.txt", text("alpha")}}, padded));
    REQUIRE(archive2.has_value());
    auto a = archive2->read(archive2->entries()[0], 100);
    REQUIRE(a.has_value());
    CHECK(*a == text("alpha"));
}

TEST(zip_zip64)
{
    ZipOptions options;
    options.zip64End = true;
    FileSpec big{"big.bin", compressible(70000, 9)};
    big.zip64Extra = true;
    FileSpec small{"small.txt", text("small"), false};
    small.zip64Extra = true;
    auto archive = openZip(makeZip({big, small, {"plain.txt", text("plain")}}, options));
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 3);
    CHECK_EQ(archive->entries()[0].size, uint64_t(70000));
    auto data = archive->read(archive->entries()[0], 70000);
    REQUIRE(data.has_value());
    CHECK(*data == big.data);
    auto smallData = archive->read(archive->entries()[1], 100);
    REQUIRE(smallData.has_value());
    CHECK(*smallData == text("small"));
    auto plain = archive->read(archive->entries()[2], 100);
    REQUIRE(plain.has_value());
    CHECK(*plain == text("plain"));
}

TEST(zip_big_directory)
{
    // a central directory bigger than the end of the file read first
    std::vector<FileSpec> files;
    for (int i = 0; i < 1500; ++i)
        files.push_back({"folder/a fairly long file name number " + std::to_string(i) + ".png", text(std::to_string(i)),
                         false});
    auto archive = openZip(makeZip(files));
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 1500);
    const ZipEntry* last = archive->find("a fairly long file name number 1499.png");
    REQUIRE(last != nullptr);
    auto data = archive->read(*last, 100);
    REQUIRE(data.has_value());
    CHECK(*data == text("1499"));
}

TEST(zip_stream)
{
    const Bytes body = compressible(300000, 5);
    auto archive = openZip(makeZip({{"music.bcstm", body}, {"stored.bin", body, false}}));
    REQUIRE(archive.has_value());

    for (const ZipEntry& entry : archive->entries())
    {
        auto opened = archive->openStream(entry);
        REQUIRE(opened.has_value());
        StreamReader& stream = **opened;
        CHECK_EQ(stream.size(), uint64_t(body.size()));

        // odd chunk sizes, to the end
        Bytes read;
        uint8_t buffer[7777];
        for (size_t chunk = 1;; chunk = chunk * 3 % 7777 + 1)
        {
            auto got = stream.read(MutableByteSpan(buffer, chunk));
            REQUIRE(got.has_value());
            if (*got == 0)
                break;
            read.insert(read.end(), buffer, buffer + *got);
        }
        CHECK(read == body);
        CHECK_EQ(stream.position(), uint64_t(body.size()));

        // forward, back to the start, back to the middle
        REQUIRE(stream.seek(0).has_value());
        REQUIRE(stream.seek(100000).has_value());
        Bytes part(1000);
        REQUIRE(stream.readExactly(part).has_value());
        CHECK(std::equal(part.begin(), part.end(), body.begin() + 100000));
        REQUIRE(stream.seek(50000).has_value());
        REQUIRE(stream.readExactly(part).has_value());
        CHECK(std::equal(part.begin(), part.end(), body.begin() + 50000));
        REQUIRE(stream.seek(body.size()).has_value());
        auto atEnd = stream.read(MutableByteSpan(buffer, 10));
        CHECK(atEnd.has_value() && *atEnd == 0);
        CHECK(!stream.seek(body.size() + 1).has_value());
    }
}

TEST(zip_stream_detects_corruption)
{
    FileSpec file{"music.bcstm", compressible(50000, 6)};
    file.badCrc = true;
    auto archive = openZip(makeZip({file}));
    REQUIRE(archive.has_value());
    auto opened = archive->openStream(archive->entries()[0]);
    REQUIRE(opened.has_value());
    Bytes all(50000);
    CHECK((*opened)->readExactly(all).error().code == Errc::InvalidData);
    CHECK(archive->read(archive->entries()[0], 50000).error().code == Errc::InvalidData);
}

TEST(zip_unsupported)
{
    FileSpec bzip2{"bzip2.bin", text("not really bzip2"), false};
    bzip2.method = 12;
    FileSpec encrypted{"secret.bin", text("secret"), false};
    encrypted.flags = 1;
    auto archive = openZip(makeZip({bzip2, encrypted, {"fine.txt", text("fine")}}));
    REQUIRE(archive.has_value());
    REQUIRE(archive->entries().size() == 3);
    CHECK(archive->read(archive->entries()[0], 100).error().code == Errc::Unsupported);
    CHECK(archive->openStream(archive->entries()[0]).error().code == Errc::Unsupported);
    CHECK(archive->read(archive->entries()[1], 100).error().code == Errc::Unsupported);
    CHECK(archive->read(archive->entries()[2], 100).has_value());

    ZipOptions split;
    split.disk = 1;
    CHECK(openZip(makeZip({{"a", text("a")}}, split)).error().code == Errc::Unsupported);
}

TEST(zip_broken)
{
    CHECK(openZip({}).error().code == Errc::InvalidData);
    CHECK(openZip(text("PK\x03\x04 this is no zip at all, just text")).error().code == Errc::InvalidData);

    const Bytes good = makeZip({{"a.txt", compressible(5000, 2)}, {"b.txt", text("b"), false}});
    // the end cut off
    CHECK(!openZip(Bytes(good.begin(), good.end() - 30)).has_value());

    // the data cut short: listed, but not readable
    Bytes truncated = good;
    truncated.erase(truncated.begin() + 40, truncated.begin() + 200);
    auto archive = openZip(truncated);
    if (archive)
        CHECK(!archive->read(archive->entries()[0], 10000).has_value());

    // whatever the bytes, nothing crashes or reads out of bounds (the sanitizers watch)
    std::mt19937 rng(77);
    for (int round = 0; round < 2000; ++round)
    {
        Bytes data = good;
        for (int i = 0, n = 1 + static_cast<int>(rng() % 6); i < n; ++i)
            data[rng() % data.size()] = static_cast<uint8_t>(rng());
        auto fuzzed = openZip(data);
        if (!fuzzed)
            continue;
        for (const ZipEntry& entry : fuzzed->entries())
        {
            (void)fuzzed->find(entry.name);
            (void)fuzzed->read(entry, 1 << 20);
            if (auto stream = fuzzed->openStream(entry))
            {
                uint8_t buffer[4096];
                for (int i = 0; i < 4; ++i)
                {
                    auto got = (*stream)->read(MutableByteSpan(buffer, sizeof(buffer)));
                    if (!got || *got == 0)
                        break;
                }
            }
        }
    }
}
