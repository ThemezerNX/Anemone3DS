// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/path.hpp"
#include "core/utf.hpp"
#include "memory_fs.hpp"
#include "test.hpp"

using namespace tz;

TEST(path_parts)
{
    CHECK_EQ(path::filename("/Themes/My Theme.zip"), std::string_view("My Theme.zip"));
    CHECK_EQ(path::parent("/Themes/My Theme.zip"), std::string_view("/Themes"));
    CHECK_EQ(path::parent("/a.zip"), std::string_view("/"));
    CHECK_EQ(path::extension("/Themes/a.b.ZIP"), std::string_view("ZIP"));
    CHECK_EQ(path::extension("/Themes/.hidden"), std::string_view(""));
    CHECK_EQ(path::stem("/Themes/a.b.zip"), std::string_view("a.b"));
    CHECK_EQ(path::join("/Themes/", "/x.zip"), std::string("/Themes/x.zip"));
    CHECK(path::hasExtension("x.Zip", "zip"));
    CHECK_EQ(path::split("//a//b/").size(), size_t(2));
}

TEST(sanitize_filename)
{
    CHECK_EQ(path::sanitizeFilename("a/b:c?"), std::string("a-b-c-"));
    CHECK_EQ(path::sanitizeFilename("  name. . "), std::string("name"));
    CHECK_EQ(path::sanitizeFilename("..."), std::string("untitled"));
    // cut on a character boundary: "é" is two bytes
    CHECK_EQ(path::sanitizeFilename("aé", 2), std::string("a"));
}

TEST(compare_ignore_case)
{
    CHECK(path::equalsIgnoreCase("Body_LZ.bin", "body_lz.BIN"));
    CHECK(path::compareIgnoreCase("apple", "Banana") < 0);
    CHECK(path::compareIgnoreCase("b", "A") > 0);
    CHECK(path::compareIgnoreCase("ab", "AB") == 0);
}

TEST(utf_roundtrip)
{
    const std::string text = "Thème 🎵 テーマ";
    CHECK_EQ(utf::fromUtf16(utf::toUtf16(text)), text);
    CHECK_EQ(utf::length(text), size_t(11));
    CHECK_EQ(utf::prefix(text, 4), std::string_view("Thèm"));

    uint8_t buffer[16];
    utf::toUtf16Le(text, buffer, 8);
    CHECK_EQ(utf::fromUtf16Le(buffer, 8), std::string("Thème 🎵"));
}

TEST(utf_invalid)
{
    const std::string broken = "a\xC3";
    CHECK_EQ(utf::fromUtf16(utf::toUtf16(broken)), std::string("a\xEF\xBF\xBD"));
    // a surrogate pair cut in half is dropped, not left behind
    uint8_t buffer[4];
    utf::toUtf16Le("a🎵", buffer, 2);
    CHECK_EQ(utf::fromUtf16Le(buffer, 2), std::string("a"));
}

TEST(memory_fs_files)
{
    test::MemoryFs fs;
    CHECK(fs.createDirectories("/3ds/Themezer/cache").has_value());
    CHECK(fs.writeFile("/3ds/Themezer/cache/a.bin", Bytes{1, 2, 3}).has_value());
    auto data = fs.readFile("/3ds/Themezer/cache/a.bin", 10);
    REQUIRE(data.has_value());
    CHECK_EQ(data->size(), size_t(3));
    CHECK_EQ(fs.readFile("/3ds/Themezer/cache/a.bin", 2).error().code == Errc::TooLarge, true);
    auto entries = fs.list("/3ds/Themezer/cache");
    REQUIRE(entries.has_value());
    CHECK_EQ(entries->size(), size_t(1));
}
