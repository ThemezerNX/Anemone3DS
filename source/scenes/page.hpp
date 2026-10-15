// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "gfx/texture.hpp"
#include "ui/input.hpp"
#include "ui/overlays.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <vector>

namespace tz::scenes
{

// One tab of the home scene: themes, splashes, badges, or Themezer. The home scene draws the frame around it
// (backdrop, status bar, tabs, hints); the page draws its content and handles its keys.
class Page
{
public:
    virtual ~Page() = default;

    virtual ui::Tab tab() const = 0;
    virtual gfx::Accent accent() const = 0;

    // When its tab is shown, the first time it starts loading; and when it's hidden again.
    virtual void onShow() {}
    virtual void onHide() {}
    // When an overlay over the home scene closes.
    virtual void onResume() {}
    // Every frame, also while hidden.
    virtual void tick(float /*dt*/) {}

    virtual void update(ui::Input& input, float dt) = 0;
    // The top screen, with the status bar drawn over it as much as topCover leaves it.
    virtual void drawTop() = 0;
    // How much a preview fills the top screen, from 0 to 1: the status bar fades out as it fades in.
    virtual float topCover() const { return 0; }
    // The bottom screen between the tabs and the hints.
    virtual void drawBottom(float top, float bottom) = 0;
    virtual std::vector<ui::Hint> hints() const = 0;

    // The blurred picture behind the screens, for what's selected.
    virtual std::shared_ptr<gfx::Picture> backdrop() const { return nullptr; }

    // What the menu (START) offers for this page.
    virtual std::vector<ui::Action> menuActions() { return {}; }
};

} // namespace tz::scenes
