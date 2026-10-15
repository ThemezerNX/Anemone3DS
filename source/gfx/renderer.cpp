// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "gfx/renderer.hpp"

#include "gfx/text.hpp"

#include <3ds.h>
#include <algorithm>
#include <citro2d.h>
#include <vector>

namespace tz::gfx
{

namespace
{

struct Rect
{
    float x, y, w, h;
};

C3D_RenderTarget* topTarget = nullptr;
C3D_RenderTarget* bottomTarget = nullptr;
Screen screen = Screen::Top;
std::vector<Rect> clips;
u64 startTick = 0;
#ifdef TZ_DEVTOOLS
// the framebuffers the frame being drawn goes to, and those the last one went to
u8* drawnTo[2] = {};
u8* shownIn[2] = {};
#endif

void applyClip()
{
    // the GPU scissors in the framebuffer's own orientation: the screens are portrait, turned a quarter
    C2D_Flush();
    if (clips.empty())
    {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        return;
    }
    const Rect& r = clips.back();
    const float width = screenWidth();
    const auto left = static_cast<u32>(std::clamp(240.0f - (r.y + r.h), 0.0f, 240.0f));
    const auto top = static_cast<u32>(std::clamp(width - (r.x + r.w), 0.0f, width));
    const auto right = static_cast<u32>(std::clamp(240.0f - r.y, 0.0f, 240.0f));
    const auto bottom = static_cast<u32>(std::clamp(width - r.x, 0.0f, width));
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left, top, right, bottom);
}

} // namespace

Color mix(Color a, Color b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    Color result = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        result |= static_cast<Color>(x + (y - x) * t + 0.5f) << shift;
    }
    return result;
}

bool init()
{
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE))
        return false;
    if (!C2D_Init(8192))
        return false;
    C2D_Prepare();
    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    startTick = svcGetSystemTick();
    initText();
    return topTarget && bottomTarget;
}

void exit()
{
    exitText();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void beginFrame()
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
#ifdef TZ_DEVTOOLS
    shownIn[0] = drawnTo[0];
    shownIn[1] = drawnTo[1];
#endif
    beginTextFrame();
}

void beginScreen(Screen which, Color clear)
{
    if (!clips.empty())
    {
        clips.clear();
        applyClip();
    }
    screen = which;
    C3D_RenderTarget* target = which == Screen::Top ? topTarget : bottomTarget;
    C2D_TargetClear(target, clear);
    C2D_SceneBegin(target);
}

void endFrame()
{
    if (!clips.empty())
    {
        clips.clear();
        applyClip();
    }
#ifdef TZ_DEVTOOLS
    // C3D_FrameEnd copies the frame into the framebuffers that aren't showing; they show once it's done
    drawnTo[0] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    drawnTo[1] = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
#endif
    C3D_FrameEnd(0);
}

Screen currentScreen()
{
    return screen;
}

float screenWidth()
{
    return screen == Screen::Top ? metric::kTopWidth : metric::kBottomWidth;
}

void pushClip(float x, float y, float width, float height)
{
    Rect r{x, y, width, height};
    if (!clips.empty())
    {
        const Rect& outer = clips.back();
        const float left = std::max(r.x, outer.x);
        const float top = std::max(r.y, outer.y);
        const float right = std::min(r.x + r.w, outer.x + outer.w);
        const float bottom = std::min(r.y + r.h, outer.y + outer.h);
        r = Rect{left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    }
    clips.push_back(r);
    applyClip();
}

void popClip()
{
    if (!clips.empty())
        clips.pop_back();
    applyClip();
}

float time()
{
    return static_cast<float>(svcGetSystemTick() - startTick) / static_cast<float>(SYSCLOCK_ARM11);
}

#ifdef TZ_DEVTOOLS
Image captureScreens()
{
    constexpr int kTop = 400;
    constexpr int kBottom = 320;
    constexpr int kHeight = 240;
    Image image(kTop, kHeight * 2);
    for (size_t i = 0; i < image.pixels.size(); i += 4)
        image.pixels[i + 3] = 0xFF;
    for (int which = 0; which < 2; ++which)
    {
        const gfxScreen_t screen = which == 0 ? GFX_TOP : GFX_BOTTOM;
        const u8* framebuffer = shownIn[which];
        if (!framebuffer || gfxGetScreenFormat(screen) != GSP_BGR8_OES)
            continue;
        const int width = which == 0 ? kTop : kBottom;
        const int height = kHeight;
        const int left = (kTop - width) / 2;
        const int top = which * height;
        GSPGPU_InvalidateDataCache(framebuffer, static_cast<u32>(width * height * 3));
        // the screens are portrait panels turned a quarter: columns from the bottom of the picture, in BGR
        for (int x = 0; x < width; ++x)
        {
            const u8* column = framebuffer + static_cast<size_t>(x) * height * 3;
            for (int y = 0; y < height; ++y)
            {
                const u8* pixel = column + static_cast<size_t>(height - 1 - y) * 3;
                uint8_t* out = image.at(left + x, top + y);
                out[0] = pixel[2];
                out[1] = pixel[1];
                out[2] = pixel[0];
            }
        }
    }
    return image;
}
#endif

} // namespace tz::gfx
