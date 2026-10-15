// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/image.hpp"

#include <citro2d.h>
#include <memory>
#include <vector>

namespace tz::gfx
{

// A picture laid out the way the GPU reads it, made on a worker thread; upload() turns it into a Picture on the
// main thread with a single copy.
struct TiledPixels
{
    int width = 0;
    int height = 0;
    int textureWidth = 0;
    int textureHeight = 0;
    bool opaque = true; // RGB565 when opaque (half the memory), RGBA8 otherwise
    std::vector<uint8_t> data;
};

// For a picture up to 1024x1024; bigger ones must be scaled down first.
TiledPixels tile(const Image& image, bool opaque);

// A picture in a GPU texture.
class Picture
{
public:
    // nullptr when there's no memory for the texture.
    static std::unique_ptr<Picture> upload(const TiledPixels& pixels);
    ~Picture();
    Picture(const Picture&) = delete;
    Picture& operator=(const Picture&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }

    // Draws the part (sx, sy, sw, sh) of the picture into (dx, dy, dw, dh).
    void draw(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, float alpha = 1.0f) const;
    // The whole picture.
    void draw(float dx, float dy, float dw, float dh, float alpha = 1.0f) const
    {
        draw(0, 0, static_cast<float>(width_), static_cast<float>(height_), dx, dy, dw, dh, alpha);
    }

private:
    Picture() = default;
    C3D_Tex texture_{};
    int width_ = 0;
    int height_ = 0;
};

// Draws part of any texture: the shared code of Picture, the icon atlas and the camera view.
void drawTexturePart(C3D_Tex* texture, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                     float alpha);

} // namespace tz::gfx
