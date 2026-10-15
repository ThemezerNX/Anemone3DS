// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/home.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "scenes/badges_page.hpp"
#include "scenes/frame.hpp"
#include "scenes/library_page.hpp"
#include "scenes/menu.hpp"
#include "scenes/themezer_page.hpp"

#include <algorithm>

namespace tz::scenes
{

namespace
{

class HomeScene final : public ui::Scene
{
public:
    HomeScene()
    {
        pages_.push_back(std::make_unique<LibraryPage>(library::Kind::Theme));
        pages_.push_back(std::make_unique<LibraryPage>(library::Kind::Splash));
        pages_.push_back(std::make_unique<BadgesPage>());
        pages_.push_back(std::make_unique<ThemezerPage>());

        std::vector<ui::Tab> tabs;
        for (const auto& page : pages_)
            tabs.push_back(page->tab());
        tabs_.setTabs(std::move(tabs));

        current_ = std::clamp(App::get().config().lastTab, 0, static_cast<int>(pages_.size()) - 1);
        tabs_.select(current_);
        pages_[current_]->onShow();
    }

    void tick(float dt) override
    {
        for (auto& page : pages_)
            page->tick(dt);
        backdrop_.set(page().backdrop(), page().accent().solid);
        backdrop_.tick(dt);
    }

    void update(ui::Input& input, float dt) override
    {
        hints_ = page().hints();
        hints_.push_back({KEY_START, tr("Menu")});
        hintBar_.update(input, hints_);

        if (tabs_.update(input))
            switchTo(tabs_.selected());

        if (input.pressed(KEY_START))
        {
            openMenu(page().menuActions());
            return;
        }
        page().update(input, dt);
    }

    void onResume() override { page().onResume(); }

    void drawTop() override
    {
        backdrop_.draw(0.35f);
        page().drawTop();
        if (const float cover = page().topCover(); cover < 1)
            drawStatusBar(1 - cover);
    }

    void drawBottom() override
    {
        using namespace gfx;
        backdrop_.draw(0.72f);
        tabs_.draw(0, 0, metric::kBottomWidth, metric::kTabBarHeight, metric::kTextNormal);
        page().drawBottom(metric::kTabBarHeight, metric::kHeight - metric::kHintBarHeight);
        hintBar_.draw(hints_);
    }

private:
    Page& page() { return *pages_[current_]; }

    void switchTo(int index)
    {
        if (index == current_)
            return;
        page().onHide();
        current_ = index;
        page().onShow();
        App::get().config().lastTab = index;
    }

    std::vector<std::unique_ptr<Page>> pages_;
    int current_ = 0;
    ui::TabBar tabs_;
    ui::HintBar hintBar_;
    std::vector<ui::Hint> hints_;
    Backdrop backdrop_;
};

} // namespace

std::unique_ptr<ui::Scene> makeHome()
{
    return std::make_unique<HomeScene>();
}

} // namespace tz::scenes
