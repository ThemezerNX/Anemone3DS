// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/lz11.hpp"
#include "core/zip.hpp"
#include "domain/home_theme.hpp"
#include "domain/themes.hpp"
#include "fixtures.hpp"
#include "memory_fs.hpp"
#include "test.hpp"

#include <algorithm>

using namespace tz;

namespace
{

struct Console
{
    test::MemoryFs theme{true};
    test::MemoryFs home{true};

    Console()
    {
        home.put(home::kSaveDataPath, test::blankSaveData());
        auto made = themes::ensureFiles(theme, home);
        CHECK(made.has_value());
    }

    home::ThemeManage manage() { return *home::ThemeManage::parse(theme.data(home::kManagePath)); }
    home::SaveData save() { return *home::SaveData::parse(home.data(home::kSaveDataPath)); }
};

bool startsWithBytes(const Bytes& data, const Bytes& prefix)
{
    return data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin());
}

bool allZero(const Bytes& data, size_t from, size_t length)
{
    return std::all_of(data.begin() + static_cast<ptrdiff_t>(from),
                       data.begin() + static_cast<ptrdiff_t>(from + length), [](uint8_t b) { return b == 0; });
}

} // namespace

TEST(themes_ensure_files_makes_blank_storage)
{
    Console console;
    CHECK_EQ(console.theme.data(home::kManagePath).size(), size_t(home::kManageSize));
    CHECK_EQ(console.theme.data(home::kBodyCachePath).size(), size_t(home::kBodyCacheSize));
    CHECK_EQ(console.theme.data(home::kBgmCachePath).size(), size_t(home::kBgmCacheSize));
    CHECK(allZero(console.theme.data(home::kManagePath), 0, home::kManageSize));
    // a second call changes nothing
    const size_t writes = console.theme.writes();
    CHECK(themes::ensureFiles(console.theme, console.home).has_value());
    CHECK_EQ(console.theme.writes(), writes);
}

TEST(themes_ensure_files_refuses_to_lose_a_selected_theme)
{
    test::MemoryFs theme(true);
    test::MemoryFs home(true);
    Bytes save = test::blankSaveData();
    save[home::SaveData::kSelectedOffset + 5] = home::type::kCustom;
    home.put(home::kSaveDataPath, save);
    auto made = themes::ensureFiles(theme, home);
    REQUIRE(!made.has_value());
    CHECK(made.error().code == Errc::InvalidData);
    CHECK(!theme.hasFile(home::kManagePath));
}

TEST(themes_install_with_music)
{
    Console console;
    // stamps the Theme Shop uses, which an install must keep
    Bytes& manage = console.theme.data(home::kManagePath);
    manage[0x00] = 1;
    writeU32(manage.data() + 0x10, 0x4E);
    writeU32(manage.data() + 0x14, 2);
    manage[0x7F0] = 0xAB;

    const Bytes body = test::themeBody(1);
    const Bytes bgm = test::fakeBgm(0x9000, 7);
    REQUIRE(themes::install(console.theme, console.home, body, bgm).has_value());

    CHECK(startsWithBytes(console.theme.data(home::kBodyCachePath), body));
    CHECK(startsWithBytes(console.theme.data(home::kBgmCachePath), bgm));
    const home::ThemeManage after = console.manage();
    CHECK_EQ(after.bodySize(), uint32_t(body.size()));
    CHECK_EQ(after.bgmSize(), uint32_t(bgm.size()));
    CHECK_EQ(after.bytes()[0x00], uint8_t(1));
    CHECK_EQ(readU32(after.bytes().data() + 0x10), uint32_t(0x4E));
    CHECK_EQ(readU32(after.bytes().data() + 0x14), uint32_t(2));
    CHECK_EQ(after.bytes()[0x7F0], uint8_t(0xAB));
    CHECK((after.shopTheme(0) == home::ThemeDescriptor{0xFF, 0, home::type::kShop, 0, 0}));

    const home::SaveData save = console.save();
    CHECK((save.selected() == home::ThemeDescriptor{0xFF, 0, home::type::kCustom, 0, 0}));
    CHECK(!save.shuffle());

    auto current = themes::readCurrent(console.theme, console.home);
    REQUIRE(current.has_value());
    REQUIRE(current->bodies.size() == 1);
    CHECK_EQ(current->bodies[0].size, uint32_t(body.size()));
    CHECK_EQ(current->bodies[0].crc, crc32(body));
    CHECK(!current->restartToShow());
}

TEST(themes_install_without_music_clears_the_music)
{
    Console console;
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(1), test::fakeBgm(0x20000, 9)).has_value());
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(1, 0x3000, 2), {}).has_value());
    CHECK_EQ(console.manage().bgmSize(), uint32_t(0));
    CHECK(allZero(console.theme.data(home::kBgmCachePath), 0, 0x10000));
}

TEST(themes_install_keeps_a_real_shop_entry)
{
    Console console;
    Bytes& manage = console.theme.data(home::kManagePath);
    home::writeDescriptor(manage.data() + 0x18, home::ThemeDescriptor{0x23, 1, home::type::kShop, 0, 0});
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(1), {}).has_value());
    CHECK((console.manage().shopTheme(0) == home::ThemeDescriptor{0x23, 1, home::type::kShop, 0, 0}));
}

TEST(themes_install_rejects_bad_files)
{
    Console console;
    CHECK(themes::install(console.theme, console.home, test::bytesOf("not lz11"), {}).error().code ==
          Errc::InvalidData);
    CHECK(themes::install(console.theme, console.home, test::themeBody(1), test::bytesOf("RIFF....")).error().code ==
          Errc::InvalidData);
    CHECK(themes::install(console.theme, console.home, Bytes(home::kBodyCacheSize + 1, 0x11), {}).error().code ==
          Errc::TooLarge);
    // nothing was written
    CHECK(!console.save().shuffle());
    CHECK_EQ(console.save().selected().type, home::type::kDefault);
}

TEST(themes_failed_install_leaves_the_default_theme)
{
    Console console;
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(1), {}).has_value());
    console.theme.failWritesAfter(0);
    auto failed = themes::install(console.theme, console.home, test::themeBody(1, 0x4000, 3), {});
    REQUIRE(!failed.has_value());
    // the HOME Menu no longer points at a cache that may be half written
    CHECK_EQ(console.save().selected().type, home::type::kDefault);
}

TEST(themes_install_music_turns_the_music_on)
{
    Console console;
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(0), {}).has_value());
    const Bytes bgm = test::fakeBgm(0x1000, 4);
    REQUIRE(themes::installMusic(console.theme, console.home, bgm).has_value());

    const home::ThemeManage manage = console.manage();
    CHECK_EQ(manage.bgmSize(), uint32_t(bgm.size()));
    const Bytes& cache = console.theme.data(home::kBodyCachePath);
    auto plain = lz11::decompress(ByteSpan(cache.data(), manage.bodySize()), 0x100000);
    REQUIRE(plain.has_value());
    CHECK_EQ((*plain)[themes::kBodyBgmFlagOffset], uint8_t(1));
    CHECK(startsWithBytes(console.theme.data(home::kBgmCachePath), bgm));
}

TEST(themes_install_music_needs_a_custom_theme)
{
    Console console;
    auto result = themes::installMusic(console.theme, console.home, test::fakeBgm(0x100, 1));
    REQUIRE(!result.has_value());
    CHECK(result.error().code == Errc::NotFound);
}

TEST(themes_install_shuffle)
{
    Console console;
    std::vector<themes::ThemeFiles> themes;
    themes.push_back({test::themeBody(1, 0x2000, 1), test::fakeBgm(0x800, 1)});
    themes.push_back({test::themeBody(1, 0x2400, 2), {}});
    themes.push_back({test::themeBody(1, 0x2800, 3), test::fakeBgm(0x900, 3)});

    int reported = 0;
    auto installed = themes::installShuffle(
        console.theme, console.home, 3, [&](int i) -> Expected<themes::ThemeFiles> { return themes[i]; },
        [&](int done, int total) {
            CHECK_EQ(total, 3);
            reported = done;
            return true;
        });
    REQUIRE(installed.has_value());
    CHECK_EQ(reported, 3);

    const home::ThemeManage manage = console.manage();
    const Bytes& bodies = console.theme.data(home::kShuffleBodyCachePath);
    CHECK_EQ(bodies.size(), size_t(home::kShuffleBodyCacheSize));
    for (int i = 0; i < 3; ++i)
    {
        CHECK_EQ(manage.shuffleBodySize(i), uint32_t(themes[i].body.size()));
        CHECK_EQ(manage.shuffleBgmSize(i), uint32_t(themes[i].bgm.size()));
        CHECK(std::equal(themes[i].body.begin(), themes[i].body.end(),
                         bodies.begin() + static_cast<ptrdiff_t>(i) * home::kBodyCacheSize));
    }
    for (int i = 0; i < home::kShuffleSlots; ++i)
        CHECK(console.theme.hasFile(home::shuffleBgmCachePath(i)));
    CHECK_EQ(manage.shuffleBodySize(5), uint32_t(0));

    const home::SaveData save = console.save();
    CHECK(save.shuffle());
    CHECK((save.shuffleTheme(2) == home::ThemeDescriptor{2, 0, home::type::kCustom, 0, 0}));
    CHECK((save.shuffleTheme(3) == home::ThemeDescriptor{}));

    auto current = themes::readCurrent(console.theme, console.home);
    REQUIRE(current.has_value());
    CHECK_EQ(current->bodies.size(), size_t(3));
    CHECK(current->restartToShow());
}

TEST(themes_shuffle_needs_two_themes)
{
    Console console;
    auto result = themes::installShuffle(
        console.theme, console.home, 1, [](int) -> Expected<themes::ThemeFiles> { return themes::ThemeFiles{}; }, {});
    CHECK(!result.has_value());
}

TEST(themes_reset_to_default)
{
    Console console;
    Bytes& manage = console.theme.data(home::kManagePath);
    home::writeDescriptor(manage.data() + 0x20, home::ThemeDescriptor{0x11, 2, home::type::kShop, 0, 0});
    REQUIRE(themes::install(console.theme, console.home, test::themeBody(1), {}).has_value());
    REQUIRE(themes::resetToDefault(console.theme, console.home).has_value());
    CHECK_EQ(console.save().selected().type, home::type::kDefault);
    CHECK_EQ(console.manage().bodySize(), uint32_t(0));
    // the downloaded Theme Shop themes stay listed
    CHECK((console.manage().shopTheme(1) == home::ThemeDescriptor{0x11, 2, home::type::kShop, 0, 0}));
    auto current = themes::readCurrent(console.theme, console.home);
    REQUIRE(current.has_value());
    CHECK(!current->hasCustomTheme());
}

TEST(themes_read_installed)
{
    Console console;
    const Bytes body = test::themeBody(1);
    const Bytes bgm = test::fakeBgm(0x700, 5);
    REQUIRE(themes::install(console.theme, console.home, body, bgm).has_value());
    auto files = themes::readInstalled(console.theme, console.home);
    REQUIRE(files.has_value());
    CHECK(files->body == body);
    CHECK(files->bgm == bgm);
}
