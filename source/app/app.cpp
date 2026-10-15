// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "app/app.hpp"

#include "app/autopilot.hpp"

#include "app/strings.hpp"
#include "domain/badges.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "scenes/home.hpp"
#include "sys/audio.hpp"
#include "sys/jobs.hpp"
#include "sys/log.hpp"
#include "sys/net.hpp"
#include "ui/overlays.hpp"

#include <3ds.h>
#include <algorithm>
#include <ctime>

namespace tz
{

namespace
{

struct RegionIds
{
    u32 theme;
    u32 home;
};

// The theme extdata and the HOME Menu's own, by region. Australia uses Europe's HOME Menu.
std::optional<RegionIds> regionIds(u8 region)
{
    switch (region)
    {
    case CFG_REGION_JPN:
        return RegionIds{0x2CC, 0x82};
    case CFG_REGION_USA:
        return RegionIds{0x2CD, 0x8F};
    case CFG_REGION_EUR:
    case CFG_REGION_AUS:
        return RegionIds{0x2CE, 0x98};
    case CFG_REGION_KOR:
        return RegionIds{0x2CF, 0xA9};
    default:
        return std::nullopt; // the Chinese and Taiwanese HOME Menus have no themes
    }
}

// HOME Menu notification: the POWER button was pressed. The HOME Menu opens its power menu, and cancelling it
// reloads the theme.
constexpr u32 kPowerButtonNotification = 0x202;

} // namespace

App& App::get()
{
    static App app;
    return app;
}

bool App::init(int argc, char** argv)
{
    log::init();
    appPath_ = argc > 0 ? argv[0] : nullptr;
    homebrew_ = envIsHomebrew();

    romfsInit();
    cfguInit();
    ptmuInit();
    acInit();
    osSetSpeedupEnable(true);
    // the system core runs the music and the camera
    APT_SetAppCpuTimeLimit(30);

    u8 language = CFG_LANGUAGE_EN;
    CFGU_GetSystemLanguage(&language);
    language_ = language;
    loadTranslations(language_);

    if (!gfx::init())
        return false;
    gfx::loadIcons();

    auto sd = Archive::openSdmc();
    if (!sd)
    {
        log::writeError("SD card", sd.error());
        return false;
    }
    sd_ = std::move(*sd);
    sd_->createDirectories(kAppFolder);
    config_ = Config::load(*sd_);

    jobs::init();
    if (audio::init() && config_.backgroundMusic)
        audio::startBackgroundMusic("romfs:/bgm.bcstm");

    openHomeStorage();
    checkForUpdates();
    LOG_INFO("%s %s started", APP_TITLE, APP_VERSION);
    return true;
}

void App::openHomeStorage()
{
    jobs::async(
        Lane::Io,
        [](const CancelToken&) {
            auto storage = std::make_shared<HomeStorage>();
            u8 region = 0;
            CFGU_SecureInfoGetRegion(&region);
            const auto ids = regionIds(region);
            if (!ids)
            {
                storage->error = Error{Errc::NotAllowed, 0, "region without themes"};
                return storage;
            }

            auto home = Archive::openExtdata(ids->home);
            if (!home)
            {
                storage->error = home.error();
                return storage;
            }
            storage->home = std::move(*home);

            auto theme = Archive::openExtdata(ids->theme);
            if (!theme && theme.error().code == Errc::NotFound && region != CFG_REGION_KOR)
            {
                // A console that has never had a theme selected has no theme storage yet. It's made the way the
                // HOME Menu would, unless the HOME Menu points at a cached theme (which would then be lost).
                auto save = storage->home->readFile("/SaveData.dat", 0x10000);
                auto saveData = save ? home::SaveData::parse(std::move(*save)) : fail(save.error());
                if (saveData && saveData->selected().type < home::type::kShop && !saveData->shuffle())
                {
                    const Result created = createExtdata(ids->theme, 1000, 1000);
                    LOG_INFO("created theme storage: %08lX", static_cast<unsigned long>(created));
                    theme = Archive::openExtdata(ids->theme);
                }
            }
            if (!theme)
            {
                storage->error = theme.error();
                return storage;
            }
            storage->theme = std::move(*theme);

            if (auto ensured = themes::ensureFiles(*storage->theme, *storage->home); !ensured)
            {
                storage->error = ensured.error();
                return storage;
            }
            if (auto current = themes::readCurrent(*storage->theme, *storage->home))
                storage->atStart = *current;
            return storage;
        },
        [this](std::shared_ptr<HomeStorage> storage) {
            homeStorage_ = std::move(*storage);
            homeStorage_.ready = true;
            if (homeStorage_.error)
                log::writeError("theme storage", *homeStorage_.error);
        });
}

Archive* App::badges(Error* error)
{
    if (!badgeArchive_ && !badgeError_)
    {
        auto archive = Archive::openExtdata(badges::kExtdataId);
        if (!archive && archive.error().code == Errc::NotFound)
        {
            // the HOME Menu makes it the first time badges are used; until then the app makes it
            createExtdata(badges::kExtdataId, 100, 100);
            archive = Archive::openExtdata(badges::kExtdataId);
        }
        if (archive)
        {
            if (auto ensured = badges::ensureFiles(**archive); ensured)
                badgeArchive_ = std::move(*archive);
            else
                badgeError_ = ensured.error();
        }
        else
        {
            badgeError_ = archive.error();
        }
        if (badgeError_)
            log::writeError("badge storage", *badgeError_);
    }
    if (error && badgeError_)
        *error = *badgeError_;
    return badgeArchive_.get();
}

void App::checkForUpdates()
{
    if (!config_.checkForUpdates)
        return;
    jobs::async(
        Lane::Network,
        [](const CancelToken& token) {
            net::Client client;
            return update::check(client, token);
        },
        [this](Expected<std::optional<update::Release>> result) {
            if (result && *result)
            {
                update_ = **result;
                ui::toast(trf("Version {} is available: install it from the menu (START)", update_->version),
                          ui::ToastKind::Info);
            }
        });
}

void App::saveConfig()
{
    if (auto saved = config_.save(*sd_); !saved)
        log::writeError("settings", saved.error());
}

void App::themeChanged(bool needsRestart)
{
    themeChanged_ = true;
    restartNeeded_ = restartNeeded_ || needsRestart;
}

void App::requestExit()
{
    if (!themeChanged_)
    {
        quit_ = true;
        return;
    }

    // A 3dsx returns to the Homebrew Launcher, not to the HOME Menu, which then can't reload the theme; nor can it
    // when it's on a basic colour or a shuffle (see themes::Current::restartToShow).
    const bool restart = restartNeeded_ || homebrew_ || homeStorage_.atStart.restartToShow();
    if (restart)
    {
        scenes_.emplace<ui::Dialog>(
            gfx::Icon::Refresh, gfx::color::kPrimaryLight, tr("Restart to see your theme?"),
            tr("The HOME Menu shows your new theme once the console restarts. You can also restart later."),
            std::vector<ui::DialogButton>{
                {tr("Later"),
                 [this] {
                     afterExit_ = AfterExit::Nothing;
                     quit_ = true;
                 }},
                {tr("Restart"),
                 [this] {
                     afterExit_ = AfterExit::Restart;
                     quit_ = true;
                 }},
            });
    }
    else
    {
        scenes_.emplace<ui::Dialog>(
            gfx::Icon::Theme, gfx::color::kPrimaryLight, tr("Your new theme is ready"),
            tr("When you close the app, the HOME Menu offers to power off: choose Cancel, and it shows your theme."),
            std::vector<ui::DialogButton>{
                {tr("Restart instead"),
                 [this] {
                     afterExit_ = AfterExit::Restart;
                     quit_ = true;
                 }},
                {tr("Close"),
                 [this] {
                     afterExit_ = AfterExit::ReloadHome;
                     quit_ = true;
                 }},
            });
    }
}

void App::updateStatus()
{
    const u64 now = osGetTime();
    if (now < nextStatus_)
        return;
    nextStatus_ = now + 1000;

    u8 level = 5;
    if (R_SUCCEEDED(PTMU_GetBatteryLevel(&level)))
        status_.battery = std::min<int>(level, 5);
    u8 charging = 0;
    if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging)))
        status_.charging = charging != 0;
    u32 wifi = 0;
    status_.online = R_SUCCEEDED(ACU_GetWifiStatus(&wifi)) && wifi != 0;
    status_.wifi = status_.online ? osGetWifiStrength() : 0;
    const time_t seconds = time(nullptr);
    if (const tm* local = gmtime(&seconds))
    {
        status_.hour = local->tm_hour;
        status_.minute = local->tm_min;
    }
}

void App::shutdown()
{
    saveConfig();
    scenes_ = ui::SceneStack();
    audio::stopBackgroundMusic();
    jobs::shutdown();
    audio::exit();
    net::exit();
    badgeArchive_.reset();
    homeStorage_ = HomeStorage();
    sd_.reset();
    gfx::freeIcons();
    gfx::exit();
    acExit();
    ptmuExit();
    cfguExit();
    romfsExit();
}

int App::run(int argc, char** argv)
{
    if (!init(argc, argv))
    {
        // nothing can be shown without the screens or the SD card; say so on the console's own error screen
        errorConf error;
        errorInit(&error, ERROR_TEXT, CFG_LANGUAGE_EN);
        errorText(&error, "Themezer couldn't start: the SD card can't be read.");
        errorDisp(&error);
        shutdown();
        return 1;
    }

    scenes_.push(scenes::makeHome());
    autopilot::start();
    ui::InputReader reader;
    u64 last = osGetTime();
    while (aptMainLoop() && !quit_)
    {
        const u64 now = osGetTime();
        const float dt = std::clamp(static_cast<float>(now - last) / 1000.0f, 0.0f, 0.1f);
        last = now;

        ui::RawInput raw = ui::readHardware();
        autopilot::drive(raw);
        ui::Input input = reader.read(raw, dt);
        jobs::pump(6);
        updateStatus();
        ui::updateToasts(dt);
        scenes_.update(input, dt);
        if (scenes_.empty() || autopilot::quitting())
            break;

        gfx::beginFrame();
        autopilot::frameStarted();
        gfx::beginScreen(gfx::Screen::Top);
        scenes_.drawTop();
        gfx::beginScreen(gfx::Screen::Bottom);
        scenes_.drawBottom();
        ui::drawToasts();
        gfx::endFrame();
    }

    const AfterExit after = afterExit_;
    shutdown();
    if (after == AfterExit::Restart)
        APT_HardwareResetAsync();
    else if (after == AfterExit::ReloadHome)
        srvPublishToSubscriber(kPowerButtonNotification, 0);
    return 0;
}

} // namespace tz
