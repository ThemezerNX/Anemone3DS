// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "domain/badges.hpp"
#include "gfx/icon_atlas.hpp"
#include "scenes/page.hpp"
#include "scenes/previews.hpp"
#include "sys/jobs.hpp"
#include "ui/list_view.hpp"

#include <vector>

namespace tz::scenes
{

// The badge sets in /Badges. They are installed together, replacing the HOME Menu's badges; X leaves a set out.
class BadgesPage final : public Page
{
public:
    BadgesPage();
    ~BadgesPage() override;

    ui::Tab tab() const override;
    gfx::Accent accent() const override { return gfx::accent::kBadges; }
    void onShow() override;
    void tick(float dt) override;
    void update(ui::Input& input, float dt) override;
    void drawTop() override;
    void drawBottom(float top, float bottom) override;
    std::vector<ui::Hint> hints() const override;
    std::shared_ptr<gfx::Picture> backdrop() const override;
    std::vector<ui::Action> menuActions() override;

private:
    struct Set
    {
        badges::SetSource source;
        uint64_t key = 0;
        bool included = true;
        bool summarized = false;
        badges::SetSummary summary;
    };

    enum class State : uint8_t
    {
        NotLoaded,
        Loading,
        Ready,
        Failed,
    };

    void load();
    void readInstalledCount();
    void openActions();
    void install();
    void uninstall();
    void exportBadges();
    void restore();
    void confirmDelete(Set& set);
    Set* selected();
    int includedCount() const;
    // How many pictures, and who made it.
    std::string detailOf(const Set& set) const;

    State state_ = State::NotLoaded;
    std::string error_;
    std::vector<Set> sets_;
    ui::ListView list_;
    gfx::IconAtlas atlas_;
    Previews previews_;
    CancelToken token_;
    int installedCount_ = -1; // on the HOME Menu; -1 until known
    bool storageBusy_ = false; // the HOME Menu has the badges open (the app runs as an applet)
    uint32_t generation_ = 0;
};

} // namespace tz::scenes
