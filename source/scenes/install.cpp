// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/install.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/bcstm.hpp"
#include "core/path.hpp"
#include "core/zip.hpp"
#include "domain/package.hpp"
#include "domain/themes.hpp"
#include "scenes/errors.hpp"
#include "ui/overlays.hpp"

#include <algorithm>

namespace tz::scenes
{

namespace
{
constexpr const char* kBodyName = "body_LZ.bin";
constexpr const char* kMusicName = "bgm.bcstm";
} // namespace

void installTheme(const library::Source& source, const std::string& name, ThemeMode mode,
                  std::function<void(bool, library::FileId)> done)
{
    HomeStorage& storage = App::get().homeStorage();
    if (!storage.ready)
    {
        ui::toast(tr("Still reading the HOME Menu's settings, try again in a moment"), ui::ToastKind::Warning);
        done(false, {});
        return;
    }
    if (storage.error || !storage.theme || !storage.home)
    {
        showError(themeStorageError(storage.error.value_or(Error{Errc::NotFound})));
        done(false, {});
        return;
    }

    struct Outcome
    {
        library::FileId body;
        bool mono = false;
    };
    Archive* theme = storage.theme.get();
    Archive* home = storage.home.get();
    const char* title = mode == ThemeMode::MusicOnly ? "Installing the music of {}" : "Installing {}";

    App::get().scenes().push(ui::ProgressOverlay::start<Expected<Outcome>>(
        {trf(title, name), Lane::Io, false, true, gfx::accent::kThemes.solid},
        [source, mode, theme, home](ui::TaskProgress& progress, const CancelToken&) -> Expected<Outcome> {
            progress.setStatus(tr("Reading the theme"));
            progress.set(0, 3);
            const Package package = TZ_TRY(Package::open(App::get().sd(), source.path, source.zip));
            Outcome outcome;
            Bytes body;
            if (mode != ThemeMode::MusicOnly)
            {
                if (!package.has(kBodyName))
                    return fail(Errc::InvalidData, "no body_LZ.bin");
                body = TZ_TRY(package.read(kBodyName, home::kBodyCacheSize + 1));
                outcome.body = {static_cast<uint32_t>(body.size()), crc32(body)};
            }
            Bytes bgm;
            if (mode != ThemeMode::NoMusic && package.has(kMusicName))
            {
                bgm = TZ_TRY(package.read(kMusicName, home::kBgmCacheSize + 1));
                if (auto info = bcstm::parse(ByteSpan(bgm).first(std::min(bgm.size(), bcstm::kHeaderSize))))
                    outcome.mono = info->mono();
            }
            progress.setStatus(tr("Writing to the HOME Menu"));
            progress.set(1, 3);
            if (mode == ThemeMode::MusicOnly)
                TZ_CHECK(themes::installMusic(*theme, *home, bgm));
            else
                TZ_CHECK(themes::install(*theme, *home, body, bgm));
            progress.set(3, 3);
            return outcome;
        },
        [name, mode, done](Expected<Outcome> result) {
            if (!result)
            {
                if (result.error().code == Errc::NotFound && mode == ThemeMode::MusicOnly)
                    showError({tr("No theme for the music"),
                               tr("The music can only go with a theme installed by this app or another theme "
                                  "manager. Install a theme first.")});
                else
                    showError(installError(result.error(), name));
                done(false, {});
                return;
            }
            App::get().themeChanged(false);
            ui::toast(trf(mode == ThemeMode::MusicOnly ? "The music of {} is on" : "{} is installed", name),
                      ui::ToastKind::Success);
            if (result->mono)
                ui::toast(tr("Its music is mono, which the HOME Menu can play badly"), ui::ToastKind::Warning);
            done(true, result->body);
        }));
}

void installSplash(const library::Source& source, const std::string& name, splashes::Screens screens,
                   std::function<void(bool)> done)
{
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<bool>>(
        {trf("Installing {}", name), Lane::Io, false, false, gfx::accent::kSplashes.solid},
        [source, screens](ui::TaskProgress& progress, const CancelToken&) -> Expected<bool> {
            progress.setStatus(tr("Copying the splash to Luma3DS"));
            FileSystem& sd = App::get().sd();
            const Package package = TZ_TRY(Package::open(sd, source.path, source.zip));
            Bytes top;
            Bytes bottom;
            if (package.has(splashes::kTopName))
                top = TZ_TRY(package.read(splashes::kTopName, splashes::kMaxSize));
            if (package.has(splashes::kBottomName))
                bottom = TZ_TRY(package.read(splashes::kBottomName, splashes::kMaxSize));
            TZ_CHECK(splashes::install(sd, top, bottom, screens));
            return splashes::lumaShowsSplashes(sd);
        },
        [name, done](Expected<bool> shown) {
            if (!shown)
            {
                showError(installError(shown.error(), name));
                done(false);
                return;
            }
            ui::toast(trf("{} is installed", name), ui::ToastKind::Success);
            if (!*shown)
                ui::toast(tr("Splashes are off in Luma3DS's settings: turn them on to see it"),
                          ui::ToastKind::Warning);
            done(true);
        }));
}

ContentKind kindOf(const std::vector<std::string>& files)
{
    bool pictures = false;
    for (const std::string& file : files)
    {
        const std::string_view name = path::filename(file);
        if (path::equalsIgnoreCase(name, kBodyName))
            return ContentKind::Theme;
        if (path::equalsIgnoreCase(name, splashes::kTopName) || path::equalsIgnoreCase(name, splashes::kBottomName))
            return ContentKind::Splash;
        if (path::hasExtension(name, "png") && !path::equalsIgnoreCase(name, "preview.png"))
            pictures = true;
    }
    return pictures ? ContentKind::Badges : ContentKind::Unknown;
}

} // namespace tz::scenes
