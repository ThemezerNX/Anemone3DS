// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/themezer_page.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/bcstm.hpp"
#include "core/image.hpp"
#include "core/path.hpp"
#include "domain/library.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "scenes/download.hpp"
#include "scenes/errors.hpp"
#include "scenes/frame.hpp"
#include "scenes/preview_scene.hpp"
#include "sys/log.hpp"
#include "ui/keyboard.hpp"

#include <algorithm>
#include <array>

namespace tz::scenes
{

using namespace gfx;

namespace
{

constexpr float kKindBarHeight = 28;
constexpr float kSearchLineHeight = 22;
// the sub-tabs: the three kinds, then the QR code scanner
constexpr int kQrTab = 3;
// how near the end of the list the next page starts loading
constexpr int kPrefetchRows = 6;
constexpr uint64_t kMaxIconSize = 256 * 1024;
constexpr uint64_t kMaxPreviewSize = 4 * 1024 * 1024;

uint64_t keyOf(themezer::Kind kind, const std::string& id)
{
    uint64_t hash = 1469598103934665603ULL ^ static_cast<uint64_t>(kind);
    for (unsigned char c : id)
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

const char* folderOf(themezer::Kind kind)
{
    switch (kind)
    {
    case themezer::Kind::Splashes:
        return library::kSplashesRoot;
    case themezer::Kind::Badges:
        return library::kBadgesRoot;
    case themezer::Kind::Themes:
        break;
    }
    return library::kThemesRoot;
}

Accent accentOf(themezer::Kind kind)
{
    switch (kind)
    {
    case themezer::Kind::Splashes:
        return accent::kSplashes;
    case themezer::Kind::Badges:
        return accent::kBadges;
    case themezer::Kind::Themes:
        break;
    }
    return accent::kThemes;
}

// Downloads a PNG on the network worker.
Expected<Image> downloadPng(net::Client& client, const std::string& url, uint64_t maxSize, int maxWidth,
                            int maxHeight, const CancelToken& token)
{
    net::Request request;
    request.url = url;
    request.accept = {"image/png"};
    request.maxSize = maxSize;
    request.cancel = &token;
    const net::Response response = TZ_TRY(client.get(request));
    return decodePng(response.body, maxWidth, maxHeight);
}

} // namespace

ThemezerPage::ThemezerPage()
{
    list_.setRowHeight(metric::kRowHeight);
    kinds_.setTabs({
        {tr("Themes"), accent::kThemes},
        {tr("Splashes"), accent::kSplashes},
        {tr("Badges"), accent::kBadges},
        {{}, accent::kApp, Icon::Qr},
    });
}

ThemezerPage::~ThemezerPage()
{
    token_.cancel();
}

ui::Tab ThemezerPage::tab() const
{
    // themezer.net, browsed: a globe, as a browser has
    return {{}, accent::kApp, Icon::Globe};
}

Accent ThemezerPage::accent() const
{
    return accentOf(kind_);
}

void ThemezerPage::onShow()
{
    if (scanningTab())
        qr_.start();
    else if (state_ == State::Idle)
        reload();
}

void ThemezerPage::onHide()
{
    qr_.stop();
}

bool ThemezerPage::scanningTab() const
{
    return kinds_.selected() == kQrTab;
}

void ThemezerPage::reload()
{
    // what's loading for the old list is of no use any more
    token_.cancel();
    token_ = CancelToken();
    entries_.clear();
    atlas_.clear();
    previews_.clear();
    list_.setCount(0);
    page_ = 0;
    pages_ = 1;
    fetching_ = false;
    state_ = State::Loading;
    fetchPage(1);
}

void ThemezerPage::fetchPage(int page)
{
    if (fetching_)
        return;
    fetching_ = true;
    const themezer::Kind kind = kind_;
    const std::string query = query_;
    auto client = client_;
    jobs::run(
        Lane::Network, token_,
        [this, client, kind, query, page, token = token_](const CancelToken&) {
            auto result = std::make_shared<Expected<themezer::Page>>(
                themezer::fetchPage(*client, kind, page, query, token));
            jobs::post([this, result, token] {
                if (token.cancelled())
                    return;
                fetching_ = false;
                if (!*result)
                {
                    if (entries_.empty())
                    {
                        state_ = State::Failed;
                        error_ = describe(result->error()).title;
                    }
                    else
                    {
                        ui::toast(describe(result->error()).title, ui::ToastKind::Error);
                    }
                    return;
                }
                page_ = (*result)->page;
                pages_ = (*result)->pages;
                const size_t from = entries_.size();
                for (themezer::Item& item : (*result)->items)
                {
                    Entry entry;
                    entry.key = keyOf(kind_, item.id);
                    entry.item = std::move(item);
                    entries_.push_back(std::move(entry));
                }
                list_.setCount(static_cast<int>(entries_.size()));
                state_ = State::Ready;
                checkOnSd(from);
                requestIcons();
            });
        },
        10);
}

void ThemezerPage::checkOnSd(size_t from)
{
    // which of these are on the SD card already, by their file name
    std::vector<std::pair<uint64_t, std::string>> names;
    for (size_t i = from; i < entries_.size(); ++i)
        names.emplace_back(entries_[i].key, path::join(folderOf(kind_), entries_[i].item.filename));
    jobs::async(
        Lane::Io,
        [names](const CancelToken&) {
            std::vector<uint64_t> present;
            FileSystem& sd = App::get().sd();
            for (const auto& [key, file] : names)
                if (sd.exists(file))
                    present.push_back(key);
            return present;
        },
        [this, token = token_](std::vector<uint64_t> present) {
            if (token.cancelled())
                return;
            for (Entry& entry : entries_)
                if (std::find(present.begin(), present.end(), entry.key) != present.end())
                    entry.onSd = true;
        });
}

void ThemezerPage::requestIcons()
{
    // the rows on screen first, a few below them after
    const auto [first, last] = list_.visibleRange();
    const int end = std::min<int>(last + 4, static_cast<int>(entries_.size()) - 1);
    for (int i = std::max(0, first); i <= end; ++i)
    {
        Entry& entry = entries_[i];
        if (entry.iconRequested || entry.item.iconUrl.empty())
            continue;
        entry.iconRequested = true;
        const std::string url = entry.item.iconUrl;
        const uint64_t key = entry.key;
        auto client = client_;
        jobs::run(Lane::Network, token_, [this, client, url, key, token = token_](const CancelToken&) {
            auto image = downloadPng(*client, url, kMaxIconSize, 48, 48, token);
            if (!image || token.cancelled())
                return;
            auto icon = std::make_shared<std::array<uint16_t, 48 * 48>>();
            imageToIcon(resize(*image, 48, 48), icon->data(), 48);
            jobs::post([this, icon, key, token] {
                if (!token.cancelled())
                    atlas_.put(key, icon->data());
            });
        });
    }
}

ThemezerPage::Entry* ThemezerPage::selected()
{
    const int index = list_.selected();
    return index >= 0 && index < static_cast<int>(entries_.size()) ? &entries_[index] : nullptr;
}

void ThemezerPage::tick(float dt)
{
    // the preview fades in also under a menu that opened before it came
    if (const Entry* entry = selected(); entry && state_ == State::Ready)
    {
        const PreviewPictures* pictures = previews_.get(entry->key);
        fade_.update(entry->key, pictures && pictures->top, dt);
    }

    // what was downloaded meanwhile shows as on the SD card
    static uint32_t seen[3] = {};
    const auto folder = kind_ == themezer::Kind::Themes
                            ? App::Folder::Themes
                            : (kind_ == themezer::Kind::Splashes ? App::Folder::Splashes : App::Folder::Badges);
    const uint32_t generation = App::get().folderGeneration(folder);
    if (generation != seen[static_cast<size_t>(folder)] && state_ == State::Ready)
    {
        seen[static_cast<size_t>(folder)] = generation;
        checkOnSd(0);
    }
}

void ThemezerPage::update(ui::Input& input, float dt)
{
    // the kinds and the QR code scanner, under the tabs: the D-pad's left and right, or a tap
    if (kinds_.update(input))
    {
        if (scanningTab())
        {
            qr_.start();
        }
        else
        {
            qr_.stop();
            switchKind(static_cast<themezer::Kind>(kinds_.selected()));
        }
        return;
    }
    if (scanningTab())
    {
        if (auto text = qr_.update())
            downloadScanned(*text);
        else if (!qr_.scanning() && input.pressed(KEY_A))
            qr_.start();
        return;
    }
    if (input.pressed(KEY_X))
    {
        search();
        return;
    }
    if (!query_.empty() && input.pressed(KEY_B))
    {
        query_.clear();
        reload();
        return;
    }
    if (state_ == State::Failed && input.pressed(KEY_A))
    {
        reload();
        return;
    }
    if (state_ != State::Ready)
        return;

    const float top = listTop(metric::kTabBarHeight);
    list_.setBounds(0, top, metric::kBottomWidth, metric::kHeight - top - metric::kHintBarHeight);
    const auto event = list_.update(input, dt);
    requestIcons();

    // the next page, before the end is reached
    if (page_ < pages_ && list_.selected() >= static_cast<int>(entries_.size()) - kPrefetchRows)
        fetchPage(page_ + 1);
    if (list_.visibleRange().second >= static_cast<int>(entries_.size()) - 2 && page_ < pages_)
        fetchPage(page_ + 1);

    Entry* entry = selected();
    if (!entry)
        return;
    if (list_.settled() && !entry->item.previewUrl.empty())
    {
        const std::string url = entry->item.previewUrl;
        auto client = client_;
        previews_.want(
            entry->key,
            [client, url](const CancelToken& token) -> Expected<preview::Screens> {
                const Image picture = TZ_TRY(downloadPng(*client, url, kMaxPreviewSize, 1024, 1024, token));
                return preview::split(picture);
            },
            Lane::Network);
    }
    if (event == ui::ListView::Event::Activated)
        openActions();
    else if (input.pressed(KEY_Y))
        openPreview(*entry);
}

std::vector<ui::Hint> ThemezerPage::hints() const
{
    std::vector<ui::Hint> hints;
    if (scanningTab())
    {
        if (!qr_.scanning())
            hints.push_back({KEY_A, tr("Scan")});
        return hints;
    }
    if (state_ == State::Failed)
        hints.push_back({KEY_A, tr("Try again")});
    else if (state_ == State::Ready && !entries_.empty())
    {
        hints.push_back({KEY_A, tr("Get")});
        hints.push_back({KEY_Y, tr("Preview")});
    }
    hints.push_back({KEY_X, tr("Search")});
    if (!query_.empty())
        hints.push_back({KEY_B, tr("Clear")});
    return hints;
}

float ThemezerPage::listTop(float top) const
{
    // under the kinds, and the search when there is one
    return top + kKindBarHeight + (query_.empty() ? 0 : kSearchLineHeight);
}

std::shared_ptr<Picture> ThemezerPage::backdrop() const
{
    auto* self = const_cast<ThemezerPage*>(this);
    const Entry* entry = self->selected();
    if (!entry)
        return nullptr;
    const PreviewPictures* pictures = self->previews_.get(entry->key);
    return pictures ? pictures->blur : nullptr;
}

float ThemezerPage::topCover() const
{
    if (scanningTab())
        return qr_.showsPicture() ? 1 : 0;
    return state_ == State::Ready ? fade_.alpha() : 0;
}

void ThemezerPage::drawTop()
{
    if (scanningTab())
    {
        qr_.drawTop();
        return;
    }
    const Entry* entry = selected();
    if (state_ != State::Ready || !entry)
        return;
    if (fade_.alpha() < 1)
        drawDetails(*entry);
    const PreviewPictures* pictures = previews_.get(entry->key);
    if (pictures && pictures->top && fade_.alpha() > 0)
        pictures->top->draw(0, 0, metric::kTopWidth, metric::kHeight, fade_.alpha());
}

void ThemezerPage::drawDetails(const Entry& entry)
{
    // until the preview is here: the icon, the name and the author
    constexpr float kLeft = 24;
    constexpr float kIcon = IconAtlas::kIconSize;
    const float top = metric::kStatusBarHeight + 24;
    if (!atlas_.draw(entry.key, kLeft, top, kIcon))
        roundedRect(kLeft, top, kIcon, kIcon, metric::kRadius, color::kSurfaceRaised);
    const float textX = kLeft + kIcon + 14;
    const float width = metric::kTopWidth - textX - kLeft;
    const float middle = top + kIcon / 2;
    drawText(entry.item.name, textX, textTop(middle - 9, metric::kTextLarge), metric::kTextLarge, color::kText,
             Align::Left, width);
    drawText(entry.item.author, textX, textTop(middle + 11, metric::kTextNormal), metric::kTextNormal,
             color::kTextDim, Align::Left, width);
    if (!entry.item.description.empty())
        drawWrapped(entry.item.description, kLeft, top + kIcon + 18, metric::kTopWidth - 2 * kLeft,
                    metric::kTextNormal, color::kTextDim, 4);
    if (previews_.loading(entry.key))
        spinner(metric::kTopWidth - 30, metric::kHeight - 26, 9, color::kTextDim);

    const float factsMiddle = metric::kHeight - 22;
    float x = kLeft;
    auto fact = [&](Icon which, const std::string& text, Color tint) {
        icon(which, x, std::round(factsMiddle - metric::kIcon / 2), metric::kIcon, tint);
        x += metric::kIcon + 4;
        x += drawText(text, x, textTop(factsMiddle, metric::kTextSmall), metric::kTextSmall, tint) + 16;
    };
    if (entry.onSd)
        fact(Icon::Check, tr("Downloaded"), accent().light);
    if (!entry.item.audioUrl.empty())
        fact(Icon::Music, tr("Has music"), color::kTextDim);
}

void ThemezerPage::drawBottom(float top, float bottom)
{
    kinds_.draw(0, top, metric::kBottomWidth, kKindBarHeight, metric::kTextSmall);
    if (scanningTab())
    {
        qr_.drawBottom(top + kKindBarHeight, bottom);
        return;
    }

    // what's searched for, above the results
    if (!query_.empty())
    {
        const float middle = top + kKindBarHeight + kSearchLineHeight / 2;
        icon(Icon::Search, 14, std::round(middle - metric::kIcon / 2), metric::kIcon, color::kTextDim);
        drawText(query_, 14 + metric::kIcon + 6, textTop(middle, metric::kTextSmall), metric::kTextSmall, color::kText,
                 Align::Left, metric::kBottomWidth - 48);
    }

    const float listTop = this->listTop(top);
    if (state_ == State::Loading || state_ == State::Idle)
    {
        spinner(metric::kBottomWidth / 2, (listTop + bottom) / 2, 14, accent().light);
        return;
    }
    if (state_ == State::Failed)
    {
        drawEmptyState(Icon::WifiOff, color::kTextDim, error_, tr("Press A to try again."), listTop, bottom);
        return;
    }
    if (entries_.empty())
    {
        drawEmptyState(Icon::SearchEmpty, accent().light,
                       query_.empty() ? tr("Nothing here yet") : trf("Nothing found for \"{}\"", query_),
                       query_.empty() ? tr("Try another kind.") : tr("Try other words, or another kind."), listTop,
                       bottom);
        return;
    }
    list_.draw([this](int index, float x, float y, float w, float h, bool isSelected) {
        const Entry& entry = entries_[index];
        if (isSelected)
            row::highlight(x, y, w, h, accent().solid);
        if (!atlas_.draw(entry.key, x + row::kIconX, row::iconTop(y, h), row::kIconSize))
            row::placeholder(x, y, h, kind_ == themezer::Kind::Themes
                                          ? Icon::Theme
                                          : (kind_ == themezer::Kind::Splashes ? Icon::Splash : Icon::Badge));
        float right = x + w - row::kRight;
        if (entry.onSd)
            right = row::status(Icon::Check, tr("Downloaded"), accent().light, right, y, h);
        if (!entry.item.audioUrl.empty())
            right = row::mark(Icon::Music, color::kTextFaint, right, y, h);
        row::lines(entry.item.name, entry.item.author, x, y, h, right);
    });
    if (fetching_ && !entries_.empty())
        spinner(metric::kBottomWidth / 2, bottom - 12, 7, accent().light);
}

void ThemezerPage::downloadScanned(const std::string& text)
{
    if (!text.starts_with("http://") && !text.starts_with("https://"))
    {
        showError({tr("That QR code isn't a download"), tr("It doesn't hold a link.")});
        return;
    }
    // Themezer's links are https; an old http one is asked for securely
    std::string url = text;
    if (url.starts_with("http://"))
        url = "https://" + url.substr(7);
    downloadToSd(url, tr("Downloading from the QR code"), "download.zip",
                 [](Saved saved) { offerInstall(saved, std::string(path::stem(path::filename(saved.path)))); });
}

void ThemezerPage::switchKind(themezer::Kind kind)
{
    if (kind == kind_)
        return;
    kind_ = kind;
    reload();
}

void ThemezerPage::search()
{
    const auto text = ui::keyboard(tr("Search Themezer"), query_, 64, tr("Cancel"), tr("Search"));
    if (!text || *text == query_)
        return;
    query_ = *text;
    reload();
}

void ThemezerPage::openActions()
{
    Entry* entry = selected();
    if (!entry)
        return;
    const uint64_t key = entry->key;
    auto find = [this](uint64_t k) -> Entry* {
        for (Entry& e : entries_)
            if (e.key == k)
                return &e;
        return nullptr;
    };
    std::vector<ui::Action> actions;
    if (kind_ != themezer::Kind::Badges)
        actions.push_back({Icon::Install, tr("Download and install"), {}, true, false, [this, key, find] {
                               if (Entry* e = find(key))
                                   download(*e, true);
                           }});
    actions.push_back({Icon::Download, entry->onSd ? tr("Download again") : tr("Download"),
                       trf("Into {}", folderOf(kind_)), true, false, [this, key, find] {
                           if (Entry* e = find(key))
                               download(*e, false);
                       }});
    actions.push_back({Icon::Eye, tr("Preview"), entry->item.audioUrl.empty() ? std::string() : tr("With its music"),
                       true, false, [this, key, find] {
                           if (Entry* e = find(key))
                               openPreview(*e);
                       }});
    App::get().scenes().emplace<ui::ActionSheet>(entry->item.name, std::move(actions), accent().light);
}

void ThemezerPage::download(Entry& entry, bool install)
{
    const std::string name = entry.item.name;
    const uint64_t key = entry.key;
    downloadToSd(entry.item.downloadUrl, trf("Downloading {}", name), entry.item.filename,
                 [this, name, key, install, token = token_](Saved saved) {
                     if (!token.cancelled())
                         for (Entry& e : entries_)
                             if (e.key == key)
                                 e.onSd = true;
                     const std::string file(path::filename(saved.path));
                     const library::Source source{saved.path, file, true, saved.size};
                     if (install && saved.kind == ContentKind::Theme)
                         installTheme(source, name, ThemeMode::Full, [](bool, library::FileId) {});
                     else if (install && saved.kind == ContentKind::Splash)
                         installSplash(source, name, splashes::Screens::Both, [](bool) {});
                     else if (saved.kind == ContentKind::Badges)
                         offerInstall(saved, name);
                     else
                         ui::toast(trf("{} is saved in {}", name, std::string(path::parent(saved.path))),
                                   ui::ToastKind::Success);
                 });
}

void ThemezerPage::openPreview(Entry& entry)
{
    PreviewSource preview;
    preview.title = entry.item.name;
    preview.key = entry.key;
    preview.previews = &previews_;
    preview.lane = Lane::Network;
    const std::string url = entry.item.previewUrl;
    auto client = client_;
    preview.loader = [client, url](const CancelToken& token) -> Expected<preview::Screens> {
        if (url.empty())
            return fail(Errc::NotFound, "no preview");
        const Image picture = TZ_TRY(downloadPng(*client, url, kMaxPreviewSize, 1024, 1024, token));
        return preview::split(picture);
    };
    if (!entry.item.audioUrl.empty())
    {
        const std::string audio = entry.item.audioUrl;
        // the music plays while it downloads
        preview.music = [audio]() -> Expected<std::unique_ptr<StreamReader>> {
            return net::stream(audio, bcstm::kMaxSize);
        };
    }
    App::get().scenes().push(makePreviewScene(std::move(preview)));
}

std::vector<ui::Action> ThemezerPage::menuActions()
{
    return {
        {Icon::Search, tr("Search"), query_.empty() ? std::string() : trf("Now: \"{}\"", query_), true, false,
         [this] { search(); }},
        {Icon::Refresh, tr("Load again"), {}, true, false, [this] { reload(); }},
    };
}

} // namespace tz::scenes
