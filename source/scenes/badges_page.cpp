// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/badges_page.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/path.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "scenes/errors.hpp"
#include "scenes/frame.hpp"
#include "sys/act.hpp"
#include "sys/log.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

namespace tz::scenes
{

using namespace gfx;

namespace
{

constexpr int kPreviewColumns = 7;
constexpr int kPreviewRows = 3;
constexpr int kPreviewBadge = 48;
constexpr int kPreviewGap = 6;

uint64_t keyOf(const badges::SetSource& set)
{
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : set.name + "|" + set.folderPath + "|" + set.zipPath)
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

// The first badges of a set in a grid on the top screen, under the status bar.
preview::Screens grid(const std::vector<Image>& pictures)
{
    preview::Screens screens;
    screens.top = Image(400, 240);
    for (int y = 0; y < 240; ++y)
    {
        for (int x = 0; x < 400; ++x)
        {
            uint8_t* p = screens.top.at(x, y);
            // stone 900, a little lighter towards the top
            const uint8_t shade = static_cast<uint8_t>(36 - y / 24);
            p[0] = shade;
            p[1] = static_cast<uint8_t>(shade - 3);
            p[2] = static_cast<uint8_t>(shade - 5);
            p[3] = 255;
        }
    }
    const int width = kPreviewColumns * kPreviewBadge + (kPreviewColumns - 1) * kPreviewGap;
    const int left = (400 - width) / 2;
    const int top = 30;
    for (size_t i = 0; i < pictures.size() && i < kPreviewColumns * kPreviewRows; ++i)
    {
        const Image badge = resize(pictures[i], kPreviewBadge, kPreviewBadge);
        const int ox = left + static_cast<int>(i % kPreviewColumns) * (kPreviewBadge + kPreviewGap);
        const int oy = top + static_cast<int>(i / kPreviewColumns) * (kPreviewBadge + kPreviewGap);
        for (int y = 0; y < kPreviewBadge; ++y)
        {
            for (int x = 0; x < kPreviewBadge; ++x)
            {
                const uint8_t* from = badge.at(x, y);
                uint8_t* to = screens.top.at(ox + x, oy + y);
                const int a = from[3];
                for (int c = 0; c < 3; ++c)
                    to[c] = static_cast<uint8_t>((from[c] * a + to[c] * (255 - a)) / 255);
            }
        }
    }
    return screens;
}

Expected<Bytes> defaultSetIcon()
{
    FILE* file = std::fopen("romfs:/badge-set.png", "rb");
    if (!file)
        return fail(Errc::NotFound, "romfs:/badge-set.png");
    Bytes data(64 * 1024);
    const size_t read = std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    data.resize(read);
    return data;
}

} // namespace

BadgesPage::BadgesPage()
{
    list_.setRowHeight(metric::kRowHeight);
}

BadgesPage::~BadgesPage()
{
    token_.cancel();
}

ui::Tab BadgesPage::tab() const
{
    return {tr("Badges"), accent::kBadges};
}

void BadgesPage::onShow()
{
    if (state_ == State::NotLoaded)
        load();
}

void BadgesPage::load()
{
    generation_ = App::get().folderGeneration(App::Folder::Badges);
    state_ = State::Loading;
    previews_.clear();
    atlas_.clear();
    const std::string looseName = tr("Other badges");
    jobs::async(
        Lane::Io,
        [looseName](const CancelToken&) {
            FileSystem& sd = App::get().sd();
            sd.createDirectories(library::kBadgesRoot);
            return badges::findSets(sd, library::kBadgesRoot, looseName);
        },
        [this, token = token_](Expected<std::vector<badges::SetSource>> found) {
            if (token.cancelled())
                return;
            if (!found)
            {
                state_ = State::Failed;
                error_ = describe(found.error()).message;
                return;
            }
            sets_.clear();
            for (badges::SetSource& source : *found)
            {
                Set set;
                set.key = keyOf(source);
                set.source = std::move(source);
                sets_.push_back(std::move(set));
            }
            list_.setCount(static_cast<int>(sets_.size()));
            list_.select(0, false);
            state_ = State::Ready;

            // how many pictures each set has, and its icon, one set after another
            for (const Set& set : sets_)
            {
                const badges::SetSource source = set.source;
                const uint64_t key = set.key;
                jobs::async(
                    Lane::Io,
                    [source](const CancelToken&) {
                        struct Result
                        {
                            Expected<badges::SetSummary> summary;
                            std::optional<std::array<uint16_t, 48 * 48>> icon;
                        };
                        auto result = std::make_shared<Result>();
                        FileSystem& sd = App::get().sd();
                        result->summary = badges::summarize(sd, source);
                        if (auto image = badges::setIcon(sd, source))
                        {
                            std::array<uint16_t, 48 * 48> icon{};
                            imageToIcon(resize(*image, 48, 48), icon.data(), 48);
                            result->icon = icon;
                        }
                        return result;
                    },
                    [this, key, token](auto result) {
                        if (token.cancelled())
                            return;
                        for (Set& set : sets_)
                        {
                            if (set.key != key)
                                continue;
                            if (result->summary)
                            {
                                set.summary = *result->summary;
                                set.summarized = true;
                            }
                            if (result->icon)
                                atlas_.put(key, result->icon->data());
                        }
                    });
            }
        },
        5);
    readInstalledCount();
}

void BadgesPage::readInstalledCount()
{
    // the badge storage opens here, on the main thread, where the app keeps it; the worker only reads it
    Error error;
    Archive* archive = App::get().badges(&error);
    if (!archive)
    {
        installedCount_ = -1;
        storageBusy_ = error.code == Errc::Busy;
        return;
    }
    jobs::async(
        Lane::Io, [archive](const CancelToken&) { return badges::installedCount(*archive); },
        [this, token = token_](int count) {
            if (!token.cancelled())
                installedCount_ = count;
        });
}

BadgesPage::Set* BadgesPage::selected()
{
    const int index = list_.selected();
    return index >= 0 && index < static_cast<int>(sets_.size()) ? &sets_[index] : nullptr;
}

int BadgesPage::includedCount() const
{
    return static_cast<int>(std::count_if(sets_.begin(), sets_.end(), [](const Set& s) { return s.included; }));
}

void BadgesPage::tick(float)
{
    if (state_ == State::Ready && App::get().folderGeneration(App::Folder::Badges) != generation_)
        load();
}

void BadgesPage::update(ui::Input& input, float dt)
{
    if (state_ != State::Ready)
        return;
    list_.setBounds(0, metric::kTabBarHeight, metric::kBottomWidth,
                    metric::kHeight - metric::kTabBarHeight - metric::kHintBarHeight);
    const auto event = list_.update(input, dt);
    Set* set = selected();
    if (!set)
        return;
    if (list_.settled())
    {
        const badges::SetSource source = set->source;
        previews_.want(set->key, [source](const CancelToken& token) -> Expected<preview::Screens> {
            auto pictures = TZ_TRY(badges::previewBadges(App::get().sd(), source, kPreviewColumns * kPreviewRows,
                                                         token));
            if (pictures.empty())
                return fail(Errc::NotFound, "no badges");
            return grid(pictures);
        });
    }
    if (event == ui::ListView::Event::Activated)
        openActions();
    else if (input.pressed(KEY_X))
        set->included = !set->included;
    else if (input.pressed(KEY_Y))
        install();
}

std::vector<ui::Hint> BadgesPage::hints() const
{
    if (state_ != State::Ready || sets_.empty())
        return {};
    auto* self = const_cast<BadgesPage*>(this);
    const Set* set = self->selected();
    return {
        {KEY_A, tr("More")},
        {KEY_X, set && set->included ? tr("Skip") : tr("Include")},
        {KEY_Y, trf("Install {}", std::to_string(includedCount()))},
    };
}

std::shared_ptr<Picture> BadgesPage::backdrop() const
{
    auto* self = const_cast<BadgesPage*>(this);
    const Set* set = self->selected();
    if (!set)
        return nullptr;
    const PreviewPictures* pictures = self->previews_.get(set->key);
    return pictures ? pictures->blur : nullptr;
}

std::string BadgesPage::detailOf(const Set& set) const
{
    if (set.source.loose())
        return tr("The pictures directly in /Badges");
    std::string detail;
    if (set.summarized)
        detail = trf(set.summary.images == 1 ? "{} picture" : "{} pictures", std::to_string(set.summary.images));
    if (!set.source.author.empty())
        detail += (detail.empty() ? "" : " \u00B7 ") + set.source.author;
    return detail;
}

void BadgesPage::drawTop()
{
    const Set* set = selected();
    if (state_ != State::Ready || !set)
    {
        // what the HOME Menu has, even without sets on the SD card
        if (installedCount_ >= 0 && state_ == State::Ready)
            drawText(trf("{} badges on the HOME Menu", std::to_string(installedCount_)), metric::kTopWidth / 2,
                     textTop(metric::kHeight - 22, metric::kTextSmall), metric::kTextSmall, color::kTextDim,
                     Align::Center);
        return;
    }
    const PreviewPictures* pictures = previews_.get(set->key);
    if (pictures && pictures->top)
        pictures->top->draw(0, 0, metric::kTopWidth, metric::kHeight);
    else if (previews_.loading(set->key))
        spinner(metric::kTopWidth / 2, 110, 14, color::kTextDim);

    // under the grid: the set's name and how many badges it has
    const float nameMiddle = metric::kHeight - 36;
    drawText(set->source.name, 24, textTop(nameMiddle, metric::kTextLarge), metric::kTextLarge, color::kText,
             Align::Left, metric::kTopWidth - 48);
    std::string line = detailOf(*set);
    if (installedCount_ >= 0)
        line += (line.empty() ? "" : "   ") + trf("The HOME Menu has {} now", std::to_string(installedCount_));
    else if (storageBusy_)
        line += std::string(line.empty() ? "" : "   ") + tr("The HOME Menu is using its badges");
    drawText(line, 24, textTop(metric::kHeight - 15, metric::kTextSmall), metric::kTextSmall, color::kTextDim,
             Align::Left, metric::kTopWidth - 48);
}

void BadgesPage::drawBottom(float top, float bottom)
{
    if (state_ == State::Loading || state_ == State::NotLoaded)
    {
        spinner(metric::kBottomWidth / 2, (top + bottom) / 2, 14, accent().light);
        return;
    }
    if (state_ == State::Failed)
    {
        drawEmptyState(Icon::Error, color::kDanger, tr("The SD card can't be read"), error_, top, bottom);
        return;
    }
    if (sets_.empty())
    {
        drawEmptyState(Icon::Badge, accent().light, tr("No badge sets yet"),
                       tr("Find some on the Themezer tab, or put folders or zips of PNGs in /Badges."), top, bottom);
        return;
    }
    list_.draw([this](int index, float x, float y, float w, float h, bool isSelected) {
        const Set& set = sets_[index];
        if (isSelected)
            row::highlight(x, y, w, h, accent().solid);
        if (!atlas_.draw(set.key, x + row::kIconX, row::iconTop(y, h), row::kIconSize, set.included ? 1.0f : 0.35f))
            row::placeholder(x, y, h, set.source.loose() ? Icon::Images : Icon::Badge);
        // whether it goes in with the next install
        const float right = row::mark(set.included ? Icon::SquareCheck : Icon::Square,
                                      set.included ? accent().light : color::kTextFaint, x + w - row::kRight, y, h);
        row::lines(set.source.name, detailOf(set), x, y, h, right, !set.included);
    });
}

void BadgesPage::openActions()
{
    Set* set = selected();
    if (!set)
        return;
    const uint64_t key = set->key;
    std::vector<ui::Action> actions;
    actions.push_back({Icon::Install, trf("Install {} badge sets", std::to_string(includedCount())),
                       tr("They replace the badges on the HOME Menu"), includedCount() > 0, false,
                       [this] { install(); }});
    actions.push_back({set->included ? Icon::Square : Icon::SquareCheck,
                       set->included ? tr("Leave this set out") : tr("Include this set"), {}, true, false,
                       [this, key] {
                           for (Set& s : sets_)
                               if (s.key == key)
                                   s.included = !s.included;
                       }});
    if (!set->source.loose())
        actions.push_back({Icon::Trash, tr("Delete from the SD card"), {}, true, true, [this, key] {
                               for (Set& s : sets_)
                                   if (s.key == key)
                                       confirmDelete(s);
                           }});
    App::get().scenes().emplace<ui::ActionSheet>(set->source.name, std::move(actions), accent().light);
}

void BadgesPage::install()
{
    std::vector<badges::SetSource> chosen;
    for (const Set& set : sets_)
        if (set.included)
            chosen.push_back(set.source);
    if (chosen.empty())
    {
        ui::toast(tr("Include at least one set (X)"), ui::ToastKind::Warning);
        return;
    }
    Error error;
    Archive* archive = App::get().badges(&error);
    if (!archive)
    {
        showError(badgeStorageError(error));
        return;
    }
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<badges::InstallResult>>(
        {tr("Installing badges"), Lane::Io, true, true, accent().solid},
        [chosen, archive](ui::TaskProgress& progress, const CancelToken& token) -> Expected<badges::InstallResult> {
            FileSystem& sd = App::get().sd();
            progress.setStatus(tr("Keeping a copy of your badges"));
            TZ_CHECK(badges::backupOnce(*archive, sd, kBadgeBackupFolder));
            // the badge files carry the console's Nintendo Network ID; without one, what's there stays
            const auto principal = act::principalId();
            const uint32_t id = principal ? *principal : badges::storedPrincipalId(*archive);
            const Bytes icon = TZ_TRY(defaultSetIcon());
            progress.setStatus(tr("Writing the badges"));
            return badges::install(sd, *archive, chosen, id, icon, [&](int done, int total) {
                progress.set(done, total);
                return !token.cancelled();
            });
        },
        [this, token = token_](Expected<badges::InstallResult> result) {
            if (token.cancelled())
                return;
            if (!result)
            {
                showError(installError(result.error(), tr("The badges")));
                return;
            }
            installedCount_ = result->badges;
            ui::toast(trf("{} badges in {} sets are installed", std::to_string(result->badges),
                          std::to_string(result->sets)),
                      ui::ToastKind::Success);
            if (result->full)
                ui::toast(tr("The HOME Menu has room for 1000 badges in 100 sets: the rest was left out"),
                          ui::ToastKind::Warning);
            if (result->skipped > 0)
                ui::toast(trf("{} pictures couldn't be used", std::to_string(result->skipped)),
                          ui::ToastKind::Warning);
            ui::toast(tr("The HOME Menu shows them after a restart"));
        }));
}

void BadgesPage::uninstall()
{
    Error error;
    Archive* archive = App::get().badges(&error);
    if (!archive)
    {
        showError(badgeStorageError(error));
        return;
    }
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Trash, color::kDanger, tr("Take all badges off the HOME Menu?"),
        tr("Your badge sets stay on the SD card, and the app keeps a copy of the badges you had before it first "
           "changed them."),
        std::vector<ui::DialogButton>{
            {tr("Keep them"), nullptr},
            {tr("Take them off"),
             [this, archive] {
                 jobs::async(
                     Lane::Io,
                     [archive](const CancelToken&) -> Expected<void> {
                         TZ_CHECK(badges::backupOnce(*archive, App::get().sd(), kBadgeBackupFolder));
                         return badges::uninstall(*archive);
                     },
                     [this, token = token_](Expected<void> result) {
                         if (token.cancelled())
                             return;
                         if (!result)
                         {
                             showError(describe(result.error()));
                             return;
                         }
                         installedCount_ = 0;
                         ui::toast(tr("The badges are off the HOME Menu"), ui::ToastKind::Success);
                     });
             },
             true},
        });
}

void BadgesPage::exportBadges()
{
    Error error;
    Archive* archive = App::get().badges(&error);
    if (!archive)
    {
        showError(badgeStorageError(error));
        return;
    }
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<std::pair<int, std::string>>>(
        {tr("Saving the HOME Menu's badges"), Lane::Io, true, false, accent().solid},
        [archive](ui::TaskProgress& progress, const CancelToken& token) -> Expected<std::pair<int, std::string>> {
            const time_t now = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M", gmtime(&now));
            const std::string folder = std::string(kAppFolder) + "/Badge exports/" + stamp;
            const int count = TZ_TRY(badges::exportBadges(*archive, App::get().sd(), folder, [&](int done, int total) {
                progress.set(done, total);
                return !token.cancelled();
            }));
            return std::make_pair(count, folder);
        },
        [](Expected<std::pair<int, std::string>> result) {
            if (!result)
            {
                showError(describe(result.error()));
                return;
            }
            ui::toast(trf("{} badges saved in {}", std::to_string(result->first), result->second),
                      ui::ToastKind::Success);
        }));
}

void BadgesPage::restore()
{
    Error error;
    Archive* archive = App::get().badges(&error);
    if (!archive)
    {
        showError(badgeStorageError(error));
        return;
    }
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Restore, accent().light, tr("Bring back your old badges?"),
        tr("The badges the HOME Menu had before this app first changed them come back, in place of the ones it has "
           "now."),
        std::vector<ui::DialogButton>{
            {tr("Cancel"), nullptr},
            {tr("Bring back"),
             [this, archive] {
                 jobs::async(
                     Lane::Io,
                     [archive](const CancelToken&) {
                         return badges::restoreBackup(App::get().sd(), kBadgeBackupFolder, *archive);
                     },
                     [this, token = token_](Expected<void> result) {
                         if (token.cancelled())
                             return;
                         if (!result)
                         {
                             showError(describe(result.error()));
                             return;
                         }
                         readInstalledCount();
                         ui::toast(tr("Your old badges are back"), ui::ToastKind::Success);
                     });
             }},
        });
}

void BadgesPage::confirmDelete(Set& set)
{
    const badges::SetSource source = set.source;
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Trash, color::kDanger, trf("Delete {}?", source.name),
        tr("The set is removed from your SD card. Badges on the HOME Menu stay."),
        std::vector<ui::DialogButton>{
            {tr("Keep"), nullptr},
            {tr("Delete"),
             [this, source] {
                 jobs::async(
                     Lane::Io,
                     [source](const CancelToken&) -> Expected<void> {
                         FileSystem& sd = App::get().sd();
                         if (!source.folderPath.empty())
                             TZ_CHECK(sd.removeDirectory(source.folderPath, true));
                         if (!source.zipPath.empty())
                             TZ_CHECK(sd.removeFile(source.zipPath));
                         return {};
                     },
                     [this, token = token_](Expected<void> result) {
                         if (token.cancelled())
                             return;
                         if (!result)
                         {
                             showError(describe(result.error()));
                             return;
                         }
                         ui::toast(tr("The set is deleted"), ui::ToastKind::Success);
                         load();
                     });
             },
             true},
        });
}

std::vector<ui::Action> BadgesPage::menuActions()
{
    std::vector<ui::Action> actions;
    actions.push_back({Icon::Trash, tr("Take all badges off the HOME Menu"), {}, true, true, [this] { uninstall(); }});
    actions.push_back({Icon::Save, tr("Save the HOME Menu's badges to the SD card"),
                       tr("As PNGs, a folder per set, in /3ds/Themezer/Badge exports"), true, false,
                       [this] { exportBadges(); }});
    const bool backup = badges::hasBackup(App::get().sd(), kBadgeBackupFolder);
    actions.push_back({Icon::Restore, tr("Bring back the badges you had before"),
                       backup ? tr("From before this app first changed them") : tr("There's no copy yet"), backup,
                       false, [this] { restore(); }});
    actions.push_back({Icon::Refresh, tr("Look at the SD card again"), {}, true, false, [this] { load(); }});
    return actions;
}

} // namespace tz::scenes
