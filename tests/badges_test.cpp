// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/image.hpp"
#include "core/utf.hpp"
#include "domain/badges.hpp"
#include "memory_fs.hpp"
#include "test.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <string>

using namespace tz;

namespace
{

// The layout, written out again to check the files independently of the code that writes them.
constexpr size_t kSetNames = 0x0;
constexpr size_t kBadgeNames = 0x35E80;
constexpr size_t kSetIcons = 0x250F80;
constexpr size_t kLarge = 0x318F80;
constexpr size_t kSmall = 0xCDCF80;
constexpr size_t kBadgeRecords = 0x3E8;
constexpr size_t kSetRecords = 0xA028;
constexpr size_t kPlaced = 0xB2E8;
constexpr uint64_t kNoShortcut = 0xFFFFFFFFFFFFFFFF;

struct Colour
{
    uint8_t r, g, b, a = 255;
};

constexpr Colour kRed{255, 0, 0};
constexpr Colour kGreen{0, 255, 0};
constexpr Colour kBlue{0, 0, 255};
constexpr Colour kWhite{255, 255, 255};
constexpr Colour kOrange{255, 128, 0};
constexpr Colour kGrey{120, 120, 120};
constexpr Colour kCyan{0, 255, 255};
constexpr Colour kYellow{255, 255, 0};
constexpr Colour kPurple{128, 0, 128};

Image solid(int width, int height, Colour colour)
{
    Image image(width, height);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
    {
        image.pixels[i] = colour.r;
        image.pixels[i + 1] = colour.g;
        image.pixels[i + 2] = colour.b;
        image.pixels[i + 3] = colour.a;
    }
    return image;
}

// A sheet of solid badges, colours given a row at a time.
Image sheet(int columns, int rows, std::initializer_list<Colour> colours)
{
    Image image(columns * 64, rows * 64);
    auto colour = colours.begin();
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column, ++colour)
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                {
                    uint8_t* p = image.at(column * 64 + x, row * 64 + y);
                    p[0] = colour->r;
                    p[1] = colour->g;
                    p[2] = colour->b;
                    p[3] = colour->a;
                }
    return image;
}

Bytes png(const Image& image)
{
    return encodePng(image).value();
}

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

// A zip of stored files.
Bytes zip(const std::vector<std::pair<std::string, Bytes>>& files)
{
    Bytes out;
    Bytes directory;
    for (const auto& [name, data] : files)
    {
        const auto crc = static_cast<uint32_t>(::crc32(0, data.data(), static_cast<uInt>(data.size())));
        const auto offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034B50);
        put16(out, 20);
        put16(out, 0);
        put16(out, 0);
        put32(out, 0);
        put32(out, crc);
        put32(out, static_cast<uint32_t>(data.size()));
        put32(out, static_cast<uint32_t>(data.size()));
        put16(out, static_cast<uint16_t>(name.size()));
        put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());

        put32(directory, 0x02014B50);
        put16(directory, 20);
        put16(directory, 20);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, crc);
        put32(directory, static_cast<uint32_t>(data.size()));
        put32(directory, static_cast<uint32_t>(data.size()));
        put16(directory, static_cast<uint16_t>(name.size()));
        put32(directory, 0);
        put32(directory, 0);
        put32(directory, 0);
        put32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const auto directoryOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    put32(out, 0x06054B50);
    put32(out, 0);
    put16(out, static_cast<uint16_t>(files.size()));
    put16(out, static_cast<uint16_t>(files.size()));
    put32(out, static_cast<uint32_t>(directory.size()));
    put32(out, directoryOffset);
    put16(out, 0);
    return out;
}

struct Badges
{
    test::MemoryFs sd;
    test::MemoryFs ext{true}; // like extdata: fixed sizes
    Bytes defaultIcon = png(solid(48, 48, kYellow));

    Badges() { (void)badges::ensureFiles(ext); }

    Bytes& data() { return ext.data("/BadgeData.dat"); }
    Bytes& manager() { return ext.data("/BadgeMngFile.dat"); }

    Expected<badges::InstallResult> installAll(const badges::ProgressFn& progress = {})
    {
        auto sets = badges::findSets(sd, "/Badges", "Other Badges");
        if (!sets)
            return std::unexpected(sets.error());
        return badges::install(sd, ext, *sets, 0x12345678, defaultIcon, progress);
    }
};

uint16_t rgb565(Colour c)
{
    return toRgb565(c.r, c.g, c.b);
}

// A pixel of a stored badge: its RGB565 value and 4-bit alpha.
std::pair<uint16_t, int> storedPixel(const Bytes& data, size_t offset, int side, int x, int y)
{
    const uint32_t index = tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(side));
    const uint8_t* rgb = data.data() + offset;
    const uint8_t* alpha = rgb + static_cast<size_t>(side) * side * 2;
    return {readU16(rgb + index * 2), (alpha[index / 2] >> ((index & 1) * 4)) & 0xF};
}

bool badgeIs(const Bytes& data, int slot, Colour colour)
{
    for (int y = 0; y < 64; y += 9)
        for (int x = 0; x < 64; x += 7)
            if (storedPixel(data, kLarge + static_cast<size_t>(slot) * 0x2800, 64, x, y) !=
                std::pair<uint16_t, int>{rgb565(colour), colour.a >> 4})
                return false;
    for (int y = 0; y < 32; y += 5)
        for (int x = 0; x < 32; x += 3)
            if (storedPixel(data, kSmall + static_cast<size_t>(slot) * 0xA00, 32, x, y) !=
                std::pair<uint16_t, int>{rgb565(colour), colour.a >> 4})
                return false;
    return true;
}

bool setIconIs(const Bytes& data, int slot, Colour colour)
{
    const uint8_t* texture = data.data() + kSetIcons + static_cast<size_t>(slot) * 0x2000;
    // the icon in the top left, its edges repeated over the rest of the 64x64 texture
    for (const auto& [x, y] :
         {std::pair{0, 0}, std::pair{47, 47}, std::pair{20, 30}, std::pair{63, 63}, std::pair{50, 5}})
        if (readU16(texture + tiledIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 64) * 2) != rgb565(colour))
            return false;
    return true;
}

std::string nameAt(const Bytes& data, size_t offset, int language)
{
    return utf::fromUtf16Le(data.data() + offset + static_cast<size_t>(language) * 0x8A, 0x45);
}

const uint8_t* badgeRecord(const Bytes& manager, int slot)
{
    return manager.data() + kBadgeRecords + static_cast<size_t>(slot) * 0x28;
}

const uint8_t* setRecord(const Bytes& manager, int slot)
{
    return manager.data() + kSetRecords + static_cast<size_t>(slot) * 0x30;
}

bool isEmptySet(const uint8_t* record)
{
    static const uint8_t empty[0x30] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0x10, 0x27, 0, 0,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    return std::memcmp(record, empty, sizeof(empty)) == 0;
}

// Animals: a folder with an icon, single badges, a sheet with a shortcut, a picture of the wrong size, a zip and a
// broken one, and a zip of the same name. Plants: a zip with everything in a folder inside it. And one loose badge.
void addScenario(test::MemoryFs& sd)
{
    Image cat = solid(64, 64, {200, 100, 50});
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            std::memset(cat.at(x, y), 0, 4); // a transparent corner
    sd.put("/Badges/Animals/_seticon.png", png(solid(48, 48, kRed)));
    sd.put("/Badges/Animals/cat.png", png(cat));
    sd.put("/Badges/Animals/dogs.00021700.png", png(sheet(2, 1, {kGreen, kBlue})));
    sd.put("/Badges/Animals/bad.png", png(solid(65, 64, kRed)));
    sd.put("/Badges/Animals/notes.txt", {'h', 'i'});
    sd.put("/Badges/Animals/broken.zip", {'P', 'K', 3, 4, 'n', 'o', 't', ' ', 'a', ' ', 'z', 'i', 'p'});
    sd.put("/Badges/Animals/sub.zip", zip({
                                          {"inner/fox.png", png(solid(64, 64, kOrange))},
                                          {"_seticon.png", png(solid(48, 48, kBlue))},
                                          {"__MACOSX/inner/._fox.png", {1, 2, 3}},
                                      }));
    sd.put("/Badges/Animals.zip", zip({
                                      {"zebra.png", png(solid(64, 64, kGrey))},
                                      {"_seticon.png", png(solid(48, 48, kPurple))},
                                  }));
    sd.put("/Badges/Plants.zip", zip({
                                     {"Plants/", {}},
                                     {"Plants/_seticon.png", png(solid(48, 48, kGreen))},
                                     {"Plants/rose.png", png(sheet(2, 2, {kRed, kGreen, kBlue, kWhite}))},
                                 }));
    sd.put("/Badges/star.png", png(solid(64, 64, kCyan)));
}

} // namespace

TEST(badges_ensure_files)
{
    test::MemoryFs ext(true);
    REQUIRE(badges::ensureFiles(ext).has_value());
    REQUIRE(ext.hasFile("/BadgeData.dat") && ext.hasFile("/BadgeMngFile.dat"));
    CHECK_EQ(ext.data("/BadgeData.dat").size(), size_t(badges::kDataSize));
    const Bytes& manager = ext.data("/BadgeMngFile.dat");
    CHECK_EQ(manager.size(), size_t(badges::kManagerSize));
    CHECK_EQ(readU32(manager.data() + 0x10), 0xFFFFFFFFu);
    CHECK(isEmptySet(setRecord(manager, 0)) && isEmptySet(setRecord(manager, 99)));
    CHECK_EQ(badges::installedCount(ext), 0);
    CHECK_EQ(badges::storedPrincipalId(ext), 0u);

    // there already: left alone
    const size_t writes = ext.writes();
    REQUIRE(badges::ensureFiles(ext).has_value());
    CHECK_EQ(ext.writes(), writes);

    // the wrong size is as good as missing; a new data file empties the manager, keeping the principal ID
    writeU32(ext.data("/BadgeMngFile.dat").data() + 0x8, 5);
    writeU32(ext.data("/BadgeMngFile.dat").data() + 0x1C, 77);
    REQUIRE(ext.removeFile("/BadgeData.dat").has_value());
    REQUIRE(ext.createFile("/BadgeData.dat", 100).has_value());
    REQUIRE(badges::ensureFiles(ext).has_value());
    CHECK_EQ(ext.data("/BadgeData.dat").size(), size_t(badges::kDataSize));
    CHECK_EQ(badges::installedCount(ext), 0);
    CHECK_EQ(badges::storedPrincipalId(ext), 77u);

    test::MemoryFs nothing(true);
    CHECK_EQ(badges::installedCount(nothing), 0);
    CHECK_EQ(badges::storedPrincipalId(nothing), 0u);
}

TEST(badges_find_sets)
{
    test::MemoryFs sd;
    sd.put("/Badges/Zeta/a.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/alpha.zip", zip({{"b.png", png(solid(64, 64, kRed))}}));
    sd.put("/Badges/Alpha/c.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/Mid.zip", zip({}));
    sd.put("/Badges/.hidden/d.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/__MACOSX/e.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/loose2.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/Loose1.png", png(solid(64, 64, kRed)));
    sd.put("/Badges/_seticon.png", png(solid(48, 48, kBlue)));
    sd.put("/Badges/notes.txt", {});
    REQUIRE(sd.createDirectory("/Badges/Empty").has_value());

    auto sets = badges::findSets(sd, "/Badges", "Other Badges");
    REQUIRE(sets.has_value());
    REQUIRE(sets->size() == 5);
    const auto& list = *sets;
    CHECK_EQ(list[0].name, std::string("Alpha")); // the folder's spelling
    CHECK_EQ(list[0].folderPath, std::string("/Badges/Alpha"));
    CHECK_EQ(list[0].zipPath, std::string("/Badges/alpha.zip"));
    CHECK_EQ(list[1].name, std::string("Empty"));
    CHECK_EQ(list[2].name, std::string("Mid"));
    CHECK(list[2].folderPath.empty());
    CHECK_EQ(list[3].name, std::string("Zeta"));
    CHECK(list[3].zipPath.empty());
    CHECK_EQ(list[4].name, std::string("Other Badges"));
    CHECK(list[4].loose());
    REQUIRE(list[4].loosePaths.size() == 2);
    CHECK_EQ(list[4].loosePaths[0], std::string("/Badges/Loose1.png"));
    CHECK_EQ(list[4].loosePaths[1], std::string("/Badges/loose2.png"));

    auto summary = badges::summarize(sd, list[0]);
    REQUIRE(summary.has_value());
    CHECK_EQ(summary->images, 2);
    CHECK(!summary->hasIcon);
    auto looseSummary = badges::summarize(sd, list[4]);
    REQUIRE(looseSummary.has_value());
    CHECK_EQ(looseSummary->images, 2);
    CHECK(looseSummary->hasIcon); // the _seticon.png beside the loose badges

    auto none = badges::findSets(sd, "/Missing", "Other Badges");
    REQUIRE(none.has_value());
    CHECK(none->empty());
}

TEST(badges_install)
{
    Badges b;
    addScenario(b.sd);
    std::vector<std::pair<int, int>> calls;
    auto result = b.installAll([&](int done, int total) {
        calls.emplace_back(done, total);
        return true;
    });
    REQUIRE(result.has_value());
    CHECK_EQ(result->sets, 3);
    CHECK_EQ(result->badges, 10);
    CHECK_EQ(result->skipped, 2); // bad.png and broken.zip
    CHECK(!result->full);
    // per picture: bad, cat, dogs, fox, zebra, rose, star
    REQUIRE(!calls.empty());
    CHECK(calls.front() == std::make_pair(0, 7));
    CHECK(calls.back() == std::make_pair(7, 7));
    CHECK_EQ(calls.size(), size_t(8));

    const Bytes& manager = b.manager();
    CHECK_EQ(readU32(manager.data() + 0x0), 0u);
    CHECK_EQ(readU32(manager.data() + 0x4), 3u);
    CHECK_EQ(readU32(manager.data() + 0x8), 10u);
    CHECK_EQ(readU32(manager.data() + 0xC), 0u);
    CHECK_EQ(readU32(manager.data() + 0x10), 0xFFFFFFFFu);
    CHECK_EQ(readU32(manager.data() + 0x18), 10u * 0xFFFF);
    CHECK_EQ(readU32(manager.data() + 0x1C), 0x12345678u);
    CHECK_EQ(int(manager[0x358]), 0xFF);
    CHECK_EQ(int(manager[0x359]), 0x03);
    CHECK_EQ(int(manager[0x35A]), 0);
    CHECK_EQ(int(manager[0x3D8]), 0x07);
    CHECK_EQ(int(manager[0x3D9]), 0);
    CHECK_EQ(badges::installedCount(b.ext), 10);
    CHECK_EQ(badges::storedPrincipalId(b.ext), 0x12345678u);

    // badges: ID, set ID, slot, whole, none placed, all of them, shortcut
    const uint32_t setOf[10] = {1, 1, 1, 1, 1, 2, 2, 2, 2, 3};
    for (int i = 0; i < 10; ++i)
    {
        const uint8_t* record = badgeRecord(manager, i);
        CHECK_EQ(readU32(record + 0x0), 0u);
        CHECK_EQ(readU32(record + 0x4), uint32_t(i + 1));
        CHECK_EQ(readU32(record + 0x8), setOf[i]);
        CHECK_EQ(readU16(record + 0xC), uint16_t(i));
        CHECK_EQ(readU16(record + 0xE), uint16_t(0));
        CHECK_EQ(readU16(record + 0x10), uint16_t(0));
        CHECK_EQ(readU16(record + 0x12), uint16_t(0xFFFF));
        const uint64_t shortcut = i == 1 || i == 2 ? 0x0004001000021700 : kNoShortcut;
        CHECK_EQ(readU64(record + 0x18), shortcut);
        CHECK_EQ(readU64(record + 0x20), shortcut);
    }
    CHECK(std::all_of(badgeRecord(manager, 10), badgeRecord(manager, 1000), [](uint8_t v) { return v == 0; }));

    // sets: ID, slot, badges, their quantity, first badge
    const uint32_t counts[3] = {5, 4, 1};
    const uint32_t firsts[3] = {0, 5, 9};
    for (int i = 0; i < 3; ++i)
    {
        const uint8_t* record = setRecord(manager, i);
        CHECK(std::all_of(record, record + 8, [](uint8_t v) { return v == 0xFF; }));
        CHECK_EQ(readU32(record + 0x8), 0u);
        CHECK_EQ(readU32(record + 0xC), 0x2710u);
        CHECK_EQ(readU32(record + 0x10), uint32_t(i + 1));
        CHECK_EQ(readU32(record + 0x14), uint32_t(i));
        CHECK_EQ(readU32(record + 0x18), 0xFFFFFFFFu);
        CHECK_EQ(readU32(record + 0x1C), counts[i]);
        CHECK_EQ(readU32(record + 0x20), counts[i] * 0xFFFF);
        CHECK_EQ(readU32(record + 0x24), firsts[i]);
        CHECK_EQ(readU64(record + 0x28), uint64_t(0));
    }
    CHECK(isEmptySet(setRecord(manager, 3)) && isEmptySet(setRecord(manager, 99)));

    // names in every language
    const Bytes& data = b.data();
    const char* names[10] = {"cat", "dogs", "dogs", "fox", "zebra", "rose", "rose", "rose", "rose", "star"};
    for (int i = 0; i < 10; ++i)
        for (int language : {0, 1, 15})
            CHECK_EQ(nameAt(data, kBadgeNames + static_cast<size_t>(i) * 0x8A0, language), std::string(names[i]));
    CHECK_EQ(nameAt(data, kSetNames, 0), std::string("Animals"));
    CHECK_EQ(nameAt(data, kSetNames + 0x8A0, 7), std::string("Plants"));
    CHECK_EQ(nameAt(data, kSetNames + 2 * 0x8A0, 15), std::string("Other Badges"));

    // pictures: sheets cut a column at a time
    CHECK(badgeIs(data, 1, kGreen));
    CHECK(badgeIs(data, 2, kBlue));
    CHECK(badgeIs(data, 3, kOrange));
    CHECK(badgeIs(data, 4, kGrey));
    CHECK(badgeIs(data, 5, kRed));
    CHECK(badgeIs(data, 6, kBlue));
    CHECK(badgeIs(data, 7, kGreen));
    CHECK(badgeIs(data, 8, kWhite));
    CHECK(badgeIs(data, 9, kCyan));

    // the cat's transparent corner: still transparent, its edge taking the colour next to it
    const size_t cat = kLarge;
    CHECK(storedPixel(data, cat, 64, 15, 0) == std::make_pair(toRgb565(200, 100, 50), 0));
    CHECK(storedPixel(data, cat, 64, 14, 3) == std::make_pair(toRgb565(200, 100, 50), 0));
    CHECK(storedPixel(data, cat, 64, 0, 0) == std::make_pair(uint16_t(0), 0));
    CHECK(storedPixel(data, cat, 64, 16, 0) == std::make_pair(toRgb565(200, 100, 50), 15));
    CHECK(storedPixel(data, kSmall, 32, 8, 8) == std::make_pair(toRgb565(200, 100, 50), 15));
    CHECK(storedPixel(data, kSmall, 32, 7, 0).second == 0);

    // icons: the folder's wins over its zips'; a zip's own in a folder in it; the default for the loose badges
    CHECK(setIconIs(data, 0, kRed));
    CHECK(setIconIs(data, 1, kGreen));
    CHECK(setIconIs(data, 2, kYellow));
}

TEST(badges_pixel_order)
{
    // every pixel different, to pin the tiling and the alpha nibbles: the even pixel of a pair in the low nibble
    Image gradient(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            uint8_t* p = gradient.at(x, y);
            p[0] = static_cast<uint8_t>(x * 4);
            p[1] = static_cast<uint8_t>(y * 4);
            p[2] = static_cast<uint8_t>((x + y) * 2);
            p[3] = static_cast<uint8_t>(16 + x + y); // never 0, which would take the colour next to it
        }
    Badges b;
    b.sd.put("/Badges/Set/gradient.png", png(gradient));
    REQUIRE(b.installAll().has_value());

    const uint8_t* rgb = b.data().data() + kLarge;
    const uint8_t* alpha = rgb + 0x2000;
    bool same = true;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            // the order GYTB (and the HOME Menu) uses: 8x8 tiles in rows, Z-order inside a tile
            const int i = 8 * 64 * (y / 8) + 64 * (x / 8) + 32 * ((y / 4) % 2) + 16 * ((x / 4) % 2) +
                          8 * ((y / 2) % 2) + 4 * ((x / 2) % 2) + 2 * (y % 2) + (x % 2);
            const uint8_t* p = gradient.at(x, y);
            same = same && readU16(rgb + i * 2) == toRgb565(p[0], p[1], p[2]);
            same = same && ((alpha[i / 2] >> (4 * (x % 2))) & 0xF) == p[3] >> 4;
        }
    CHECK(same);
}

TEST(badges_placed_badges_kept)
{
    Badges b;
    addScenario(b.sd);
    Bytes& manager = b.manager();
    for (size_t i = kPlaced; i < manager.size(); ++i)
        manager[i] = static_cast<uint8_t>(i * 7);
    const Bytes placed(manager.begin() + kPlaced, manager.end());
    REQUIRE(b.installAll().has_value());
    CHECK(Bytes(b.manager().begin() + kPlaced, b.manager().end()) == placed);

    // again, over badges that are there
    REQUIRE(b.installAll().has_value());
    CHECK(Bytes(b.manager().begin() + kPlaced, b.manager().end()) == placed);

    // taking every badge off the HOME Menu clears them
    REQUIRE(badges::uninstall(b.ext).has_value());
    const Bytes& after = b.manager();
    CHECK(std::all_of(after.begin() + kPlaced, after.end(), [](uint8_t v) { return v == 0; }));
    CHECK_EQ(readU32(after.data() + 0x4), 0u);
    CHECK_EQ(readU32(after.data() + 0x8), 0u);
    CHECK_EQ(readU32(after.data() + 0x10), 0xFFFFFFFFu);
    CHECK_EQ(readU32(after.data() + 0x1C), 0x12345678u);
    CHECK(isEmptySet(setRecord(after, 0)));
    CHECK_EQ(int(after[0x358]), 0);
    CHECK_EQ(badges::installedCount(b.ext), 0);
}

TEST(badges_limit_of_badges)
{
    Badges b;
    // 14 sheets of 12x6: 1008 badges, 8 too many
    std::vector<Colour> colours;
    for (int i = 0; i < 72; ++i)
        colours.push_back({static_cast<uint8_t>(i * 3), 7, 9});
    Image full(12 * 64, 6 * 64);
    for (int column = 0; column < 12; ++column)
        for (int row = 0; row < 6; ++row)
        {
            const Colour c = colours[static_cast<size_t>(row * 12 + column)];
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                {
                    uint8_t* p = full.at(column * 64 + x, row * 64 + y);
                    p[0] = c.r;
                    p[1] = c.g;
                    p[2] = c.b;
                    p[3] = 0xFF;
                }
        }
    const Bytes sheetPng = png(full);
    for (int i = 0; i < 14; ++i)
        b.sd.put("/Badges/Big/sheet" + std::to_string(100 + i) + ".png", sheetPng);
    b.sd.put("/Badges/Later/x.png", png(solid(64, 64, kRed)));

    auto result = b.installAll();
    REQUIRE(result.has_value());
    CHECK_EQ(result->badges, badges::kMaxBadges);
    CHECK_EQ(result->sets, 1);
    CHECK(result->full);
    CHECK_EQ(badges::installedCount(b.ext), 1000);
    const Bytes& manager = b.manager();
    CHECK(std::all_of(manager.begin() + 0x358, manager.begin() + 0x358 + 125, [](uint8_t v) { return v == 0xFF; }));
    CHECK_EQ(int(manager[0x358 + 125]), 0);
    CHECK_EQ(readU32(setRecord(manager, 0) + 0x1C), 1000u);
    CHECK_EQ(readU32(badgeRecord(manager, 999) + 0x4), 1000u);
    // a column at a time: the second badge of a sheet is its second row
    CHECK(badgeIs(b.data(), 1, colours[12]));
    CHECK(badgeIs(b.data(), 6, colours[1]));
    CHECK(badgeIs(b.data(), 999, colours[static_cast<size_t>((999 % 72) % 6 * 12 + (999 % 72) / 6)]));
}

TEST(badges_limit_of_sets)
{
    Badges b;
    const Bytes badge = png(solid(64, 64, kBlue));
    for (int i = 0; i < 101; ++i)
        b.sd.put("/Badges/Set " + std::to_string(1000 + i) + "/b.png", badge);
    auto result = b.installAll();
    REQUIRE(result.has_value());
    CHECK_EQ(result->sets, badges::kMaxSets);
    CHECK_EQ(result->badges, 100);
    CHECK(result->full);
    CHECK_EQ(readU32(b.manager().data() + 0x4), 100u);
    CHECK_EQ(readU32(setRecord(b.manager(), 99) + 0x10), 100u);
    CHECK_EQ(nameAt(b.data(), kSetNames + 99 * 0x8A0, 1), std::string("Set 1099"));
    CHECK_EQ(int(b.manager()[0x3D8 + 12]), 0x0F);
}

TEST(badges_cancel)
{
    Badges b;
    for (int i = 0; i < 50; ++i)
        b.sd.put("/Badges/Many/" + std::to_string(100 + i) + ".png", png(solid(64, 64, kGreen)));
    b.sd.put("/Badges/More/x.png", png(solid(64, 64, kRed)));
    REQUIRE(b.installAll().has_value());
    const Bytes data = b.data();
    const Bytes manager = b.manager();

    // before anything is written: everything stays as it was
    auto cancelled = b.installAll([](int, int) { return false; });
    REQUIRE(!cancelled.has_value());
    CHECK(cancelled.error().code == Errc::Cancelled);
    CHECK(b.data() == data);
    CHECK(b.manager() == manager);

    // later: what is installed by then is complete
    auto partly = b.installAll([](int done, int) { return done < 40; });
    REQUIRE(!partly.has_value());
    CHECK(partly.error().code == Errc::Cancelled);
    CHECK_EQ(badges::installedCount(b.ext), 40);
    CHECK_EQ(readU32(b.manager().data() + 0x4), 1u);
    CHECK_EQ(readU32(setRecord(b.manager(), 0) + 0x1C), 40u);
    CHECK_EQ(nameAt(b.data(), kSetNames, 1), std::string("Many"));
}

TEST(badges_writes_in_batches)
{
    Badges b;
    for (int i = 0; i < 100; ++i)
        b.sd.put("/Badges/Many/" + std::to_string(100 + i) + ".png", png(solid(64, 64, kGreen)));
    const size_t before = b.ext.writes();
    REQUIRE(b.installAll().has_value());
    // 4 batches of names, pictures and small pictures; the set names and icons; the manager
    CHECK(b.ext.writes() - before <= 15);
}

TEST(badges_preview_and_icons)
{
    test::MemoryFs sd;
    addScenario(sd);
    sd.put("/Badges/_seticon.png", png(solid(30, 30, kWhite))); // scaled to 48x48
    auto sets = badges::findSets(sd, "/Badges", "Other Badges");
    REQUIRE(sets.has_value());
    REQUIRE(sets->size() == 3);
    const badges::SetSource& animals = (*sets)[0];

    auto summary = badges::summarize(sd, animals);
    REQUIRE(summary.has_value());
    CHECK_EQ(summary->images, 5); // bad, cat, dogs, fox, zebra
    CHECK(summary->hasIcon);

    CancelToken token;
    auto preview = badges::previewBadges(sd, animals, 3, token);
    REQUIRE(preview.has_value());
    REQUIRE(preview->size() == 3);
    CHECK_EQ((*preview)[0].width, 64);
    CHECK_EQ(int((*preview)[1].at(10, 10)[1]), 255); // the dogs: green, then blue
    CHECK_EQ(int((*preview)[2].at(10, 10)[2]), 255);
    auto all = badges::previewBadges(sd, (*sets)[1], 100, token);
    REQUIRE(all.has_value());
    CHECK_EQ(all->size(), size_t(4));

    auto icon = badges::setIcon(sd, animals);
    REQUIRE(icon.has_value());
    CHECK_EQ(icon->width, 48);
    CHECK_EQ(int(icon->at(5, 5)[0]), 255);
    auto plants = badges::setIcon(sd, (*sets)[1]);
    REQUIRE(plants.has_value());
    CHECK_EQ(int(plants->at(40, 40)[1]), 255);
    auto loose = badges::setIcon(sd, (*sets)[2]);
    REQUIRE(loose.has_value());
    CHECK_EQ(loose->width, 48);
    CHECK_EQ(int(loose->at(47, 47)[2]), 255);

    sd.put("/Badges/NoIcon/a.png", png(solid(64, 64, kRed)));
    badges::SetSource noIcon;
    noIcon.name = "NoIcon";
    noIcon.folderPath = "/Badges/NoIcon";
    CHECK(badges::setIcon(sd, noIcon).error().code == Errc::NotFound);

    token.cancel();
    CHECK(badges::previewBadges(sd, animals, 3, token).error().code == Errc::Cancelled);
}

TEST(badges_backup)
{
    Badges b;
    addScenario(b.sd);
    REQUIRE(b.installAll().has_value());
    const Bytes data = b.data();
    const Bytes manager = b.manager();

    test::MemoryFs sd;
    CHECK(!badges::hasBackup(sd, "/3ds/Themezer/Badges"));
    auto first = badges::backupOnce(b.ext, sd, "/3ds/Themezer/Badges");
    REQUIRE(first.has_value());
    CHECK(*first);
    CHECK(badges::hasBackup(sd, "/3ds/Themezer/Badges"));
    CHECK(sd.data("/3ds/Themezer/Badges/BadgeData.dat") == data);
    CHECK(sd.data("/3ds/Themezer/Badges/BadgeMngFile.dat") == manager);
    CHECK(!sd.hasFile("/3ds/Themezer/Badges/BadgeData.dat.part"));

    // only once: the first copy is the one to keep
    REQUIRE(badges::uninstall(b.ext).has_value());
    auto second = badges::backupOnce(b.ext, sd, "/3ds/Themezer/Badges");
    REQUIRE(second.has_value());
    CHECK(!*second);
    CHECK(sd.data("/3ds/Themezer/Badges/BadgeMngFile.dat") == manager);

    REQUIRE(badges::restoreBackup(sd, "/3ds/Themezer/Badges", b.ext).has_value());
    CHECK(b.data() == data);
    CHECK(b.manager() == manager);

    // restoring into extdata without the files
    test::MemoryFs fresh(true);
    REQUIRE(badges::restoreBackup(sd, "/3ds/Themezer/Badges", fresh).has_value());
    CHECK(fresh.data("/BadgeMngFile.dat") == manager);

    // an interrupted copy isn't a backup
    test::MemoryFs partial;
    partial.put("/backup/BadgeData.dat.part", Bytes(badges::kDataSize));
    partial.put("/backup/BadgeMngFile.dat", Bytes(badges::kManagerSize));
    CHECK(!badges::hasBackup(partial, "/backup"));
    CHECK(badges::restoreBackup(partial, "/backup", b.ext).error().code == Errc::NotFound);

    // nothing to copy from
    test::MemoryFs empty(true);
    test::MemoryFs sd2;
    CHECK(badges::backupOnce(empty, sd2, "/backup").error().code == Errc::NotFound);
}

TEST(badges_export)
{
    Badges b;
    addScenario(b.sd);
    REQUIRE(b.installAll().has_value());

    int calls = 0;
    auto saved = badges::exportBadges(b.ext, b.sd, "/Export", [&](int, int total) {
        ++calls;
        return total == 10;
    });
    REQUIRE(saved.has_value());
    CHECK_EQ(*saved, 10);
    CHECK_EQ(calls, 11);

    for (const char* file :
         {"/Export/Animals/_seticon.png", "/Export/Animals/cat.png", "/Export/Animals/dogs.00021700.1.png",
          "/Export/Animals/dogs.00021700.2.png", "/Export/Animals/fox.png", "/Export/Animals/zebra.png",
          "/Export/Plants/_seticon.png", "/Export/Plants/rose.1.png", "/Export/Plants/rose.2.png",
          "/Export/Plants/rose.3.png", "/Export/Plants/rose.4.png", "/Export/Other Badges/star.png"})
        CHECK(b.sd.hasFile(file));

    auto icon = decodePng(b.sd.data("/Export/Animals/_seticon.png"));
    REQUIRE(icon.has_value());
    CHECK_EQ(icon->width, 48);
    CHECK(icon->at(10, 10)[0] == 255 && icon->at(10, 10)[1] == 0 && icon->at(10, 10)[3] == 255);
    auto cat = decodePng(b.sd.data("/Export/Animals/cat.png"));
    REQUIRE(cat.has_value());
    CHECK_EQ(cat->width, 64);
    CHECK_EQ(int(cat->at(0, 0)[3]), 0);
    CHECK_EQ(int(cat->at(40, 40)[3]), 255);
    CHECK_EQ(toRgb565(cat->at(40, 40)[0], cat->at(40, 40)[1], cat->at(40, 40)[2]), toRgb565(200, 100, 50));

    // installed again from the export: the same badges, the parts of a sheet in the same order
    Badges again;
    auto sets = badges::findSets(b.sd, "/Export", "Other Badges");
    REQUIRE(sets.has_value());
    auto result = badges::install(b.sd, again.ext, *sets, 1, again.defaultIcon, {});
    REQUIRE(result.has_value());
    CHECK_EQ(result->badges, 10);
    CHECK_EQ(result->sets, 3);
    CHECK_EQ(result->skipped, 0);
    CHECK_EQ(readU64(badgeRecord(again.manager(), 1) + 0x18), uint64_t(0x0004001000021700));
    CHECK_EQ(readU64(badgeRecord(again.manager(), 2) + 0x18), uint64_t(0x0004001000021700));
    CHECK_EQ(nameAt(again.data(), kBadgeNames + 2 * 0x8A0, 1), std::string("dogs"));
    CHECK(badgeIs(again.data(), 2, kBlue));
    CHECK(setIconIs(again.data(), 0, kRed));

    // cancelled
    CHECK(badges::exportBadges(b.ext, b.sd, "/Export2", [](int, int) { return false; }).error().code ==
          Errc::Cancelled);
}

TEST(badges_export_without_sets)
{
    // as GYTB leaves them: no sets, every badge in a set 0xEFBE that isn't there
    Badges b;
    Bytes& manager = b.manager();
    writeU32(manager.data() + 0x8, 2);
    for (int i = 0; i < 2; ++i)
    {
        uint8_t* record = manager.data() + kBadgeRecords + static_cast<size_t>(i) * 0x28;
        writeU32(record + 0x4, static_cast<uint32_t>(i + 1));
        writeU32(record + 0x8, 0xEFBE);
        writeU16(record + 0xC, static_cast<uint16_t>(i));
        writeU64(record + 0x18, kNoShortcut);
        utf::toUtf16Le("Lenny.face", b.data().data() + kBadgeNames + static_cast<size_t>(i) * 0x8A0, 0x45);
    }
    auto saved = badges::exportBadges(b.ext, b.sd, "/Dump", {});
    REQUIRE(saved.has_value());
    CHECK_EQ(*saved, 2);
    CHECK(b.sd.hasFile("/Dump/Lenny-face.1.png"));
    CHECK(b.sd.hasFile("/Dump/Lenny-face.2.png"));

    // nothing installed: nothing saved
    Badges empty;
    auto none = badges::exportBadges(empty.ext, empty.sd, "/Dump", {});
    REQUIRE(none.has_value());
    CHECK_EQ(*none, 0);
}
