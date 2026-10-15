// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "gfx/icon_atlas.hpp"
#include "scenes/page.hpp"
#include "scenes/previews.hpp"
#include "scenes/qr_view.hpp"
#include "services/themezer.hpp"
#include "sys/jobs.hpp"
#include "sys/net.hpp"
#include "ui/list_view.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tz::scenes
{

// Themezer's catalogue: themes, splash screens and badge sets, searched and browsed a page at a time (the next page
// loads as the list nears its end), with icons and previews downloaded as they're looked at. A last tab scans the QR
// codes on themezer.net.
class ThemezerPage final : public Page
{
public:
    ThemezerPage();
    ~ThemezerPage() override;

    ui::Tab tab() const override;
    gfx::Accent accent() const override;
    void onShow() override;
    void onHide() override;
    void tick(float dt) override;
    void update(ui::Input& input, float dt) override;
    void drawTop() override;
    float topCover() const override;
    void drawBottom(float top, float bottom) override;
    std::vector<ui::Hint> hints() const override;
    std::shared_ptr<gfx::Picture> backdrop() const override;
    std::vector<ui::Action> menuActions() override;

private:
    struct Entry
    {
        themezer::Item item;
        uint64_t key = 0;
        bool iconRequested = false;
        bool onSd = false; // already downloaded
    };

    enum class State : uint8_t
    {
        Idle,
        Loading,
        Ready,
        Failed,
    };

    void reload();
    void fetchPage(int page);
    void requestIcons();
    void checkOnSd(size_t from);
    void search();
    void switchKind(themezer::Kind kind);
    void openActions();
    void download(Entry& entry, bool install);
    void openPreview(Entry& entry);
    void drawDetails(const Entry& entry);
    bool scanningTab() const;
    void downloadScanned(const std::string& text);
    // Where the list starts, below the kinds and the search.
    float listTop(float top) const;
    Entry* selected();

    themezer::Kind kind_ = themezer::Kind::Themes;
    std::string query_;
    State state_ = State::Idle;
    std::string error_;
    std::vector<Entry> entries_;
    int page_ = 0;
    int pages_ = 1;
    bool fetching_ = false;
    ui::ListView list_;
    gfx::IconAtlas atlas_;
    Previews previews_;
    PreviewFade fade_;
    CancelToken token_; // cancelled when the page goes, or when the list is replaced
    // used only on the network lane's one worker, so never by two threads at once
    std::shared_ptr<net::Client> client_ = std::make_shared<net::Client>();
    ui::TabBar kinds_{KEY_LEFT, KEY_RIGHT};
    QrView qr_;
};

} // namespace tz::scenes
