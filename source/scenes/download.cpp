// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/download.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/path.hpp"
#include "core/zip.hpp"
#include "gfx/draw.hpp"
#include "scenes/errors.hpp"
#include "sys/net.hpp"
#include "ui/overlays.hpp"

namespace tz::scenes
{

namespace
{

constexpr uint64_t kMaxDownload = 32 * 1024 * 1024;

const char* folderFor(ContentKind kind)
{
    switch (kind)
    {
    case ContentKind::Theme:
        return library::kThemesRoot;
    case ContentKind::Splash:
        return library::kSplashesRoot;
    case ContentKind::Badges:
        return library::kBadgesRoot;
    case ContentKind::Unknown:
        break;
    }
    return nullptr;
}

// "Name.zip", or "Name (2).zip" and up when that's taken.
Expected<std::string> freePath(FileSystem& sd, const std::string& folder, const std::string& name)
{
    const std::string stem(path::stem(name));
    for (int n = 1; n < 1000; ++n)
    {
        const std::string candidate = path::join(folder, n == 1 ? stem + ".zip"
                                                                : stem + " (" + std::to_string(n) + ").zip");
        if (!sd.exists(candidate))
            return candidate;
    }
    return fail(Errc::NoSpace, "no free name for " + name);
}

} // namespace

void downloadToSd(std::string url, std::string title, std::string fallbackName, std::function<void(Saved)> done)
{
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<Saved>>(
        {std::move(title), Lane::Network, true, false, gfx::color::kPrimary},
        [url, fallbackName](ui::TaskProgress& task, const CancelToken& token) -> Expected<Saved> {
            task.setStatus(tr("Downloading"));
            net::Client client;
            net::Request request;
            request.url = url;
            request.accept = {"application/zip", "application/x-zip-compressed", "application/octet-stream"};
            request.maxSize = kMaxDownload;
            request.cancel = &token;
            request.progress = &task.download;
            net::Response response = TZ_TRY(client.get(request));

            task.setStatus(tr("Saving it to the SD card"));
            std::shared_ptr<Reader> reader = std::make_shared<MemoryReader>(std::move(response.body));
            const ZipArchive zip = TZ_TRY(ZipArchive::open(reader));
            std::vector<std::string> files;
            for (const ZipEntry& entry : zip.entries())
                files.push_back(entry.name);
            Saved saved;
            saved.kind = kindOf(files);
            const char* folder = folderFor(saved.kind);
            if (!folder)
                return fail(Errc::Unsupported, "neither a theme, a splash nor badges");

            std::string name = response.filename.empty() ? fallbackName : response.filename;
            name = path::sanitizeFilename(name.empty() ? std::string("download") : name);
            FileSystem& sd = App::get().sd();
            TZ_CHECK(sd.createDirectories(folder));
            saved.path = TZ_TRY(freePath(sd, folder, name));
            Bytes data = TZ_TRY(reader->readAll(kMaxDownload));
            saved.size = data.size();
            TZ_CHECK(sd.writeFile(saved.path, data));
            return saved;
        },
        [done](Expected<Saved> saved) {
            if (!saved)
            {
                if (saved.error().code == Errc::Unsupported)
                    showError({tr("That's not something to install"),
                               tr("The download isn't a theme, a splash screen or a badge set.")});
                else
                    showError(downloadError(saved.error()));
                return;
            }
            const App::Folder folder = saved->kind == ContentKind::Theme
                                           ? App::Folder::Themes
                                           : (saved->kind == ContentKind::Splash ? App::Folder::Splashes
                                                                                 : App::Folder::Badges);
            App::get().folderChanged(folder);
            done(*saved);
        }));
}

void offerInstall(const Saved& saved, const std::string& name)
{
    const std::string file(path::filename(saved.path));
    if (saved.kind == ContentKind::Badges)
    {
        App::get().scenes().emplace<ui::Dialog>(
            gfx::Icon::Badge, gfx::accent::kBadges.light, trf("{} is saved", name),
            trf("It's in /Badges as {}. Install your badges from the Badges tab.", file),
            std::vector<ui::DialogButton>{{tr("OK"), nullptr}});
        return;
    }
    const library::Source source{saved.path, file, true, saved.size};
    const bool theme = saved.kind == ContentKind::Theme;
    App::get().scenes().emplace<ui::Dialog>(
        theme ? gfx::Icon::Theme : gfx::Icon::Splash, theme ? gfx::accent::kThemes.light : gfx::accent::kSplashes.light,
        trf("{} is saved", name), trf("Install it now? It's in {} as {}.", path::parent(saved.path), file),
        std::vector<ui::DialogButton>{
            {tr("Later"), nullptr},
            {tr("Install"),
             [source, name, theme] {
                 if (theme)
                     installTheme(source, name, ThemeMode::Full, [](bool, library::FileId) {});
                 else
                     installSplash(source, name, splashes::Screens::Both, [](bool) {});
             }},
        });
}

} // namespace tz::scenes
