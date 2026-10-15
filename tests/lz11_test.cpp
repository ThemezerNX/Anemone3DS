// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/lz11.hpp"
#include "test.hpp"

#include <random>
#include <string>

using namespace tz;

namespace
{

Bytes withHeader(uint32_t size, std::initializer_list<uint8_t> body)
{
    Bytes data = {0x11, static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size >> 16)};
    data.insert(data.end(), body);
    return data;
}

bool roundTrips(const Bytes& data)
{
    const Bytes packed = lz11::compress(data);
    if (lz11::decompressedSize(packed) != data.size() && !data.empty())
        return false;
    auto unpacked = lz11::decompress(packed, data.size());
    return unpacked && *unpacked == data;
}

Bytes randomBytes(size_t size, uint32_t seed)
{
    std::mt19937 rng(seed);
    Bytes data(size);
    for (uint8_t& b : data)
        b = static_cast<uint8_t>(rng());
    return data;
}

// Something like a theme body: RGB565 pixels of gradients and dithered noise, repeated tiles, a little text.
Bytes textureLike(size_t size, uint32_t seed)
{
    std::mt19937 rng(seed);
    Bytes data;
    data.reserve(size);
    while (data.size() < size)
    {
        switch (rng() % 4)
        {
        case 0: // a gradient
            for (int i = 0, base = static_cast<int>(rng() % 0xFFFF); i < 512; ++i)
            {
                const uint16_t pixel = static_cast<uint16_t>(base + i / 8);
                data.push_back(static_cast<uint8_t>(pixel));
                data.push_back(static_cast<uint8_t>(pixel >> 8));
            }
            break;
        case 1: // a dithered colour
            for (int i = 0; i < 700; ++i)
            {
                const uint16_t pixel = static_cast<uint16_t>(0xDEDB + (rng() % 3));
                data.push_back(static_cast<uint8_t>(pixel));
                data.push_back(static_cast<uint8_t>(pixel >> 8));
            }
            break;
        case 2: // a copy of something earlier
            if (data.size() > 5000)
            {
                const size_t from = data.size() - 1 - rng() % 4000;
                const size_t length = 16 + rng() % 600;
                for (size_t i = 0; i < length; ++i)
                    data.push_back(data[from + i]);
            }
            break;
        default:
            for (char c : std::string("ThemeName\0\0\0Author 1.0\x01\x02"))
                data.push_back(static_cast<uint8_t>(c));
            break;
        }
    }
    data.resize(size);
    return data;
}

} // namespace

TEST(lz11_header)
{
    CHECK_EQ(lz11::decompressedSize(withHeader(0x123456, {})), uint32_t(0x123456));
    // a 24-bit size of 0: the size is the u32 after it
    const Bytes extended = {0x11, 0, 0, 0, 0x78, 0x56, 0x34, 0x12};
    CHECK_EQ(lz11::decompressedSize(extended), uint32_t(0x12345678));
    CHECK_EQ(lz11::decompressedSize(Bytes{0x11, 0, 0, 0}), uint32_t(0));
    CHECK_EQ(lz11::decompressedSize(Bytes{0x10, 5, 0, 0}), uint32_t(0));
    CHECK_EQ(lz11::decompressedSize(Bytes{0x11, 5}), uint32_t(0));
    CHECK(!lz11::decompress(Bytes{0x10, 1, 0, 0, 0, 'a'}, 100));
}

TEST(lz11_literals)
{
    const auto out = lz11::decompress(withHeader(5, {0x00, 'h', 'e', 'l', 'l', 'o'}), 100);
    REQUIRE(out.has_value());
    CHECK_EQ(std::string(out->begin(), out->end()), std::string("hello"));
}

TEST(lz11_short_copy)
{
    // "abc", then 6 bytes from 3 back: nibble 6 - 1 = 5, distance - 1 = 2
    const auto out = lz11::decompress(withHeader(9, {0x10, 'a', 'b', 'c', 0x50, 0x02}), 100);
    REQUIRE(out.has_value());
    CHECK_EQ(std::string(out->begin(), out->end()), std::string("abcabcabc"));

    // the shortest (3) and longest (16) of this encoding, the farthest distance (4096)
    Bytes data = withHeader(4096 + 3 + 16, {});
    Bytes expected;
    for (int group = 0; group < 512; ++group)
    {
        data.push_back(0x00);
        for (int i = 0; i < 8; ++i)
        {
            data.push_back(static_cast<uint8_t>(group * 8 + i));
            expected.push_back(static_cast<uint8_t>(group * 8 + i));
        }
    }
    data.insert(data.end(), {0xC0, 0x2F, 0xFF, 0xFF, 0xFF});
    for (int i = 0; i < 3; ++i)
        expected.push_back(expected[expected.size() - 4096]);
    for (int i = 0; i < 16; ++i)
        expected.push_back(expected[expected.size() - 4096]);
    const auto out2 = lz11::decompress(data, 1 << 20);
    REQUIRE(out2.has_value());
    CHECK(*out2 == expected);
}

TEST(lz11_medium_copy)
{
    // 'x', then 0x20 bytes from 1 back: 0x20 - 0x11 = 0x0F in 8 bits split over two nibbles
    const auto out = lz11::decompress(withHeader(0x21, {0x40, 'x', 0x00, 0xF0, 0x00}), 100);
    REQUIRE(out.has_value());
    CHECK(*out == Bytes(0x21, 'x'));

    // the ends of the range: 0x11 and 0x110
    const auto low = lz11::decompress(withHeader(0x12, {0x40, 'q', 0x00, 0x00, 0x00}), 1000);
    REQUIRE(low.has_value());
    CHECK(*low == Bytes(0x12, 'q'));
    const auto high = lz11::decompress(withHeader(0x111, {0x40, 'r', 0x0F, 0xF0, 0x00}), 1000);
    REQUIRE(high.has_value());
    CHECK(*high == Bytes(0x111, 'r'));
}

TEST(lz11_long_copy)
{
    // 'y', then 0x1000 bytes: 0x1000 - 0x111 = 0xEEF in 16 bits
    const auto out = lz11::decompress(withHeader(0x1001, {0x40, 'y', 0x10, 0xEE, 0xF0, 0x00}), 0x10000);
    REQUIRE(out.has_value());
    CHECK(*out == Bytes(0x1001, 'y'));

    // the ends of the range: 0x111 and 0x10110
    const auto low = lz11::decompress(withHeader(0x112, {0x40, 's', 0x10, 0x00, 0x00, 0x00}), 0x100000);
    REQUIRE(low.has_value());
    CHECK(*low == Bytes(0x112, 's'));
    const auto high = lz11::decompress(withHeader(0x10111, {0x40, 't', 0x1F, 0xFF, 0xF0, 0x00}), 0x100000);
    REQUIRE(high.has_value());
    CHECK(*high == Bytes(0x10111, 't'));
}

TEST(lz11_extended_header)
{
    const Bytes data = {0x11, 0, 0, 0, 3, 0, 0, 0, 0x00, 'a', 'b', 'c'};
    const auto out = lz11::decompress(data, 100);
    REQUIRE(out.has_value());
    CHECK_EQ(std::string(out->begin(), out->end()), std::string("abc"));
    const auto empty = lz11::decompress(Bytes{0x11, 0, 0, 0, 0, 0, 0, 0}, 100);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST(lz11_corrupt)
{
    // ends early: in the flags, a literal, and each kind of copy
    CHECK(lz11::decompress(withHeader(5, {}), 100).error().code == Errc::InvalidData);
    CHECK(lz11::decompress(withHeader(5, {0x00, 'a', 'b'}), 100).error().code == Errc::InvalidData);
    CHECK(lz11::decompress(withHeader(9, {0x10, 'a', 'b', 'c', 0x50}), 100).error().code == Errc::InvalidData);
    CHECK(lz11::decompress(withHeader(0x21, {0x40, 'x', 0x00, 0xF0}), 100).error().code == Errc::InvalidData);
    CHECK(lz11::decompress(withHeader(0x1001, {0x40, 'y', 0x10, 0xEE, 0xF0}), 0x10000).error().code ==
          Errc::InvalidData);
    // a copy from before the start
    CHECK(lz11::decompress(withHeader(9, {0x10, 'a', 'b', 'c', 0x50, 0x05}), 100).error().code == Errc::InvalidData);
    CHECK(lz11::decompress(withHeader(4, {0x80, 0x20, 0x00}), 100).error().code == Errc::InvalidData);
    // bigger than allowed
    CHECK(lz11::decompress(withHeader(101, {0x00, 'a'}), 100).error().code == Errc::TooLarge);
    // a size no data this short could hold is refused before anything is allocated
    CHECK(lz11::decompress(Bytes{0x11, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x7F, 0x00}, SIZE_MAX).error().code ==
          Errc::InvalidData);

    // a copy running past the stated size is cut there
    const auto cut = lz11::decompress(withHeader(4, {0x40, 'z', 0x50, 0x00}), 100);
    REQUIRE(cut.has_value());
    CHECK(*cut == Bytes(4, 'z'));
}

TEST(lz11_fuzz)
{
    // nothing crashes or reads out of bounds (the sanitizers watch), whatever the input
    std::mt19937 rng(1234);
    const Bytes valid = lz11::compress(textureLike(20000, 7));
    for (int round = 0; round < 3000; ++round)
    {
        Bytes data;
        if (round % 2 == 0)
        {
            data = valid;
            for (int i = 0, n = 1 + static_cast<int>(rng() % 8); i < n; ++i)
                data[4 + rng() % (data.size() - 4)] = static_cast<uint8_t>(rng());
            if (round % 6 == 0)
                data.resize(rng() % data.size());
        }
        else
        {
            data = randomBytes(rng() % 300, static_cast<uint32_t>(round));
            if (!data.empty())
                data[0] = 0x11;
        }
        auto out = lz11::decompress(data, 1 << 20);
        if (out)
            CHECK_EQ(out->size(), size_t(lz11::decompressedSize(data)));
    }
}

TEST(lz11_roundtrip_small)
{
    CHECK(roundTrips({}));
    for (size_t size : {1, 2, 3, 4, 5, 8, 9, 15, 16, 17, 100, 4095, 4096, 4097, 8193})
    {
        CHECK(roundTrips(randomBytes(size, static_cast<uint32_t>(size))));
        CHECK(roundTrips(Bytes(size, 0xAB)));
    }
    // an empty input still gets a header that says so
    CHECK(lz11::compress({}) == (Bytes{0x11, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(lz11_roundtrip_random)
{
    const Bytes data = randomBytes(100000, 42);
    const Bytes packed = lz11::compress(data);
    // incompressible: no worse than a flag byte for every eight literals
    CHECK(packed.size() <= data.size() + data.size() / 8 + 9);
    auto unpacked = lz11::decompress(packed, data.size());
    REQUIRE(unpacked.has_value());
    CHECK(*unpacked == data);
}

TEST(lz11_roundtrip_repetitive)
{
    // runs longer than the longest copy, periods of every size up to the window and past it
    Bytes zeros(0x30000, 0);
    const Bytes packed = lz11::compress(zeros);
    CHECK(packed.size() < 64);
    CHECK(roundTrips(zeros));
    for (size_t period : {1, 2, 3, 7, 64, 1000, 4095, 4096, 4097, 5000})
    {
        Bytes data(70000);
        const Bytes unit = randomBytes(period, static_cast<uint32_t>(period));
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = unit[i % period];
        CHECK(roundTrips(data));
        if (period <= 4096)
            CHECK(lz11::compress(data).size() < period + 2000);
    }
}

TEST(lz11_roundtrip_mixed)
{
    const Bytes texture = textureLike(1500000, 3);
    const Bytes packed = lz11::compress(texture);
    CHECK(packed.size() < texture.size() / 2);
    auto unpacked = lz11::decompress(packed, texture.size());
    REQUIRE(unpacked.has_value());
    CHECK(*unpacked == texture);

    // random and repetitive stretches next to each other
    Bytes mixed;
    for (int i = 0; i < 40; ++i)
    {
        const Bytes noise = randomBytes(3000 + static_cast<size_t>(i) * 37, static_cast<uint32_t>(i));
        mixed.insert(mixed.end(), noise.begin(), noise.end());
        mixed.insert(mixed.end(), 2000 + static_cast<size_t>(i) * 101, static_cast<uint8_t>(i));
        mixed.insert(mixed.end(), noise.begin(), noise.begin() + 1500);
    }
    CHECK(mixed.size() > 0x10000);
    CHECK(roundTrips(mixed));
}

TEST(lz11_roundtrip_huge)
{
    // more than 24 bits of size: the 32-bit header
    Bytes data(0x1000010, 0x5A);
    for (size_t i = 0; i < data.size(); i += 4099)
        data[i] = static_cast<uint8_t>(i);
    const Bytes packed = lz11::compress(data);
    CHECK_EQ(packed[1] | packed[2] | packed[3], 0);
    CHECK_EQ(lz11::decompressedSize(packed), uint32_t(data.size()));
    auto unpacked = lz11::decompress(packed, data.size());
    REQUIRE(unpacked.has_value());
    CHECK(*unpacked == data);
}
