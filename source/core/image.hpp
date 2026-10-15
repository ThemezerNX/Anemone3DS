// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <vector>

namespace tz
{

// 8-bit RGBA pixels, rows from the top, each pixel the bytes R, G, B, A.
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;

    Image() = default;
    Image(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * h * 4, 0) {}

    bool empty() const { return width <= 0 || height <= 0; }
    uint8_t* at(int x, int y) { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
    const uint8_t* at(int x, int y) const { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
};

// Decodes a PNG of any kind (palette, grey, 16-bit, interlaced...). An image larger than maxWidth x maxHeight is
// scaled down to fit, keeping its aspect ratio. InvalidData for anything that isn't a valid PNG; never crashes on
// corrupt files.
Expected<Image> decodePng(ByteSpan png, int maxWidth = 1024, int maxHeight = 1024);
Expected<Bytes> encodePng(const Image& image);

// Area-averaging downscale, bilinear upscale.
Image resize(const Image& image, int width, int height);
// The part of an image; parts outside it are transparent.
Image crop(const Image& image, int x, int y, int width, int height);

inline uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// The 3DS GPU stores textures in 8x8 tiles, each in Morton (Z) order, tiles in rows from the top of the texture.
// The index of pixel (x, y) in a texture textureWidth pixels wide.
inline uint32_t tiledIndex(uint32_t x, uint32_t y, uint32_t textureWidth)
{
    const uint32_t morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y >> 3) * (textureWidth >> 3) + (x >> 3)) << 6) + morton;
}

// Copies an image into a tiled texture buffer at (x, y); parts outside the texture are skipped.
// RGB565 drops alpha. RGBA8 is the GPU's byte order (A, B, G, R in memory).
void tileRgb565(const Image& image, uint16_t* texture, uint32_t textureWidth, uint32_t textureHeight, int x = 0,
                int y = 0);
void tileRgba8(const Image& image, uint32_t* texture, uint32_t textureWidth, uint32_t textureHeight, int x = 0,
               int y = 0);

// SMDH icons are tiled RGB565 squares (24 or 48 pixels); these convert them to and from images.
Image iconToImage(const uint16_t* tiled, int size);
void imageToIcon(const Image& image, uint16_t* tiled, int size);

} // namespace tz
