// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/image.hpp"
#include "test.hpp"

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>

using namespace tz;

namespace
{

// PNGs of every kind, written with libpng from raw rows.
struct RawPng
{
    int width = 0;
    int height = 0;
    int colorType = PNG_COLOR_TYPE_RGB_ALPHA;
    int bitDepth = 8;
    bool interlaced = false;
    std::vector<png_color> palette;
    std::vector<uint8_t> paletteAlpha;
    bool colorKey = false;
    png_color_16 key{};
    Bytes rows;
    size_t rowBytes = 0;
};

int channels(int colorType)
{
    switch (colorType)
    {
    case PNG_COLOR_TYPE_GRAY:
    case PNG_COLOR_TYPE_PALETTE:
        return 1;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
        return 2;
    case PNG_COLOR_TYPE_RGB:
        return 3;
    default:
        return 4;
    }
}

// Packs the samples (each below 1 << bitDepth) into rows, high bits first, 16-bit samples big endian.
void fillRows(RawPng& png, const std::function<uint16_t(int x, int y, int channel)>& sample)
{
    const int count = channels(png.colorType);
    png.rowBytes = (static_cast<size_t>(png.width) * count * png.bitDepth + 7) / 8;
    png.rows.assign(png.rowBytes * static_cast<size_t>(png.height), 0);
    for (int y = 0; y < png.height; ++y)
    {
        uint8_t* row = png.rows.data() + static_cast<size_t>(y) * png.rowBytes;
        size_t bit = 0;
        for (int x = 0; x < png.width; ++x)
        {
            for (int c = 0; c < count; ++c, bit += static_cast<size_t>(png.bitDepth))
            {
                const uint16_t value = sample(x, y, c);
                if (png.bitDepth == 16)
                {
                    row[bit / 8] = static_cast<uint8_t>(value >> 8);
                    row[bit / 8 + 1] = static_cast<uint8_t>(value);
                }
                else
                {
                    row[bit / 8] |= static_cast<uint8_t>(value << (8 - png.bitDepth - bit % 8));
                }
            }
        }
    }
}

void appendTo(png_structp png, png_bytep data, size_t length)
{
    auto* out = static_cast<Bytes*>(png_get_io_ptr(png));
    out->insert(out->end(), data, data + length);
}

void noFlush(png_structp)
{
}

bool writeRaw(const RawPng& raw, Bytes& out)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        return false;
    }
    png_set_write_fn(png, &out, appendTo, noFlush);
    png_set_IHDR(png, info, static_cast<png_uint_32>(raw.width), static_cast<png_uint_32>(raw.height), raw.bitDepth,
                 raw.colorType, raw.interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    if (!raw.palette.empty())
        png_set_PLTE(png, info, raw.palette.data(), static_cast<int>(raw.palette.size()));
    if (!raw.paletteAlpha.empty())
        png_set_tRNS(png, info, raw.paletteAlpha.data(), static_cast<int>(raw.paletteAlpha.size()), nullptr);
    if (raw.colorKey)
        png_set_tRNS(png, info, nullptr, 0, &raw.key);
    png_write_info(png, info);
    const int passes = png_set_interlace_handling(png);
    for (int pass = 0; pass < passes; ++pass)
        for (int y = 0; y < raw.height; ++y)
            png_write_row(png, raw.rows.data() + static_cast<size_t>(y) * raw.rowBytes);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return true;
}

Bytes makePng(RawPng raw, const std::function<uint16_t(int, int, int)>& sample)
{
    fillRows(raw, sample);
    Bytes out;
    if (!writeRaw(raw, out))
        std::abort();
    return out;
}

bool pixelIs(const Image& image, int x, int y, int r, int g, int b, int a, int tolerance = 0)
{
    const uint8_t* p = image.at(x, y);
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance &&
           std::abs(p[3] - a) <= tolerance;
}

Image randomImage(int width, int height, uint32_t seed, bool opaque)
{
    std::mt19937 rng(seed);
    Image image(width, height);
    for (size_t i = 0; i < image.pixels.size(); ++i)
        image.pixels[i] = (opaque && i % 4 == 3) ? 0xFF : static_cast<uint8_t>(rng());
    return image;
}

void putChunk(Bytes& png, const char* type, std::initializer_list<uint8_t> body)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        png.push_back(static_cast<uint8_t>(body.size() >> shift));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), body);
    const auto crc = static_cast<uint32_t>(::crc32(0, png.data() + start, static_cast<uInt>(png.size() - start)));
    for (int shift = 24; shift >= 0; shift -= 8)
        png.push_back(static_cast<uint8_t>(crc >> shift));
}

// A PNG with a header and the start of its image data, for testing the limits without a whole image.
Bytes headerOnly(uint32_t width, uint32_t height, bool withData = true)
{
    Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    putChunk(png, "IHDR",
             {static_cast<uint8_t>(width >> 24), static_cast<uint8_t>(width >> 16), static_cast<uint8_t>(width >> 8),
              static_cast<uint8_t>(width), static_cast<uint8_t>(height >> 24), static_cast<uint8_t>(height >> 16),
              static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height), 8, 6, 0, 0, 0});
    if (withData)
        putChunk(png, "IDAT", {0x78, 0x9C, 0x03, 0x00});
    putChunk(png, "IEND", {});
    return png;
}

} // namespace

TEST(png_roundtrip)
{
    const Image image = randomImage(37, 23, 1, false);
    auto png = encodePng(image);
    REQUIRE(png.has_value());
    CHECK_EQ(int((*png)[25]), PNG_COLOR_TYPE_RGB_ALPHA);
    auto decoded = decodePng(*png);
    REQUIRE(decoded.has_value());
    CHECK_EQ(decoded->width, 37);
    CHECK_EQ(decoded->height, 23);
    CHECK(decoded->pixels == image.pixels);

    // opaque images are written as RGB
    const Image opaque = randomImage(16, 9, 2, true);
    auto rgb = encodePng(opaque);
    REQUIRE(rgb.has_value());
    CHECK_EQ(int((*rgb)[25]), PNG_COLOR_TYPE_RGB);
    auto decodedRgb = decodePng(*rgb);
    REQUIRE(decodedRgb.has_value());
    CHECK(decodedRgb->pixels == opaque.pixels);

    CHECK(encodePng(Image()).error().code == Errc::InvalidData);
}

TEST(png_palette_and_grey)
{
    RawPng palette{4, 2, PNG_COLOR_TYPE_PALETTE, 8};
    palette.palette = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {10, 20, 30}};
    palette.paletteAlpha = {255, 128, 0}; // the fourth colour has no entry: opaque
    auto image = decodePng(makePng(palette, [](int x, int, int) { return uint16_t(x); }));
    REQUIRE(image.has_value());
    CHECK(pixelIs(*image, 0, 0, 255, 0, 0, 255));
    CHECK(pixelIs(*image, 1, 1, 0, 255, 0, 128));
    CHECK(pixelIs(*image, 2, 0, 0, 0, 255, 0));
    CHECK(pixelIs(*image, 3, 1, 10, 20, 30, 255));

    RawPng palette2{9, 1, PNG_COLOR_TYPE_PALETTE, 2};
    palette2.palette = {{0, 0, 0}, {100, 100, 100}, {200, 200, 200}, {250, 0, 250}};
    auto small = decodePng(makePng(palette2, [](int x, int, int) { return uint16_t(x % 4); }));
    REQUIRE(small.has_value());
    CHECK(pixelIs(*small, 3, 0, 250, 0, 250, 255));
    CHECK(pixelIs(*small, 8, 0, 0, 0, 0, 255));

    for (int depth : {1, 2, 4, 8})
    {
        RawPng grey{11, 3, PNG_COLOR_TYPE_GRAY, depth};
        const int levels = 1 << depth;
        auto decoded = decodePng(makePng(grey, [&](int x, int y, int) { return uint16_t((x + y) % levels); }));
        REQUIRE(decoded.has_value());
        const int value = 255 / (levels - 1) * ((2 + 1) % levels);
        CHECK(pixelIs(*decoded, 2, 1, value, value, value, 255));
    }

    RawPng greyAlpha{3, 3, PNG_COLOR_TYPE_GRAY_ALPHA, 8};
    auto ga = decodePng(makePng(greyAlpha, [](int x, int y, int c) { return uint16_t(c == 0 ? x * 50 : y * 100); }));
    REQUIRE(ga.has_value());
    CHECK(pixelIs(*ga, 2, 1, 100, 100, 100, 100));

    // a grey colour key
    RawPng keyed{2, 1, PNG_COLOR_TYPE_GRAY, 8};
    keyed.colorKey = true;
    keyed.key.gray = 7;
    auto k = decodePng(makePng(keyed, [](int x, int, int) { return uint16_t(x == 0 ? 7 : 9); }));
    REQUIRE(k.has_value());
    CHECK_EQ(int(k->at(0, 0)[3]), 0);
    CHECK(pixelIs(*k, 1, 0, 9, 9, 9, 255));
}

TEST(png_deep_and_keyed_colour)
{
    // 16 bits per sample: v * 257 scales back to v exactly
    RawPng deep{5, 4, PNG_COLOR_TYPE_RGB_ALPHA, 16};
    auto image = decodePng(makePng(deep, [](int x, int y, int c) { return uint16_t((x * 40 + y * 3 + c * 5) * 257); }));
    REQUIRE(image.has_value());
    CHECK(pixelIs(*image, 4, 3, 169, 174, 179, 184));

    RawPng deepRgb{2, 2, PNG_COLOR_TYPE_RGB, 16};
    auto rgb = decodePng(makePng(deepRgb, [](int x, int, int c) { return uint16_t((x * 100 + c) * 257); }));
    REQUIRE(rgb.has_value());
    CHECK(pixelIs(*rgb, 1, 1, 100, 101, 102, 255));

    RawPng keyed{3, 1, PNG_COLOR_TYPE_RGB, 8};
    keyed.colorKey = true;
    keyed.key.red = 1;
    keyed.key.green = 2;
    keyed.key.blue = 3;
    auto k = decodePng(makePng(keyed, [](int x, int, int c) { return uint16_t(x == 1 ? c + 1 : 200); }));
    REQUIRE(k.has_value());
    CHECK(pixelIs(*k, 0, 0, 200, 200, 200, 255));
    CHECK_EQ(int(k->at(1, 0)[3]), 0);
}

TEST(png_interlaced)
{
    RawPng raw{13, 11, PNG_COLOR_TYPE_RGB_ALPHA, 8, true};
    auto sample = [](int x, int y, int c) { return uint16_t((x * 17 + y * 23 + c * 61) & 0xFF); };
    auto image = decodePng(makePng(raw, sample));
    REQUIRE(image.has_value());
    bool same = true;
    for (int y = 0; y < 11; ++y)
        for (int x = 0; x < 13; ++x)
            for (int c = 0; c < 4; ++c)
                same = same && image->at(x, y)[c] == sample(x, y, c);
    CHECK(same);

    // scaled down after decoding
    RawPng big{600, 400, PNG_COLOR_TYPE_RGB, 8, true};
    auto scaled = decodePng(makePng(big, [](int x, int y, int) { return uint16_t((x + y) % 2 ? 255 : 0); }), 300, 300);
    REQUIRE(scaled.has_value());
    CHECK_EQ(scaled->width, 300);
    CHECK_EQ(scaled->height, 200);
    CHECK(pixelIs(*scaled, 150, 100, 128, 128, 128, 255, 1));
}

TEST(png_scaled_while_decoding)
{
    // whole blocks: plain averages
    RawPng blocks{4, 4, PNG_COLOR_TYPE_RGB_ALPHA, 8};
    auto image =
        decodePng(makePng(blocks, [](int x, int y, int c) { return uint16_t(c == 3 ? 255 : (y * 4 + x) * 10); }), 2, 2);
    REQUIRE(image.has_value());
    CHECK_EQ(image->width, 2);
    CHECK_EQ(image->height, 2);
    CHECK(pixelIs(*image, 0, 0, 25, 25, 25, 255));  // (0 + 10 + 40 + 50) / 4
    CHECK(pixelIs(*image, 1, 1, 125, 125, 125, 255)); // (100 + 110 + 140 + 150) / 4

    // three pixels into two: the middle one is shared
    RawPng thirds{3, 1, PNG_COLOR_TYPE_RGB, 8};
    auto shared = decodePng(makePng(thirds, [](int x, int, int) { return uint16_t(x == 0 ? 0 : x == 1 ? 90 : 180); }),
                            2, 2);
    REQUIRE(shared.has_value());
    CHECK_EQ(shared->width, 2);
    CHECK_EQ(shared->height, 1);
    CHECK(pixelIs(*shared, 0, 0, 30, 30, 30, 255, 1));
    CHECK(pixelIs(*shared, 1, 0, 150, 150, 150, 255, 1));

    // transparent pixels don't darken what they're averaged with
    RawPng edge{2, 1, PNG_COLOR_TYPE_RGB_ALPHA, 8};
    auto opaqueRed = [](int x, int, int c) { return uint16_t(x == 0 && (c == 0 || c == 3) ? 255 : 0); };
    auto edged = decodePng(makePng(edge, opaqueRed), 1, 1);
    REQUIRE(edged.has_value());
    CHECK(pixelIs(*edged, 0, 0, 255, 0, 0, 128, 1));

    // big, keeping the aspect ratio
    RawPng big{1200, 600, PNG_COLOR_TYPE_RGB, 8};
    auto checker = decodePng(makePng(big, [](int x, int y, int) { return uint16_t((x / 2 + y / 2) % 2 ? 255 : 0); }),
                             300, 300);
    REQUIRE(checker.has_value());
    CHECK_EQ(checker->width, 300);
    CHECK_EQ(checker->height, 150);
    bool grey = true;
    for (int y = 0; y < checker->height; ++y)
        for (int x = 0; x < checker->width; ++x)
            grey = grey && pixelIs(*checker, x, y, 128, 128, 128, 255, 1);
    CHECK(grey);

    // very thin: never down to nothing
    RawPng thin{1000, 3, PNG_COLOR_TYPE_GRAY, 8};
    auto line = decodePng(makePng(thin, [](int, int, int) { return uint16_t(77); }), 100, 100);
    REQUIRE(line.has_value());
    CHECK_EQ(line->width, 100);
    CHECK_EQ(line->height, 1);
    CHECK(pixelIs(*line, 50, 0, 77, 77, 77, 255));

    // within the limits: as it is
    auto same = decodePng(makePng(blocks, [](int, int, int) { return uint16_t(9); }), 4, 100);
    REQUIRE(same.has_value());
    CHECK_EQ(same->width, 4);
}

TEST(png_corrupt)
{
    CHECK(decodePng({}).error().code == Errc::InvalidData);
    CHECK(decodePng(Bytes{'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g', '!'}).error().code == Errc::InvalidData);
    CHECK(!decodePng(headerOnly(0, 10)).has_value());
    CHECK(!decodePng(headerOnly(20000, 10)).has_value());
    CHECK(decodePng(headerOnly(8192, 8193)).error().code == Errc::TooLarge);
    CHECK(!decodePng(headerOnly(10, 10)).has_value());        // not enough image data
    CHECK(!decodePng(headerOnly(10, 10, false)).has_value()); // none at all

    const Bytes good = encodePng(randomImage(40, 30, 3, false)).value();
    // cut anywhere before the end of the image data (a missing IEND alone isn't worth refusing an image for)
    for (size_t length = 0; length + 24 < good.size(); length += 7)
        CHECK(!decodePng(ByteSpan(good).first(length)).has_value());

    // whatever the bytes, nothing crashes or reads out of bounds (the sanitizers watch)
    std::mt19937 rng(5);
    RawPng interlaced{20, 20, PNG_COLOR_TYPE_PALETTE, 4, true};
    interlaced.palette.assign(16, png_color{1, 2, 3});
    const Bytes other = makePng(interlaced, [](int x, int y, int) { return uint16_t((x ^ y) & 15); });
    for (int round = 0; round < 1500; ++round)
    {
        Bytes data = round % 2 ? good : other;
        for (int i = 0, n = 1 + static_cast<int>(rng() % 4); i < n; ++i)
            data[8 + rng() % (data.size() - 8)] = static_cast<uint8_t>(rng());
        auto image = decodePng(data, 16, 16);
        if (image)
            CHECK(image->width <= 16 && image->height <= 16);
    }
}

TEST(resize_and_crop)
{
    Image line(2, 1);
    std::memcpy(line.pixels.data(), "\x00\x00\x00\xFF\xFF\xFF\xFF\xFF", 8);
    const Image wide = resize(line, 4, 1);
    REQUIRE(wide.width == 4);
    CHECK(pixelIs(wide, 0, 0, 0, 0, 0, 255));
    CHECK(pixelIs(wide, 1, 0, 64, 64, 64, 255));
    CHECK(pixelIs(wide, 2, 0, 191, 191, 191, 255));
    CHECK(pixelIs(wide, 3, 0, 255, 255, 255, 255));

    const Image square = randomImage(8, 8, 4, false);
    CHECK(resize(square, 8, 8).pixels == square.pixels);
    CHECK(resize(square, 0, 4).empty());
    const Image mixed = resize(square, 4, 16); // narrower and taller
    CHECK_EQ(mixed.width, 4);
    CHECK_EQ(mixed.height, 16);
    const Image fromNothing = resize(Image(), 3, 3);
    CHECK_EQ(fromNothing.width, 3);

    const Image part = crop(square, 6, -1, 4, 3);
    REQUIRE(part.width == 4 && part.height == 3);
    CHECK(std::memcmp(part.at(0, 1), square.at(6, 0), 8) == 0);
    CHECK(std::memcmp(part.at(0, 2), square.at(6, 1), 8) == 0);
    CHECK(pixelIs(part, 0, 0, 0, 0, 0, 0)); // above the image
    CHECK(pixelIs(part, 2, 1, 0, 0, 0, 0)); // right of it
    CHECK(crop(square, 100, 100, 2, 2).pixels == Bytes(16, 0));
}

TEST(tiling)
{
    CHECK_EQ(tiledIndex(0, 0, 8), 0u);
    CHECK_EQ(tiledIndex(1, 0, 8), 1u);
    CHECK_EQ(tiledIndex(0, 1, 8), 2u);
    CHECK_EQ(tiledIndex(1, 1, 8), 3u);
    CHECK_EQ(tiledIndex(2, 0, 8), 4u);
    CHECK_EQ(tiledIndex(0, 2, 8), 8u);
    CHECK_EQ(tiledIndex(4, 0, 8), 16u);
    CHECK_EQ(tiledIndex(0, 4, 8), 32u);
    CHECK_EQ(tiledIndex(7, 7, 8), 63u);
    CHECK_EQ(tiledIndex(8, 0, 16), 64u);
    CHECK_EQ(tiledIndex(0, 8, 16), 128u);
    CHECK_EQ(tiledIndex(9, 9, 16), 192u + 3u);

    Image image(3, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
        {
            uint8_t* p = image.at(x, y);
            p[0] = static_cast<uint8_t>(0x10 * (x + 1));
            p[1] = static_cast<uint8_t>(0x20 * (y + 1));
            p[2] = 0xF8;
            p[3] = static_cast<uint8_t>(x + y * 3);
        }

    std::vector<uint16_t> rgb565(16 * 8, 0xAAAA);
    tileRgb565(image, rgb565.data(), 16, 8, 14, -1); // only (14..15, 0) land
    CHECK_EQ(rgb565[tiledIndex(14, 0, 16)], toRgb565(0x10, 0x40, 0xF8));
    CHECK_EQ(rgb565[tiledIndex(15, 0, 16)], toRgb565(0x20, 0x40, 0xF8));
    CHECK_EQ(int(std::count(rgb565.begin(), rgb565.end(), uint16_t(0xAAAA))), 16 * 8 - 2);

    std::vector<uint32_t> rgba(8 * 8, 0);
    tileRgba8(image, rgba.data(), 8, 8, 1, 1);
    const uint32_t texel = rgba[tiledIndex(3, 2, 8)]; // image pixel (2, 1)
    CHECK_EQ(texel, 0x3040F805u);
    uint8_t bytes[4];
    std::memcpy(bytes, &texel, 4);
    CHECK(bytes[0] == 5 && bytes[1] == 0xF8 && bytes[2] == 0x40 && bytes[3] == 0x30); // A, B, G, R
    CHECK_EQ(toRgb565(0xFF, 0xFF, 0xFF), uint16_t(0xFFFF));
    CHECK_EQ(toRgb565(0xF8, 0, 0), uint16_t(0xF800));
}

TEST(icon_conversion)
{
    // colours RGB565 holds exactly survive the trip
    Image image(48, 48);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 48; ++x)
        {
            uint8_t* p = image.at(x, y);
            const int r = x % 32;
            const int g = (x + y) % 64;
            const int b = y % 32;
            p[0] = static_cast<uint8_t>(r << 3 | r >> 2);
            p[1] = static_cast<uint8_t>(g << 2 | g >> 4);
            p[2] = static_cast<uint8_t>(b << 3 | b >> 2);
            p[3] = 0xFF;
        }
    std::vector<uint16_t> icon(48 * 48);
    imageToIcon(image, icon.data(), 48);
    CHECK_EQ(icon[tiledIndex(5, 7, 48)], toRgb565(image.at(5, 7)[0], image.at(5, 7)[1], image.at(5, 7)[2]));
    const Image back = iconToImage(icon.data(), 48);
    CHECK(back.pixels == image.pixels);

    // no alpha in icons: transparent goes black; other sizes are scaled to fit
    Image clear(96, 96);
    for (size_t i = 0; i < clear.pixels.size(); i += 4)
        clear.pixels[i] = 0xFF;
    std::vector<uint16_t> small(24 * 24, 0x1234);
    imageToIcon(clear, small.data(), 24);
    CHECK(std::all_of(small.begin(), small.end(), [](uint16_t v) { return v == 0; }));
}
