// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "domain/library.hpp"
#include "gfx/icon_atlas.hpp"
#include "scenes/page.hpp"
#include "scenes/previews.hpp"
#include "sys/jobs.hpp"
#include "sys/thread.hpp"
#include "ui/list_view.hpp"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace tz::scenes
{

// The themes or the splashes on the SD card. The list shows at once from the cache of the last start, names and
// icons fill in as they're read (the visible ones first), and the selected one's preview fills the top screen.
class LibraryPage final : public Page
{
public:
    explicit LibraryPage(library::Kind kind);
    ~LibraryPage() override;

    ui::Tab tab() const override;
    gfx::Accent accent() const override;
    void onShow() override;
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
        library::Item item;
        uint64_t key = 0;
        bool installed = false;      // themes: the theme, or in the shuffle; splashes: both screens
        bool topInstalled = false;   // splashes: just one of the screens
        bool bottomInstalled = false;
        bool crcPending = false;
    };

    // Items whose info is still to be read, shared with the worker reading them.
    struct InfoQueue
    {
        Mutex mutex;
        std::deque<library::Source> pending;
        bool running = false;
    };

    enum class State : uint8_t
    {
        NotLoaded,
        Loading,
        Ready,
        Failed,
    };

    void load();
    void onScanned(std::vector<library::Source> sources, std::vector<std::pair<library::Source, library::Info>> cached);
    void onInfo(const library::Source& source, Expected<library::Info> info);
    void prioritiseVisible();
    void startInfoReader();
    void saveCacheLater();
    void refreshInstalled();
    void applyInstalled(Entry& entry);
    void focusInstalled();
    void rebuildIndex();
    void resort(bool keepSelection);

    Entry* selectedEntry();
    Entry* find(uint64_t key);
    void openActions();
    void install(Entry& entry, int mode);
    void installSplash(Entry& entry, int screens);
    void installShuffle();
    void toggleShuffle(Entry& entry);
    void confirmDelete(Entry& entry);
    void openPreview(Entry& entry);
    void resetTheme();
    void removeSplash();
    void saveInstalled();

    // "In use" and the like, for what the HOME Menu (or Luma) has now; nullptr when it isn't.
    const char* statusOf(const Entry& entry) const;
    void drawRow(int index, float x, float y, float w, float h, bool selected);
    void drawEmpty(float top, float bottom);
    void drawDetails(const Entry& entry);

    library::Kind kind_;
    State state_ = State::NotLoaded;
    std::string error_;
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, size_t> index_;
    ui::ListView list_;
    gfx::IconAtlas atlas_;
    Previews previews_;
    PreviewFade fade_;
    std::shared_ptr<InfoQueue> infoQueue_ = std::make_shared<InfoQueue>();
    CancelToken token_; // everything this page started; cancelled when it goes
    std::pair<int, int> visible_{0, -1};
    bool interacted_ = false;
    bool installedFound_ = false;   // an item turned out to be in use: the list may open on it
    bool focusedInstalled_ = false;
    float cacheSaveIn_ = -1;
    uint32_t generation_ = 0; // of the folder, when it was read

    // what the HOME Menu (or Luma) has installed now
    std::vector<library::FileId> installedBodies_;
    bool installedShuffle_ = false;
    std::optional<library::FileId> installedTop_;
    std::optional<library::FileId> installedBottom_;

    // themes picked for a shuffle, in order, and whether each keeps its music
    struct ShufflePick
    {
        uint64_t key;
        bool music;
    };
    std::vector<ShufflePick> shuffle_;
};

} // namespace tz::scenes
