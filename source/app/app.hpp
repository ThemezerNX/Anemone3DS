// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "app/config.hpp"
#include "domain/themes.hpp"
#include "services/updater.hpp"
#include "sys/fs.hpp"
#include "ui/scene.hpp"

#include <memory>
#include <optional>

namespace tz
{

// The HOME Menu's storage for this console's region: its theme extdata and its own (for SaveData.dat).
struct HomeStorage
{
    bool ready = false; // false while it's being opened
    std::unique_ptr<Archive> theme;
    std::unique_ptr<Archive> home;
    // Why themes can't be installed, when they can't.
    std::optional<Error> error;
    // What the HOME Menu had selected when the app started, before anything changed.
    themes::Current atStart;
};

class App
{
public:
    static App& get();

    int run(int argc, char** argv);

    Archive& sd() { return *sd_; }
    Config& config() { return config_; }
    void saveConfig();
    ui::SceneStack& scenes() { return scenes_; }
    int language() const { return language_; }
    const char* appPath() const { return appPath_; }
    // Started from the Homebrew Launcher as a 3dsx, rather than installed.
    bool homebrew() const { return homebrew_; }

    HomeStorage& homeStorage() { return homeStorage_; }
    // The badge extdata, opened (and made, when missing) the first time; nullptr with the error when it can't be.
    Archive* badges(Error* error = nullptr);

    // The HOME Menu's theme has changed. It shows after the HOME Menu reloads, or after a restart when needed.
    void themeChanged(bool needsRestart);
    bool themeChangePending() const { return themeChanged_; }

    // Closes the app, first asking how to show a changed theme.
    void requestExit();

    // A newer release, once the update check found one.
    const std::optional<update::Release>& availableUpdate() const { return update_; }

    // Something was added to a folder on the SD card (a download); the page showing it looks again.
    enum class Folder : uint8_t
    {
        Themes,
        Splashes,
        Badges,
    };
    void folderChanged(Folder folder) { ++folderGenerations_[static_cast<size_t>(folder)]; }
    uint32_t folderGeneration(Folder folder) const { return folderGenerations_[static_cast<size_t>(folder)]; }

    struct Status
    {
        int battery = 5; // 0..5
        bool charging = false;
        int wifi = 0; // 0..3 bars
        bool online = false;
        int hour = 0;
        int minute = 0;
    };
    const Status& status() const { return status_; }

private:
    App() = default;
    bool init(int argc, char** argv);
    void shutdown();
    void openHomeStorage();
    void checkForUpdates();
    void updateStatus();

    std::unique_ptr<Archive> sd_;
    std::unique_ptr<Archive> badgeArchive_;
    std::optional<Error> badgeError_;
    HomeStorage homeStorage_;
    Config config_;
    ui::SceneStack scenes_;
    int language_ = 1;
    const char* appPath_ = nullptr;
    bool homebrew_ = false;
    bool quit_ = false;
    bool themeChanged_ = false;
    bool restartNeeded_ = false;
    enum class AfterExit : uint8_t
    {
        Nothing,
        ReloadHome,
        Restart,
    } afterExit_ = AfterExit::Nothing;
    std::optional<update::Release> update_;
    Status status_;
    u64 nextStatus_ = 0;
    uint32_t folderGenerations_[3] = {};
};

} // namespace tz
