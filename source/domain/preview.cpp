// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "domain/preview.hpp"

#include <algorithm>

namespace tz::preview
{

namespace
{
constexpr int kTopWidth = 400;
constexpr int kBottomWidth = 320;
constexpr int kHeight = 240;

// Fits a picture into a screen-sized canvas, centred on black.
Image fit(const Image& picture, int width, int height)
{
    const float scale = std::min(static_cast<float>(width) / picture.width, static_cast<float>(height) / picture.height);
    const int w = std::max(1, static_cast<int>(picture.width * scale));
    const int h = std::max(1, static_cast<int>(picture.height * scale));
    const Image scaled = resize(picture, w, h);
    Image canvas(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            canvas.at(x, y)[3] = 255;
    const int ox = (width - w) / 2;
    const int oy = (height - h) / 2;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* from = scaled.at(x, y);
            uint8_t* to = canvas.at(ox + x, oy + y);
            const int a = from[3];
            for (int c = 0; c < 3; ++c)
                to[c] = static_cast<uint8_t>(from[c] * a / 255);
        }
    }
    return canvas;
}
} // namespace

Screens split(const Image& picture)
{
    Screens screens;
    const int w = picture.width;
    const int h = picture.height;
    if (w >= kTopWidth && w <= 432 && h >= kHeight * 2 && h <= 528)
    {
        const int border = (w == 432 && h == 528) ? 16 : 0;
        screens.top = crop(picture, (w - kTopWidth) / 2, border, kTopWidth, kHeight);
        screens.bottom = crop(picture, (w - kBottomWidth) / 2, kHeight + border * 2, kBottomWidth, kHeight);
        return screens;
    }
    if (w == kTopWidth && h == kHeight)
    {
        screens.top = picture;
        return screens;
    }
    screens.top = fit(picture, kTopWidth, kHeight);
    return screens;
}

Image blurSource(const Image& top)
{
    // a small picture with its neighbours averaged: stretched by the GPU, it becomes a smooth blur
    Image small = resize(top, 40, 24);
    Image blurred(small.width, small.height);
    for (int y = 0; y < small.height; ++y)
    {
        for (int x = 0; x < small.width; ++x)
        {
            int sum[3] = {0, 0, 0};
            int count = 0;
            for (int dy = -2; dy <= 2; ++dy)
            {
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int sx = std::clamp(x + dx, 0, small.width - 1);
                    const int sy = std::clamp(y + dy, 0, small.height - 1);
                    const uint8_t* p = small.at(sx, sy);
                    for (int c = 0; c < 3; ++c)
                        sum[c] += p[c];
                    ++count;
                }
            }
            uint8_t* out = blurred.at(x, y);
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>(sum[c] / count);
            out[3] = 255;
        }
    }
    return blurred;
}

} // namespace tz::preview
