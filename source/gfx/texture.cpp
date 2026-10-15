// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "gfx/texture.hpp"

#include <algorithm>
#include <cstring>

namespace tz::gfx
{

namespace
{
int powerOfTwo(int value)
{
    int size = 8;
    while (size < value && size < 1024)
        size *= 2;
    return size;
}
} // namespace

TiledPixels tile(const Image& image, bool opaque)
{
    TiledPixels pixels;
    pixels.width = std::min(image.width, 1024);
    pixels.height = std::min(image.height, 1024);
    pixels.textureWidth = powerOfTwo(pixels.width);
    pixels.textureHeight = powerOfTwo(pixels.height);
    pixels.opaque = opaque;
    const size_t texels = static_cast<size_t>(pixels.textureWidth) * pixels.textureHeight;
    if (opaque)
    {
        pixels.data.assign(texels * 2, 0);
        tileRgb565(image, reinterpret_cast<uint16_t*>(pixels.data.data()), pixels.textureWidth, pixels.textureHeight);
    }
    else
    {
        pixels.data.assign(texels * 4, 0);
        tileRgba8(image, reinterpret_cast<uint32_t*>(pixels.data.data()), pixels.textureWidth, pixels.textureHeight);
    }
    return pixels;
}

std::unique_ptr<Picture> Picture::upload(const TiledPixels& pixels)
{
    if (pixels.width <= 0 || pixels.height <= 0 || pixels.data.empty())
        return nullptr;
    std::unique_ptr<Picture> picture(new Picture());
    const GPU_TEXCOLOR format = pixels.opaque ? GPU_RGB565 : GPU_RGBA8;
    if (!C3D_TexInit(&picture->texture_, pixels.textureWidth, pixels.textureHeight, format))
        return nullptr;
    std::memcpy(picture->texture_.data, pixels.data.data(), std::min<size_t>(picture->texture_.size, pixels.data.size()));
    C3D_TexFlush(&picture->texture_);
    C3D_TexSetFilter(&picture->texture_, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&picture->texture_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    picture->width_ = pixels.width;
    picture->height_ = pixels.height;
    return picture;
}

Picture::~Picture()
{
    C3D_TexDelete(&texture_);
}

void Picture::draw(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh, float alpha) const
{
    drawTexturePart(const_cast<C3D_Tex*>(&texture_), sx, sy, sw, sh, dx, dy, dw, dh, alpha);
}

void drawTexturePart(C3D_Tex* texture, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                     float alpha)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || alpha <= 0)
        return;
    const float tw = texture->width;
    const float th = texture->height;
    // the texture's rows start at its top, where v is 1
    const Tex3DS_SubTexture sub{
        static_cast<u16>(sw), static_cast<u16>(sh), sx / tw, 1.0f - sy / th, (sx + sw) / tw, 1.0f - (sy + sh) / th,
    };
    const C2D_Image image{texture, &sub};
    C2D_DrawParams params{};
    params.pos = {dx, dy, dw, dh};
    params.depth = 0;
    if (alpha < 1.0f)
    {
        C2D_ImageTint tint;
        C2D_AlphaImageTint(&tint, alpha);
        C2D_DrawImage(image, &params, &tint);
    }
    else
    {
        C2D_DrawImage(image, &params, nullptr);
    }
}

} // namespace tz::gfx
