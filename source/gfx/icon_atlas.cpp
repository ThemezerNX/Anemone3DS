// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "gfx/icon_atlas.hpp"

#include "gfx/texture.hpp"

#include <array>
#include <cmath>
#include <cstring>

namespace tz::gfx
{

namespace
{
constexpr int kTextureSize = 512;
constexpr int kPerRow = kTextureSize / IconAtlas::kIconSize; // 10
constexpr int kTilesPerIconRow = IconAtlas::kIconSize / 8;   // 6
constexpr int kPixels = IconAtlas::kIconSize * IconAtlas::kIconSize;
// the corners' radius, as the placeholders have it: 6 pixels on a row's 36-pixel icon
constexpr float kCornerRadius = 8;

// Which pixels of an icon, in the order of its 8x8 tiles, are inside its rounded corners. RGBA5551 has one bit of
// alpha; the GPU's filtering softens the edge where the icon is drawn smaller.
std::array<bool, kPixels> roundedMask()
{
    std::array<bool, kPixels> inside{};
    constexpr int size = IconAtlas::kIconSize;
    for (int i = 0; i < kPixels; ++i)
    {
        const int tile = i / 64;
        const int morton = i % 64;
        const int x = (tile % kTilesPerIconRow) * 8 + ((morton & 1) | ((morton >> 1) & 2) | ((morton >> 2) & 4));
        const int y = (tile / kTilesPerIconRow) * 8 + (((morton >> 1) & 1) | ((morton >> 2) & 2) | ((morton >> 3) & 4));
        // the distance from the nearest corner's centre, for the pixels in a corner square
        const float cx = x < kCornerRadius ? kCornerRadius : (x >= size - kCornerRadius ? size - kCornerRadius : -1);
        const float cy = y < kCornerRadius ? kCornerRadius : (y >= size - kCornerRadius ? size - kCornerRadius : -1);
        if (cx < 0 || cy < 0)
        {
            inside[i] = true;
            continue;
        }
        const float dx = x + 0.5f - cx;
        const float dy = y + 0.5f - cy;
        inside[i] = dx * dx + dy * dy <= kCornerRadius * kCornerRadius;
    }
    return inside;
}

// RGB565 to RGBA5551, opaque or not
uint16_t withAlphaBit(uint16_t rgb565, bool opaque)
{
    const uint16_t red = rgb565 >> 11;
    const uint16_t green = (rgb565 >> 6) & 0x1F;
    const uint16_t blue = rgb565 & 0x1F;
    return static_cast<uint16_t>((red << 11) | (green << 6) | (blue << 1) | (opaque ? 1 : 0));
}
} // namespace

IconAtlas::IconAtlas()
{
    ready_ = C3D_TexInit(&texture_, kTextureSize, kTextureSize, GPU_RGBA5551);
    if (ready_)
    {
        C3D_TexSetFilter(&texture_, GPU_LINEAR, GPU_LINEAR);
        std::memset(texture_.data, 0, texture_.size);
    }
    slots_.resize(kPerRow * kPerRow);
}

IconAtlas::~IconAtlas()
{
    if (ready_)
        C3D_TexDelete(&texture_);
}

int IconAtlas::find(uint64_t key) const
{
    for (size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].used && slots_[i].key == key)
            return static_cast<int>(i);
    return -1;
}

bool IconAtlas::has(uint64_t key) const
{
    return find(key) >= 0;
}

void IconAtlas::put(uint64_t key, const uint16_t* tiledIcon)
{
    if (!ready_)
        return;
    int index = find(key);
    if (index < 0)
    {
        // a free slot, or the one drawn longest ago
        index = 0;
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            if (!slots_[i].used)
            {
                index = static_cast<int>(i);
                break;
            }
            if (slots_[i].lastUse < slots_[index].lastUse)
                index = static_cast<int>(i);
        }
    }
    Slot& slot = slots_[index];
    slot = Slot{key, true, ++clock_};

    // An SMDH icon is six rows of six 8x8 tiles; in the texture each row of tiles is contiguous too, so a row of tiles
    // is 6 * 64 pixels in a row. The corners are cut round on the way.
    static const std::array<bool, kPixels> mask = roundedMask();
    const int column = index % kPerRow;
    const int row = index / kPerRow;
    auto* destination = static_cast<uint16_t*>(texture_.data);
    constexpr int kRowPixels = kTilesPerIconRow * 64;
    for (int tileRow = 0; tileRow < kTilesPerIconRow; ++tileRow)
    {
        const int textureTileRow = row * kTilesPerIconRow + tileRow;
        const int textureTileColumn = column * kTilesPerIconRow;
        uint16_t* to = destination + (textureTileRow * (kTextureSize / 8) + textureTileColumn) * 64;
        const int from = tileRow * kRowPixels;
        for (int i = 0; i < kRowPixels; ++i)
            to[i] = withAlphaBit(tiledIcon[from + i], mask[static_cast<size_t>(from + i)]);
        // only the part written goes to the GPU's view of memory
        GSPGPU_FlushDataCache(to, kRowPixels * sizeof(uint16_t));
    }
}

bool IconAtlas::draw(uint64_t key, float x, float y, float size, float alpha)
{
    const int index = find(key);
    if (index < 0)
        return false;
    slots_[index].lastUse = ++clock_;
    const float sx = static_cast<float>((index % kPerRow) * kIconSize);
    const float sy = static_cast<float>((index / kPerRow) * kIconSize);
    drawTexturePart(&texture_, sx, sy, kIconSize, kIconSize, std::round(x), std::round(y), size, size, alpha);
    return true;
}

void IconAtlas::clear()
{
    for (Slot& slot : slots_)
        slot.used = false;
}

} // namespace tz::gfx
