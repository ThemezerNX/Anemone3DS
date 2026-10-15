// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/image.hpp"
#include "core/smdh.hpp"
#include "core/utf.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace tz;

namespace
{

std::array<uint16_t, 48 * 48> solidIcon(uint16_t colour)
{
    std::array<uint16_t, 48 * 48> icon;
    icon.fill(colour);
    return icon;
}

void setTitle(Bytes& smdh, int language, std::string_view name, std::string_view description = {},
              std::string_view author = {})
{
    uint8_t* title = smdh.data() + 0x8 + static_cast<size_t>(language) * 0x200;
    utf::toUtf16Le(name, title, 0x40);
    utf::toUtf16Le(description, title + 0x80, 0x80);
    utf::toUtf16Le(author, title + 0x180, 0x40);
}

} // namespace

TEST(smdh_build_layout)
{
    const SmdhText text{"Th\xC3\xA8me \xF0\x9F\x8E\xB5", "A description", "Someone"};
    const Bytes smdh = buildSmdh(text, solidIcon(0xF800));
    REQUIRE(smdh.size() == kSmdhSize);
    CHECK(std::memcmp(smdh.data(), "SMDH", 4) == 0);
    CHECK_EQ(readU16(smdh.data() + 4), uint16_t(0));
    for (int language = 0; language < 16; ++language)
    {
        const uint8_t* title = smdh.data() + 0x8 + language * 0x200;
        CHECK_EQ(utf::fromUtf16Le(title, 0x40), text.name);
        CHECK_EQ(utf::fromUtf16Le(title + 0x80, 0x80), text.description);
        CHECK_EQ(utf::fromUtf16Le(title + 0x180, 0x40), text.author);
    }
    // the settings are left zero
    CHECK(std::all_of(smdh.begin() + 0x2008, smdh.begin() + 0x2040, [](uint8_t b) { return b == 0; }));
    // both icons, the small one scaled down from the large
    CHECK_EQ(readU16(smdh.data() + 0x24C0), uint16_t(0xF800));
    CHECK_EQ(readU16(smdh.data() + 0x24C0 + 0x1200 - 2), uint16_t(0xF800));
    CHECK_EQ(readU16(smdh.data() + 0x2040), uint16_t(0xF800));
    CHECK_EQ(readU16(smdh.data() + 0x2040 + 0x480 - 2), uint16_t(0xF800));
}

TEST(smdh_roundtrip)
{
    std::array<uint16_t, 48 * 48> icon{};
    for (size_t i = 0; i < icon.size(); ++i)
        icon[i] = static_cast<uint16_t>(i * 31);
    const Bytes smdh = buildSmdh({"Name", "Long description", "Author"}, icon);
    auto parsed = parseSmdh(smdh);
    REQUIRE(parsed.has_value());
    CHECK(parsed->largeIcon == icon);
    CHECK_EQ(parsed->title(1).name, std::string("Name"));
    CHECK_EQ(parsed->title(1).description, std::string("Long description"));
    CHECK_EQ(parsed->title(1).author, std::string("Author"));

    // the small icon is the large one at half the size: a quarter of the pixels, averaged
    Image large = iconToImage(icon.data(), 48);
    Image small = iconToImage(parsed->smallIcon.data(), 24);
    const Image expected = resize(large, 24, 24);
    int far = 0;
    for (size_t i = 0; i < small.pixels.size(); ++i)
        far += std::abs(small.pixels[i] - expected.pixels[i]) > 8;
    CHECK_EQ(far, 0);
}

TEST(smdh_titles)
{
    Bytes smdh(kSmdhSize, 0);
    std::memcpy(smdh.data(), "SMDH", 4);
    setTitle(smdh, 0, "  Japanese  ", "desc", "\tme\n");
    setTitle(smdh, 3, "German");
    auto parsed = parseSmdh(smdh);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->titles[0].name, std::string("Japanese")); // trimmed
    CHECK_EQ(parsed->titles[0].author, std::string("me"));
    CHECK_EQ(parsed->title(3).name, std::string("German"));
    // no English: the first one with a name
    CHECK_EQ(parsed->title(5).name, std::string("Japanese"));
    CHECK_EQ(parsed->title(-1).name, std::string("Japanese"));
    CHECK_EQ(parsed->title(99).name, std::string("Japanese"));

    setTitle(smdh, 1, "English");
    parsed = parseSmdh(smdh);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->title(5).name, std::string("English"));
    CHECK_EQ(parsed->title(3).name, std::string("German"));

    // none at all: the first title, empty
    Bytes empty(kSmdhSize, 0);
    std::memcpy(empty.data(), "SMDH", 4);
    auto none = parseSmdh(empty);
    REQUIRE(none.has_value());
    CHECK(none->title(1).name.empty());
    CHECK(&none->title(4) == &none->titles[0]);
}

TEST(smdh_limits)
{
    // too long: cut to the field, on a character boundary
    const std::string name(100, 'n');
    const std::string author = "\xF0\x9F\x8E\xB5" + std::string(62, 'a') + "\xF0\x9F\x8E\xB5";
    const Bytes smdh = buildSmdh({name, std::string(200, 'd'), author}, solidIcon(0));
    auto parsed = parseSmdh(smdh);
    REQUIRE(parsed.has_value());
    CHECK_EQ(parsed->title(0).name, std::string(64, 'n'));
    CHECK_EQ(parsed->title(0).description, std::string(128, 'd'));
    CHECK_EQ(parsed->title(0).author, "\xF0\x9F\x8E\xB5" + std::string(62, 'a'));

    CHECK(parseSmdh(ByteSpan(smdh).first(kSmdhSize - 1)).error().code == Errc::InvalidData);
    Bytes wrong = smdh;
    wrong[0] = 'X';
    CHECK(parseSmdh(wrong).error().code == Errc::InvalidData);
    // longer is fine (an SMDH inside something bigger)
    Bytes longer = smdh;
    longer.resize(kSmdhSize + 100);
    CHECK(parseSmdh(longer).has_value());
}
