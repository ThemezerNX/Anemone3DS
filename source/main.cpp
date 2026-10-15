// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include <3ds.h>
#include <citro2d.h>

int main()
{
    romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget * top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C2D_TextBuf buffer = C2D_TextBufNew(256);
    C2D_Text title;
    C2D_TextParse(&title, buffer, APP_TITLE " " APP_VERSION);
    C2D_TextOptimize(&title);

    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top, C2D_Color32(18, 18, 18, 255));
        C2D_SceneBegin(top);
        C2D_DrawText(&title, C2D_WithColor | C2D_AlignCenter, 200.0f, 110.0f, 0.0f, 0.7f, 0.7f,
                     C2D_Color32(255, 255, 255, 255));
        C3D_FrameEnd(0);
    }

    C2D_TextBufDelete(buffer);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
