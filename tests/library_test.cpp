// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/image.hpp"
#include "core/smdh.hpp"
#include "core/zip.hpp"
#include "domain/library.hpp"
#include "domain/package.hpp"
#include "domain/splashes.hpp"
#include "fixtures.hpp"
#include "memory_fs.hpp"
#include "test.hpp"

#include <algorithm>

using namespace tz;

namespace
{

Bytes smdhFor(const std::string& name, const std::string& author)
{
    std::array<uint16_t, 48 * 48> icon{};
    icon.fill(0xF800);
    return buildSmdh(SmdhText{name, "A description", author}, icon);
}

} // namespace

TEST(library_scan_finds_folders_and_zips)
{
    test::MemoryFs sd;
    sd.put("/Themes/Folder Theme/body_LZ.bin", test::themeBody(1));
    sd.put("/Themes/Zipped.zip", test::storedZip({{"body_LZ.bin", test::themeBody(1)}}));
    sd.put("/Themes/notes.txt", test::bytesOf("hello"));
    sd.put("/Themes/.hidden/body_LZ.bin", test::themeBody(1));
    auto sources = library::scan(sd, library::kThemesRoot);
    REQUIRE(sources.has_value());
    CHECK_EQ(sources->size(), size_t(2));
    const auto zip = std::find_if(sources->begin(), sources->end(), [](const auto& s) { return s.zip; });
    REQUIRE(zip != sources->end());
    CHECK_EQ(zip->path, std::string("/Themes/Zipped.zip"));
    CHECK(zip->size > 0);
}

TEST(library_scan_makes_a_missing_root)
{
    test::MemoryFs sd;
    auto sources = library::scan(sd, library::kSplashesRoot);
    REQUIRE(sources.has_value());
    CHECK(sources->empty());
    CHECK(sd.exists(library::kSplashesRoot));
}

TEST(library_read_info_from_a_zip)
{
    test::MemoryFs sd;
    const Bytes body = test::themeBody(1);
    sd.put("/Themes/T.zip", test::storedZip({{"T/info.smdh", smdhFor("Blue Sky", "Ana")},
                                             {"T/body_LZ.bin", body},
                                             {"T/bgm.bcstm", test::fakeBgm(64, 1)},
                                             {"T/preview.png", test::bytesOf("png")}}));
    library::Source source{"/Themes/T.zip", "T.zip", true, sd.data("/Themes/T.zip").size()};
    auto info = library::readInfo(sd, source, library::Kind::Theme, 1);
    REQUIRE(info.has_value());
    CHECK_EQ(info->name, std::string("Blue Sky"));
    CHECK_EQ(info->author, std::string("Ana"));
    CHECK(info->hasMusic);
    CHECK(info->hasPreview);
    CHECK(info->icon != nullptr);
    CHECK_EQ(info->body.size, uint32_t(body.size()));
    CHECK_EQ(info->body.crc, crc32(body));
    CHECK(info->usable(library::Kind::Theme));
}

TEST(library_read_info_from_a_folder)
{
    test::MemoryFs sd;
    const Bytes top(splashes::kTopRawSize, 3);
    sd.put("/Splashes/S/splash.bin", top);
    library::Source source{"/Splashes/S", "S", false, 0};
    auto info = library::readInfo(sd, source, library::Kind::Splash, 1);
    REQUIRE(info.has_value());
    CHECK(!info->hasSmdh);
    CHECK_EQ(info->top.size, uint32_t(top.size()));
    CHECK_EQ(info->top.crc, uint32_t(0)); // unknown until computed
    CHECK_EQ(info->bottom.size, uint32_t(0));
    CHECK(info->usable(library::Kind::Splash));

    auto crc = library::computeCrc(sd, source, splashes::kTopName);
    REQUIRE(crc.has_value());
    CHECK_EQ(*crc, crc32(top));

    library::Item item{source, std::nullopt};
    CHECK_EQ(item.displayName(), std::string("S"));
}

TEST(library_cache_round_trip)
{
    test::MemoryFs sd;
    library::Item item;
    item.source = {"/Themes/A.zip", "A.zip", true, 1234};
    library::Info info;
    info.name = "A theme";
    info.author = "Someone";
    info.description = "Words";
    info.hasMusic = true;
    info.body = {100, 0xDEADBEEF};
    auto icon = std::make_shared<library::Icon>();
    (*icon)[7] = 0x1234;
    info.icon = icon;
    item.info = info;
    library::Item unloaded;
    unloaded.source = {"/Themes/B", "B", false, 0};

    REQUIRE(library::saveCache(sd, "/3ds/Themezer/cache/themes.bin", library::Kind::Theme, {item, unloaded})
                .has_value());
    auto entries = library::loadCache(sd, "/3ds/Themezer/cache/themes.bin", library::Kind::Theme);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].first == item.source);
    CHECK_EQ(entries[0].second.name, std::string("A theme"));
    CHECK_EQ(entries[0].second.body.crc, uint32_t(0xDEADBEEF));
    CHECK(entries[0].second.hasMusic);
    REQUIRE(entries[0].second.icon != nullptr);
    CHECK_EQ((*entries[0].second.icon)[7], uint16_t(0x1234));

    // another kind's cache isn't read as this one
    CHECK(library::loadCache(sd, "/3ds/Themezer/cache/themes.bin", library::Kind::Splash).empty());
}

TEST(library_cache_survives_garbage)
{
    test::MemoryFs sd;
    Bytes junk = test::bytesOf("TZLC");
    junk.resize(200, 0xFF);
    sd.put("/c.bin", junk);
    CHECK(library::loadCache(sd, "/c.bin", library::Kind::Theme).empty());
}

TEST(library_natural_sort)
{
    CHECK(library::compareNatural("Theme 2", "Theme 10") < 0);
    CHECK(library::compareNatural("theme 2", "Theme 2") == 0);
    CHECK(library::compareNatural("a", "B") < 0);
    CHECK(library::compareNatural("x007", "x7") == 0);
    CHECK(library::compareNatural("abc", "ab") > 0);

    std::vector<library::Item> items(3);
    items[0].source = {"/Themes/c.zip", "c.zip", true, 1};
    items[1].source = {"/Themes/a.zip", "a.zip", true, 1};
    items[2].source = {"/Themes/b.zip", "b.zip", true, 1};
    items[2].info = library::Info{};
    items[2].info->name = "Zebra";
    library::sort(items, library::Sort::Name);
    CHECK_EQ(items[0].displayName(), std::string("a"));
    CHECK_EQ(items[2].displayName(), std::string("Zebra"));
}

TEST(library_download_names)
{
    auto theme = library::parseDownloadName("Darth Vader Theme Theme by Jeageruzumaki (Themezer-T1ED55)");
    REQUIRE(theme.has_value());
    CHECK_EQ(theme->name, std::string("Darth Vader Theme"));
    CHECK_EQ(theme->author, std::string("Jeageruzumaki"));

    auto badges = library::parseDownloadName("Horimiya Badges Badge Set by dingle-bob (Themezer-BS1B98F)");
    REQUIRE(badges.has_value());
    CHECK_EQ(badges->name, std::string("Horimiya Badges"));
    CHECK_EQ(badges->author, std::string("dingle-bob"));

    auto splash = library::parseDownloadName("Jokers Mask Splash by .c.e.n.t.u.r.i.o.n. (Themezer-S1EB59)");
    REQUIRE(splash.has_value());
    CHECK_EQ(splash->name, std::string("Jokers Mask"));
    CHECK_EQ(splash->author, std::string(".c.e.n.t.u.r.i.o.n."));

    CHECK(!library::parseDownloadName("My theme").has_value());
    CHECK(!library::parseDownloadName("Theme by someone (Themezer-T1)").has_value());
    CHECK(!library::parseDownloadName("A Theme by  (Themezer-T1)").has_value());

    library::Item item;
    item.source = {"/Themes/Korn Theme by isaac-rizzi (Themezer-T1ED6C).zip",
                   "Korn Theme by isaac-rizzi (Themezer-T1ED6C).zip", true, 1};
    CHECK_EQ(item.displayName(), std::string("Korn"));
    CHECK_EQ(item.author(), std::string("isaac-rizzi"));
    item.info = library::Info{};
    item.info->name = "Korn (from its SMDH)";
    CHECK_EQ(item.displayName(), std::string("Korn (from its SMDH)"));
}

TEST(splashes_install_and_detect)
{
    test::MemoryFs sd;
    const Bytes top(splashes::kTopRawSize, 1);
    const Bytes bottom(splashes::kBottomRawSize, 2);
    REQUIRE(splashes::install(sd, top, bottom, splashes::Screens::Both).has_value());
    auto current = splashes::readCurrent(sd);
    REQUIRE(current.has_value());
    REQUIRE(current->top.has_value());
    CHECK_EQ(current->top->crc, crc32(top));
    CHECK_EQ(current->bottom->size, uint32_t(bottom.size()));

    // only the top screen: the bottom stays
    const Bytes otherTop(splashes::kTopRawSize, 9);
    REQUIRE(splashes::install(sd, otherTop, {}, splashes::Screens::Top).has_value());
    CHECK(sd.data(splashes::kTopPath) == otherTop);
    CHECK(sd.data(splashes::kBottomPath) == bottom);

    // a splash with only a top screen, installed for both, takes the old bottom away
    REQUIRE(splashes::install(sd, top, {}, splashes::Screens::Both).has_value());
    CHECK(!sd.hasFile(splashes::kBottomPath));

    REQUIRE(splashes::uninstall(sd).has_value());
    CHECK(!sd.hasFile(splashes::kTopPath));
    CHECK(!splashes::readInstalled(sd).has_value());
}

TEST(splashes_raw_to_image)
{
    Bytes raw(splashes::kBottomRawSize, 0);
    // the pixel at (x = 2, y = 0), top-left of the picture: column 2, last in its column, in BGR
    const size_t at = (2 * 240 + 239) * 3;
    raw[at] = 30;     // blue
    raw[at + 1] = 20; // green
    raw[at + 2] = 10; // red
    auto image = splashes::toImage(raw, false);
    REQUIRE(image.has_value());
    CHECK_EQ(image->width, 320);
    const uint8_t* pixel = image->at(2, 0);
    CHECK_EQ(pixel[0], uint8_t(10));
    CHECK_EQ(pixel[1], uint8_t(20));
    CHECK_EQ(pixel[2], uint8_t(30));
    CHECK(!splashes::toImage(Bytes(100), true).has_value());
}

TEST(splashes_luma_config)
{
    test::MemoryFs sd;
    CHECK(splashes::lumaShowsSplashes(sd)); // unknown: assume shown
    Bytes config(0x20, 0);
    config[0] = 'C';
    config[1] = 'O';
    config[2] = 'N';
    config[3] = 'F';
    sd.put(splashes::kLumaConfigPath, config);
    CHECK(!splashes::lumaShowsSplashes(sd));
    config[0xC] = 1 << 4;
    sd.put(splashes::kLumaConfigPath, config);
    CHECK(splashes::lumaShowsSplashes(sd));
}
