// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <citro2d.h>
#include <cstdint>
#include <vector>

namespace tz::gfx
{

// The 48x48 icons of the items in a list, kept in one texture: a hundred at a time, the least recently drawn
// making room for new ones. Icons are SMDH-style tiled RGB565, which goes into the texture as it is.
class IconAtlas
{
public:
    static constexpr int kIconSize = 48;

    IconAtlas();
    ~IconAtlas();
    IconAtlas(const IconAtlas&) = delete;
    IconAtlas& operator=(const IconAtlas&) = delete;

    // Whether the icon for this key is in the atlas.
    bool has(uint64_t key) const;
    // Puts an icon in (or replaces the one for the key).
    void put(uint64_t key, const uint16_t* tiledIcon);
    // Draws the icon for the key at (x, y) in a square of size; false when it isn't in the atlas.
    bool draw(uint64_t key, float x, float y, float size, float alpha = 1.0f);
    void clear();

private:
    struct Slot
    {
        uint64_t key = 0;
        bool used = false;
        uint32_t lastUse = 0;
    };
    int find(uint64_t key) const;

    C3D_Tex texture_{};
    bool ready_ = false;
    std::vector<Slot> slots_;
    uint32_t clock_ = 0;
};

} // namespace tz::gfx
