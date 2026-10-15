// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/library_page.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/bcstm.hpp"
#include "core/path.hpp"
#include "core/zip.hpp"
#include "domain/package.hpp"
#include "domain/splashes.hpp"
#include "domain/themes.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "scenes/errors.hpp"
#include "scenes/frame.hpp"
#include "scenes/install.hpp"
#include "scenes/preview_scene.hpp"
#include "sys/log.hpp"

#include <algorithm>
#include <ctime>
#include <mutex>

namespace tz::scenes
{

using namespace gfx;

namespace
{

constexpr const char* kBodyName = "body_LZ.bin";
constexpr const char* kMusicName = "bgm.bcstm";
constexpr const char* kPreviewName = "preview.png";
constexpr uint64_t kMaxPreviewSize = 4 * 1024 * 1024;
constexpr int kMaxShuffle = home::kShuffleSlots;

// the order of ThemeMode
enum InstallMode
{
    kFull,
    kNoMusic,
    kMusicOnly,
};

uint64_t keyOf(const std::string& path)
{
    // FNV-1a: stable, and the same item keeps its icon slot after the list is sorted again
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : path)
    {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

std::string cachePath(library::Kind kind)
{
    return std::string(kCacheFolder) + (kind == library::Kind::Theme ? "/themes.bin" : "/splashes.bin");
}

// Reads an item's preview: preview.png, or for a splash without one, its own screens.
Expected<preview::Screens> readPreview(const library::Source& source, library::Kind kind, const CancelToken& token)
{
    FileSystem& sd = App::get().sd();
    const Package package = TZ_TRY(Package::open(sd, source.path, source.zip));
    if (token.cancelled())
        return fail(Errc::Cancelled);
    if (package.has(kPreviewName))
    {
        const Bytes png = TZ_TRY(package.read(kPreviewName, kMaxPreviewSize));
        if (token.cancelled())
            return fail(Errc::Cancelled);
        const Image picture = TZ_TRY(decodePng(png, 1024, 1024));
        return preview::split(picture);
    }
    if (kind == library::Kind::Splash)
    {
        preview::Screens screens;
        bool any = false;
        if (auto top = package.read(splashes::kTopName, splashes::kMaxSize))
        {
            if (auto image = splashes::toImage(*top, true))
            {
                screens.top = std::move(*image);
                any = true;
            }
        }
        if (auto bottom = package.read(splashes::kBottomName, splashes::kMaxSize))
        {
            if (auto image = splashes::toImage(*bottom, false))
            {
                screens.bottom = std::move(*image);
                if (!any)
                {
                    // only a bottom screen: the top shows black
                    screens.top = Image(400, 240);
                    for (size_t i = 3; i < screens.top.pixels.size(); i += 4)
                        screens.top.pixels[i] = 255;
                    any = true;
                }
            }
        }
        if (any)
            return screens;
    }
    return fail(Errc::NotFound, "no preview");
}

} // namespace

LibraryPage::LibraryPage(library::Kind kind) : kind_(kind)
{
    list_.setRowHeight(metric::kRowHeight);
}

LibraryPage::~LibraryPage()
{
    token_.cancel();
}

ui::Tab LibraryPage::tab() const
{
    if (kind_ == library::Kind::Theme)
        return {tr("Themes"), accent::kThemes};
    return {tr("Splashes"), accent::kSplashes};
}

Accent LibraryPage::accent() const
{
    return kind_ == library::Kind::Theme ? accent::kThemes : accent::kSplashes;
}

void LibraryPage::onShow()
{
    if (state_ == State::NotLoaded)
        load();
}

// ---------------------------------------------------------------------------------------------------------------------
// Loading

void LibraryPage::load()
{
    generation_ = App::get().folderGeneration(kind_ == library::Kind::Theme ? App::Folder::Themes
                                                                           : App::Folder::Splashes);
    state_ = State::Loading;
    entries_.clear();
    index_.clear();
    focusedInstalled_ = false;
    atlas_.clear();
    previews_.clear();
    shuffle_.clear();
    {
        std::lock_guard lock(infoQueue_->mutex);
        infoQueue_->pending.clear();
    }
    const library::Kind kind = kind_;
    struct Scan
    {
        Expected<std::vector<library::Source>> sources;
        std::vector<std::pair<library::Source, library::Info>> cached;
    };
    jobs::run(Lane::Io, token_, [this, kind, token = token_](const CancelToken&) {
        auto scan = std::make_shared<Scan>();
        FileSystem& sd = App::get().sd();
        scan->sources = library::scan(sd, library::root(kind));
        if (scan->sources)
            scan->cached = library::loadCache(sd, cachePath(kind), kind);
        jobs::post([this, token, scan] {
            if (token.cancelled())
                return;
            if (!scan->sources)
            {
                state_ = State::Failed;
                error_ = describe(scan->sources.error()).message;
                return;
            }
            onScanned(std::move(*scan->sources), std::move(scan->cached));
        });
    }, 5);
    refreshInstalled();
}

void LibraryPage::onScanned(std::vector<library::Source> sources,
                            std::vector<std::pair<library::Source, library::Info>> cached)
{
    std::unordered_map<std::string, library::Info*> byPath;
    for (auto& [source, info] : cached)
        byPath.emplace(source.path, &info);

    std::vector<library::Source> missing;
    entries_.reserve(sources.size());
    for (library::Source& source : sources)
    {
        Entry entry;
        entry.key = keyOf(source.path);
        if (auto it = byPath.find(source.path); it != byPath.end())
        {
            // a cached info only counts for the same file: a zip that changed size was replaced
            const auto cachedSource = std::find_if(cached.begin(), cached.end(),
                                                   [&](const auto& c) { return c.first.path == source.path; });
            if (cachedSource != cached.end() && cachedSource->first == source)
                entry.item.info = *it->second;
        }
        if (!entry.item.info)
            missing.push_back(source);
        entry.item.source = std::move(source);
        entries_.push_back(std::move(entry));
    }

    resort(false);
    for (Entry& entry : entries_)
        applyInstalled(entry);
    state_ = State::Ready;
    list_.setCount(static_cast<int>(entries_.size()));
    list_.select(0, false);

    if (!missing.empty())
    {
        {
            std::lock_guard lock(infoQueue_->mutex);
            infoQueue_->pending.assign(missing.begin(), missing.end());
        }
        prioritiseVisible();
        startInfoReader();
    }
    // a cache with entries for items that have gone is written again without them
    if (cached.size() != entries_.size() - missing.size())
        saveCacheLater();
}

void LibraryPage::startInfoReader()
{
    {
        std::lock_guard lock(infoQueue_->mutex);
        if (infoQueue_->running || infoQueue_->pending.empty())
            return;
        infoQueue_->running = true;
    }
    auto queue = infoQueue_;
    const library::Kind kind = kind_;
    const int language = App::get().language();
    jobs::run(Lane::Io, token_, [this, queue, kind, language, token = token_](const CancelToken&) {
        for (;;)
        {
            library::Source source;
            {
                std::lock_guard lock(queue->mutex);
                if (queue->pending.empty() || token.cancelled())
                {
                    queue->running = false;
                    return;
                }
                source = std::move(queue->pending.front());
                queue->pending.pop_front();
            }
            auto info = std::make_shared<Expected<library::Info>>(
                library::readInfo(App::get().sd(), source, kind, language));
            jobs::post([this, token, source, info] {
                if (!token.cancelled())
                    onInfo(source, std::move(*info));
            });
        }
    });
}

void LibraryPage::prioritiseVisible()
{
    // the rows on screen first, then the ones after them, then the rest
    std::lock_guard lock(infoQueue_->mutex);
    auto& pending = infoQueue_->pending;
    if (pending.empty() || entries_.empty())
        return;
    const auto [first, last] = list_.visibleRange();
    std::vector<std::string> order;
    for (int i = std::max(0, first - 2); i <= std::min<int>(last + 6, static_cast<int>(entries_.size()) - 1); ++i)
        order.push_back(entries_[i].item.source.path);
    std::stable_partition(pending.begin(), pending.end(), [&](const library::Source& source) {
        return std::find(order.begin(), order.end(), source.path) != order.end();
    });
}

void LibraryPage::onInfo(const library::Source& source, Expected<library::Info> info)
{
    Entry* entry = find(keyOf(source.path));
    if (!entry)
        return;
    if (info)
    {
        entry->item.info = std::move(*info);
    }
    else
    {
        // unreadable: shown by its file name, and not looked at again until the next scan
        log::writeError(source.path.c_str(), info.error());
        entry->item.info = library::Info{};
    }
    if (entry->item.info->icon)
        atlas_.put(entry->key, entry->item.info->icon->data());
    applyInstalled(*entry);
    saveCacheLater();

    // once everything is read, a list nobody has touched yet takes its proper order
    std::lock_guard lock(infoQueue_->mutex);
    if (infoQueue_->pending.empty() && !interacted_)
        resort(focusedInstalled_);
}

void LibraryPage::saveCacheLater()
{
    // a burst of new infos is written once
    cacheSaveIn_ = 2.0f;
}

void LibraryPage::rebuildIndex()
{
    index_.clear();
    for (size_t i = 0; i < entries_.size(); ++i)
        index_[entries_[i].key] = i;
}

void LibraryPage::resort(bool keepSelection)
{
    const uint64_t selectedKey = keepSelection && selectedEntry() ? selectedEntry()->key : 0;
    const library::Sort order = kind_ == library::Kind::Theme ? App::get().config().themeSort
                                                              : App::get().config().splashSort;
    std::vector<library::Item> items;
    items.reserve(entries_.size());
    // sort the items, keeping each entry's state with it
    std::stable_sort(entries_.begin(), entries_.end(), [&](const Entry& a, const Entry& b) {
        std::vector<library::Item> pair{a.item, b.item};
        library::sort(pair, order);
        return pair.front().source.path == a.item.source.path && a.item.source.path != b.item.source.path;
    });
    rebuildIndex();
    if (selectedKey)
        if (auto it = index_.find(selectedKey); it != index_.end())
            list_.select(static_cast<int>(it->second), false);
}

LibraryPage::Entry* LibraryPage::find(uint64_t key)
{
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second];
}

LibraryPage::Entry* LibraryPage::selectedEntry()
{
    const int index = list_.selected();
    return index >= 0 && index < static_cast<int>(entries_.size()) ? &entries_[index] : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// What's installed

void LibraryPage::refreshInstalled()
{
    if (kind_ == library::Kind::Theme)
    {
        HomeStorage& storage = App::get().homeStorage();
        if (!storage.ready)
            return; // tick() asks again once it is
        Archive* theme = storage.theme.get();
        Archive* home = storage.home.get();
        if (!theme || !home)
            return;
        jobs::async(
            Lane::Io, [theme, home](const CancelToken&) { return themes::readCurrent(*theme, *home); },
            [this, token = token_](Expected<themes::Current> current) {
                if (token.cancelled() || !current)
                    return;
                installedBodies_.clear();
                for (const themes::Fingerprint& body : current->bodies)
                    installedBodies_.push_back({body.size, body.crc});
                installedShuffle_ = current->shuffle;
                for (Entry& entry : entries_)
                    applyInstalled(entry);
            });
    }
    else
    {
        jobs::async(
            Lane::Io, [](const CancelToken&) { return splashes::readCurrent(App::get().sd()); },
            [this, token = token_](Expected<splashes::Current> current) {
                if (token.cancelled() || !current)
                    return;
                installedTop_ = current->top ? std::optional(library::FileId{current->top->size, current->top->crc})
                                             : std::nullopt;
                installedBottom_ = current->bottom
                                       ? std::optional(library::FileId{current->bottom->size, current->bottom->crc})
                                       : std::nullopt;
                for (Entry& entry : entries_)
                    applyInstalled(entry);
            });
    }
}

void LibraryPage::applyInstalled(Entry& entry)
{
    entry.installed = entry.topInstalled = entry.bottomInstalled = false;
    if (!entry.item.info)
        return;
    const library::Info& info = *entry.item.info;

    // A file id with an unknown CRC (an item in a folder) whose size matches has its CRC worked out first.
    auto matches = [&](const library::FileId& mine, const library::FileId& installed, const char* name) {
        if (mine.size == 0 || mine.size != installed.size)
            return false;
        if (mine.crc != 0)
            return mine.crc == installed.crc;
        if (!entry.crcPending)
        {
            entry.crcPending = true;
            const uint64_t key = entry.key;
            const library::Source source = entry.item.source;
            const std::string file = name;
            jobs::async(
                Lane::Io, [source, file](const CancelToken&) { return library::computeCrc(App::get().sd(), source, file); },
                [this, key, file, token = token_](Expected<uint32_t> crc) {
                    if (token.cancelled())
                        return;
                    Entry* found = find(key);
                    if (!found || !found->item.info || !crc)
                        return;
                    found->crcPending = false;
                    library::Info& info = *found->item.info;
                    if (file == kBodyName)
                        info.body.crc = *crc;
                    else if (file == splashes::kTopName)
                        info.top.crc = *crc;
                    else
                        info.bottom.crc = *crc;
                    applyInstalled(*found);
                    saveCacheLater();
                });
        }
        return false;
    };

    if (kind_ == library::Kind::Theme)
    {
        for (const library::FileId& installed : installedBodies_)
            entry.installed = entry.installed || matches(info.body, installed, kBodyName);
    }
    else
    {
        entry.topInstalled = installedTop_ && matches(info.top, *installedTop_, splashes::kTopName);
        entry.bottomInstalled = installedBottom_ && matches(info.bottom, *installedBottom_, splashes::kBottomName);
        // installed when every screen it has is the one Luma shows
        entry.installed = (info.top.size == 0 || entry.topInstalled) &&
                          (info.bottom.size == 0 || entry.bottomInstalled) && (entry.topInstalled || entry.bottomInstalled);
    }
    if (statusOf(entry))
        installedFound_ = true;
}

void LibraryPage::focusInstalled()
{
    // the list opens on what the console uses now, until something else is chosen
    if (interacted_ || focusedInstalled_)
        return;
    for (size_t i = 0; i < entries_.size(); ++i)
    {
        if (statusOf(entries_[i]))
        {
            LOG_INFO("the list opens on %s (%u of %u)", entries_[i].item.displayName().c_str(),
                     static_cast<unsigned>(i), static_cast<unsigned>(entries_.size()));
            list_.select(static_cast<int>(i), false);
            focusedInstalled_ = true;
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

void LibraryPage::tick(float dt)
{
    if (installedFound_)
    {
        installedFound_ = false;
        focusInstalled();
    }
    // the preview fades in also under a menu that opened before it came
    if (const Entry* entry = selectedEntry(); entry && state_ == State::Ready)
    {
        const PreviewPictures* pictures = previews_.get(entry->key);
        fade_.update(entry->key, pictures && pictures->top, dt);
    }

    // something was downloaded into the folder: read it again
    const auto folder = kind_ == library::Kind::Theme ? App::Folder::Themes : App::Folder::Splashes;
    if (state_ == State::Ready && App::get().folderGeneration(folder) != generation_)
        load();

    if (cacheSaveIn_ > 0)
    {
        cacheSaveIn_ -= dt;
        if (cacheSaveIn_ <= 0)
        {
            const auto items = std::make_shared<std::vector<library::Item>>();
            for (const Entry& entry : entries_)
                items->push_back(entry.item);
            const library::Kind kind = kind_;
            jobs::run(Lane::Io, token_, [items, kind](const CancelToken&) {
                if (auto saved = library::saveCache(App::get().sd(), cachePath(kind), kind, *items); !saved)
                    log::writeError("library cache", saved.error());
            });
        }
    }
    // the HOME Menu's storage opens in the background at start-up
    static bool themesChecked = false;
    if (kind_ == library::Kind::Theme && !themesChecked && App::get().homeStorage().ready)
    {
        themesChecked = true;
        refreshInstalled();
    }
}

void LibraryPage::update(ui::Input& input, float dt)
{
    if (state_ != State::Ready)
        return;

    list_.setBounds(0, metric::kTabBarHeight, metric::kBottomWidth,
                    metric::kHeight - metric::kTabBarHeight - metric::kHintBarHeight);
    const ui::ListView::Event event = list_.update(input, dt);
    if (event != ui::ListView::Event::None)
        interacted_ = true;
    if (const auto visible = list_.visibleRange(); visible != visible_)
    {
        visible_ = visible;
        prioritiseVisible();
    }

    Entry* entry = selectedEntry();
    if (entry)
    {
        // the preview of what's selected, once the selection rests a moment
        const library::Source source = entry->item.source;
        const library::Kind kind = kind_;
        if (list_.settled())
            previews_.want(entry->key, [source, kind](const CancelToken& token) {
                return readPreview(source, kind, token);
            });
        // icons scrolled back into view after leaving the atlas
        const auto [first, last] = list_.visibleRange();
        for (int i = first; i <= last; ++i)
        {
            Entry& visible = entries_[i];
            if (visible.item.info && visible.item.info->icon && !atlas_.has(visible.key))
                atlas_.put(visible.key, visible.item.info->icon->data());
        }
    }

    if (event == ui::ListView::Event::Activated && entry)
    {
        openActions();
        return;
    }
    if (!entry)
        return;

    if (input.pressed(KEY_Y))
        openPreview(*entry);
    else if (kind_ == library::Kind::Theme && input.pressed(KEY_X))
        toggleShuffle(*entry);
    else if (kind_ == library::Kind::Theme && !shuffle_.empty() && input.pressed(KEY_SELECT))
        installShuffle();
    else if (!shuffle_.empty() && input.pressed(KEY_B))
    {
        shuffle_.clear();
        ui::toast(tr("Shuffle cleared"));
    }
}

std::vector<ui::Hint> LibraryPage::hints() const
{
    std::vector<ui::Hint> hints;
    if (state_ != State::Ready || entries_.empty())
        return hints;
    if (kind_ == library::Kind::Theme && !shuffle_.empty())
    {
        // picking themes for a shuffle: only what that takes
        auto* self = const_cast<LibraryPage*>(this);
        const Entry* entry = self->selectedEntry();
        const bool picked = entry && std::any_of(shuffle_.begin(), shuffle_.end(),
                                                 [&](const ShufflePick& pick) { return pick.key == entry->key; });
        hints.push_back({KEY_X, picked ? tr("Unpick") : tr("Shuffle")});
        hints.push_back({KEY_SELECT, trf("Install {}", std::to_string(shuffle_.size()))});
        hints.push_back({KEY_B, tr("Clear")});
        return hints;
    }
    hints.push_back({KEY_A, tr("Install")});
    hints.push_back({KEY_Y, tr("Preview")});
    if (kind_ == library::Kind::Theme)
        hints.push_back({KEY_X, tr("Shuffle")});
    return hints;
}

std::shared_ptr<Picture> LibraryPage::backdrop() const
{
    auto* self = const_cast<LibraryPage*>(this);
    const Entry* entry = self->selectedEntry();
    if (!entry)
        return nullptr;
    const PreviewPictures* pictures = self->previews_.get(entry->key);
    return pictures ? pictures->blur : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

float LibraryPage::topCover() const
{
    return state_ == State::Ready ? fade_.alpha() : 0;
}

void LibraryPage::drawTop()
{
    const Entry* entry = selectedEntry();
    if (state_ != State::Ready || !entry)
        return;
    // the details until the preview is there, then the top screen as the HOME Menu would show it (the name is on the
    // selected row)
    if (fade_.alpha() < 1)
        drawDetails(*entry);
    const PreviewPictures* pictures = previews_.get(entry->key);
    if (pictures && pictures->top && fade_.alpha() > 0)
        pictures->top->draw(0, 0, metric::kTopWidth, metric::kHeight, fade_.alpha());
}

const char* LibraryPage::statusOf(const Entry& entry) const
{
    if (kind_ == library::Kind::Theme)
        return entry.installed ? (installedShuffle_ ? "In shuffle" : "In use") : nullptr;
    if (entry.installed)
        return "In use";
    if (entry.topInstalled)
        return "Top in use";
    if (entry.bottomInstalled)
        return "Bottom in use";
    return nullptr;
}

void LibraryPage::drawDetails(const Entry& entry)
{
    // without a preview: the item's icon, name, author and description
    const library::Item& item = entry.item;
    constexpr float kLeft = 24;
    constexpr float kIcon = IconAtlas::kIconSize;
    const float textWidth = metric::kTopWidth - 2 * kLeft;
    const std::string description = item.info ? item.info->description : item.source.fileName;
    const auto lines = wrap(description, textWidth, metric::kTextNormal, 4);
    const float blockHeight =
        kIcon + (lines.empty() ? 0 : 16 + static_cast<float>(lines.size()) * lineHeight(metric::kTextNormal));
    // between the status bar and the facts along the bottom
    const float top = std::round(metric::kStatusBarHeight + (metric::kHeight - 44 - metric::kStatusBarHeight -
                                                              blockHeight) / 2);
    if (!atlas_.draw(entry.key, kLeft, top, kIcon))
    {
        roundedRect(kLeft, top, kIcon, kIcon, metric::kRadius, color::kSurfaceRaised);
        icon(kind_ == library::Kind::Theme ? Icon::Theme : Icon::Splash, kLeft + (kIcon - metric::kIcon) / 2,
             top + (kIcon - metric::kIcon) / 2, metric::kIcon, color::kTextFaint);
    }
    const float textX = kLeft + kIcon + 14;
    const float width = metric::kTopWidth - textX - kLeft;
    const float middle = top + kIcon / 2;
    const std::string author = item.author();
    drawText(item.displayName(), textX, textTop(author.empty() ? middle : middle - 9, metric::kTextLarge),
             metric::kTextLarge, color::kText, Align::Left, width);
    if (!author.empty())
        drawText(author, textX, textTop(middle + 11, metric::kTextNormal), metric::kTextNormal, color::kTextDim,
                 Align::Left, width);
    drawWrapped(description, kLeft, top + kIcon + 16, textWidth, metric::kTextNormal,
                item.info ? color::kTextDim : color::kTextFaint, 4);

    // what it is, along the bottom
    const float factsMiddle = metric::kHeight - 22;
    float x = kLeft;
    auto fact = [&](Icon which, const std::string& text, Color tint) {
        icon(which, x, std::round(factsMiddle - metric::kIcon / 2), metric::kIcon, tint);
        x += metric::kIcon + 4;
        x += drawText(text, x, textTop(factsMiddle, metric::kTextSmall), metric::kTextSmall, tint) + 16;
    };
    if (const char* status = statusOf(entry))
        fact(Icon::Check, tr(status), accent().light);
    for (size_t i = 0; i < shuffle_.size(); ++i)
        if (shuffle_[i].key == entry.key)
            fact(Icon::Shuffle, trf("Shuffle pick {}", std::to_string(i + 1)), accent().light);
    if (item.info && item.info->hasMusic)
        fact(Icon::Music, tr("Has music"), color::kTextDim);
}

void LibraryPage::drawBottom(float top, float bottom)
{
    if (state_ == State::Loading || state_ == State::NotLoaded)
    {
        spinner(metric::kBottomWidth / 2, (top + bottom) / 2, 14, accent().light);
        return;
    }
    if (state_ == State::Failed || entries_.empty())
    {
        drawEmpty(top, bottom);
        return;
    }
    list_.draw([this](int index, float x, float y, float w, float h, bool selected) {
        drawRow(index, x, y, w, h, selected);
    });
}

void LibraryPage::drawRow(int index, float x, float y, float w, float h, bool selected)
{
    const Entry& entry = entries_[index];
    const library::Item& item = entry.item;
    if (selected)
        row::highlight(x, y, w, h, accent().solid);
    const float iconTop = row::iconTop(y, h);
    if (!atlas_.draw(entry.key, x + row::kIconX, iconTop, row::kIconSize))
        row::placeholder(x, y, h, kind_ == library::Kind::Theme ? Icon::Theme : Icon::Splash);

    // picked for a shuffle: its place in it, on the icon's corner
    const ShufflePick* pick = nullptr;
    for (size_t i = 0; i < shuffle_.size(); ++i)
    {
        if (shuffle_[i].key != entry.key)
            continue;
        pick = &shuffle_[i];
        const std::string number = std::to_string(i + 1);
        const float width = std::max(16.0f, std::round(textWidth(number, metric::kTextSmall) + 8));
        const float right = x + row::kIconX + row::kIconSize + 3;
        const float top = iconTop - 3;
        roundedRect(right - width, top, width, 16, 8, accent().solid);
        drawText(number, right - width / 2, textTop(top + 8, metric::kTextSmall), metric::kTextSmall,
                 color::kOnAccent, Align::Center);
    }

    float right = x + w - row::kRight;
    if (const char* status = statusOf(entry))
        right = row::status(Icon::Check, tr(status), accent().light, right, y, h);
    if (kind_ == library::Kind::Theme && item.info && item.info->hasMusic)
        right = row::mark(pick && !pick->music ? Icon::NoMusic : Icon::Music, color::kTextFaint, right, y, h);

    const bool usable = !item.info || item.info->usable(kind_);
    std::string detail;
    if (!usable)
        detail = kind_ == library::Kind::Theme ? tr("Not a theme: no body_LZ.bin") : tr("Not a splash screen");
    else if (const std::string author = item.author(); !author.empty())
        detail = author;
    else
        detail = item.source.fileName;
    row::lines(item.displayName(), detail, x, y, h, right, !usable);
}

void LibraryPage::drawEmpty(float top, float bottom)
{
    const bool themes = kind_ == library::Kind::Theme;
    if (state_ == State::Failed)
    {
        drawEmptyState(Icon::Error, color::kDanger, tr("The SD card can't be read"), error_, top, bottom);
        return;
    }
    drawEmptyState(themes ? Icon::Theme : Icon::Splash, accent().light,
                   themes ? tr("No themes yet") : tr("No splash screens yet"),
                   themes ? tr("Find some on the Themezer tab, or copy them to /Themes on your SD card.")
                          : tr("Find some on the Themezer tab, or copy them to /Splashes on your SD card."),
                   top, bottom);
}

// ---------------------------------------------------------------------------------------------------------------------
// Actions

void LibraryPage::openActions()
{
    Entry* entry = selectedEntry();
    if (!entry)
        return;
    const uint64_t key = entry->key;
    const library::Info* info = entry->item.info ? &*entry->item.info : nullptr;
    std::vector<ui::Action> actions;

    if (kind_ == library::Kind::Theme)
    {
        const bool music = info && info->hasMusic;
        const bool usable = !info || info->usable(kind_);
        actions.push_back({Icon::Install, tr("Install"), music ? tr("With its music") : tr("It has no music"), usable,
                           false, [this, key] {
                               if (Entry* e = find(key))
                                   install(*e, kFull);
                           }});
        if (music)
        {
            actions.push_back({Icon::NoMusic, tr("Install without music"), {}, usable, false, [this, key] {
                                   if (Entry* e = find(key))
                                       install(*e, kNoMusic);
                               }});
            const themes::Current& current = App::get().homeStorage().atStart;
            const bool customInstalled = !installedBodies_.empty() || current.hasCustomTheme();
            actions.push_back({Icon::MusicOnly, tr("Use only its music"),
                               customInstalled ? tr("With the theme you have now") : tr("Install a theme first"),
                               customInstalled, false, [this, key] {
                                   if (Entry* e = find(key))
                                       install(*e, kMusicOnly);
                               }});
        }
        const bool picked = std::any_of(shuffle_.begin(), shuffle_.end(), [&](const auto& p) { return p.key == key; });
        actions.push_back({Icon::Shuffle, picked ? tr("Change in shuffle") : tr("Add to shuffle"),
                           trf("{} of {} chosen", std::to_string(shuffle_.size()), std::to_string(kMaxShuffle)),
                           usable, false, [this, key] {
                               if (Entry* e = find(key))
                                   toggleShuffle(*e);
                           }});
        if (shuffle_.size() >= 2)
            actions.push_back({Icon::Shuffle, trf("Install the shuffle of {}", std::to_string(shuffle_.size())), {},
                               true, false, [this] { installShuffle(); }});
    }
    else
    {
        const bool top = info && info->top.size != 0;
        const bool bottom = info && info->bottom.size != 0;
        const std::string screens = top && bottom ? tr("Both screens") : (top ? tr("Top screen") : tr("Bottom screen"));
        actions.push_back({Icon::Screens, tr("Install"), screens, top || bottom || !info, false, [this, key] {
                               if (Entry* e = find(key))
                                   installSplash(*e, static_cast<int>(splashes::Screens::Both));
                           }});
        if (top && bottom)
        {
            actions.push_back({Icon::ScreenTop, tr("Install the top screen only"), {}, true, false, [this, key] {
                                   if (Entry* e = find(key))
                                       installSplash(*e, static_cast<int>(splashes::Screens::Top));
                               }});
            actions.push_back({Icon::ScreenBottom, tr("Install the bottom screen only"), {}, true, false, [this, key] {
                                   if (Entry* e = find(key))
                                       installSplash(*e, static_cast<int>(splashes::Screens::Bottom));
                               }});
        }
    }
    actions.push_back({Icon::Eye, tr("Preview"), {}, true, false, [this, key] {
                           if (Entry* e = find(key))
                               openPreview(*e);
                       }});
    actions.push_back({Icon::Trash, tr("Delete from the SD card"), {}, true, true, [this, key] {
                           if (Entry* e = find(key))
                               confirmDelete(*e);
                       }});
    App::get().scenes().emplace<ui::ActionSheet>(entry->item.displayName(), std::move(actions), accent().light);
}

void LibraryPage::install(Entry& entry, int mode)
{
    const uint64_t key = entry.key;
    installTheme(entry.item.source, entry.item.displayName(), static_cast<ThemeMode>(mode),
                 [this, key, mode, token = token_](bool installed, library::FileId body) {
                     if (token.cancelled() || !installed)
                         return;
                     if (mode != kMusicOnly)
                         installedBodies_ = {body};
                     refreshInstalled();
                     (void)key;
                 });
}

void LibraryPage::installSplash(Entry& entry, int screens)
{
    scenes::installSplash(entry.item.source, entry.item.displayName(), static_cast<splashes::Screens>(screens),
                          [this, token = token_](bool installed) {
                              if (!token.cancelled() && installed)
                                  refreshInstalled();
                          });
}

void LibraryPage::toggleShuffle(Entry& entry)
{
    if (entry.item.info && !entry.item.info->usable(kind_))
        return;
    auto it = std::find_if(shuffle_.begin(), shuffle_.end(), [&](const auto& p) { return p.key == entry.key; });
    const bool music = entry.item.info && entry.item.info->hasMusic;
    if (it == shuffle_.end())
    {
        if (static_cast<int>(shuffle_.size()) >= kMaxShuffle)
        {
            ui::toast(trf("A shuffle has at most {} themes", std::to_string(kMaxShuffle)), ui::ToastKind::Warning);
            return;
        }
        shuffle_.push_back({entry.key, true});
    }
    else if (it->music && music)
    {
        // a second press keeps it in the shuffle without its music, a third takes it out
        it->music = false;
    }
    else
    {
        shuffle_.erase(it);
    }
}

void LibraryPage::installShuffle()
{
    if (shuffle_.size() < 2)
    {
        ui::toast(tr("Choose at least two themes for a shuffle (X)"), ui::ToastKind::Warning);
        return;
    }
    HomeStorage& storage = App::get().homeStorage();
    if (!storage.ready || storage.error || !storage.theme || !storage.home)
    {
        showError(themeStorageError(storage.error.value_or(Error{Errc::NotFound})));
        return;
    }
    struct Pick
    {
        library::Source source;
        bool music;
    };
    auto picks = std::make_shared<std::vector<Pick>>();
    for (const ShufflePick& pick : shuffle_)
        if (Entry* entry = find(pick.key))
            picks->push_back({entry->item.source, pick.music});
    Archive* theme = storage.theme.get();
    Archive* home = storage.home.get();

    App::get().scenes().push(ui::ProgressOverlay::start<Expected<std::vector<library::FileId>>>(
        {trf("Installing a shuffle of {} themes", std::to_string(picks->size())), Lane::Io, false, true,
         accent().solid},
        [picks, theme, home](ui::TaskProgress& progress,
                             const CancelToken&) -> Expected<std::vector<library::FileId>> {
            std::vector<library::FileId> bodies;
            FileSystem& sd = App::get().sd();
            TZ_CHECK(themes::installShuffle(
                *theme, *home, static_cast<int>(picks->size()),
                [&](int index) -> Expected<themes::ThemeFiles> {
                    const Pick& pick = (*picks)[index];
                    progress.setStatus(path::stem(pick.source.fileName).data());
                    const Package package = TZ_TRY(Package::open(sd, pick.source.path, pick.source.zip));
                    themes::ThemeFiles files;
                    files.body = TZ_TRY(package.read(kBodyName, home::kBodyCacheSize + 1));
                    if (pick.music && package.has(kMusicName))
                        files.bgm = TZ_TRY(package.read(kMusicName, home::kBgmCacheSize + 1));
                    bodies.push_back({static_cast<uint32_t>(files.body.size()), crc32(files.body)});
                    return files;
                },
                [&](int done, int total) {
                    progress.set(done, total);
                    return true;
                }));
            return bodies;
        },
        [this, token = token_](Expected<std::vector<library::FileId>> result) {
            if (token.cancelled())
                return;
            if (!result)
            {
                showError(installError(result.error(), tr("the shuffle")));
                return;
            }
            installedBodies_ = *result;
            shuffle_.clear();
            for (Entry& entry : entries_)
                applyInstalled(entry);
            App::get().themeChanged(true);
            ui::toast(tr("The shuffle is installed"), ui::ToastKind::Success);
        }));
}

void LibraryPage::confirmDelete(Entry& entry)
{
    const library::Source source = entry.item.source;
    const uint64_t key = entry.key;
    const std::string name = entry.item.displayName();
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Trash, color::kDanger, trf("Delete {}?", name),
        trf("{} is removed from your SD card. What's installed stays installed.", source.fileName),
        std::vector<ui::DialogButton>{
            {tr("Keep"), nullptr},
            {tr("Delete"),
             [this, source, key, name] {
                 jobs::async(
                     Lane::Io,
                     [source](const CancelToken&) {
                         FileSystem& sd = App::get().sd();
                         return source.zip ? sd.removeFile(source.path) : sd.removeDirectory(source.path, true);
                     },
                     [this, key, name, token = token_](Expected<void> removed) {
                         if (token.cancelled())
                             return;
                         if (!removed)
                         {
                             showError(describe(removed.error()));
                             return;
                         }
                         auto it = index_.find(key);
                         if (it != index_.end())
                         {
                             entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(it->second));
                             std::erase_if(shuffle_, [&](const auto& p) { return p.key == key; });
                             rebuildIndex();
                             list_.setCount(static_cast<int>(entries_.size()));
                             saveCacheLater();
                         }
                         ui::toast(trf("{} is deleted", name), ui::ToastKind::Success);
                     });
             },
             true},
        });
}

void LibraryPage::openPreview(Entry& entry)
{
    const library::Source source = entry.item.source;
    const library::Kind kind = kind_;
    const bool music = kind_ == library::Kind::Theme && entry.item.info && entry.item.info->hasMusic;
    PreviewSource preview;
    preview.title = entry.item.displayName();
    preview.key = entry.key;
    preview.previews = &previews_;
    preview.loader = [source, kind](const CancelToken& token) { return readPreview(source, kind, token); };
    if (music)
    {
        preview.music = [source]() -> Expected<std::unique_ptr<StreamReader>> {
            const Package package = TZ_TRY(Package::open(App::get().sd(), source.path, source.zip));
            return package.stream(kMusicName);
        };
    }
    App::get().scenes().push(makePreviewScene(std::move(preview)));
}

// ---------------------------------------------------------------------------------------------------------------------
// The menu's actions for this page

std::vector<ui::Action> LibraryPage::menuActions()
{
    std::vector<ui::Action> actions;
    const library::Sort current =
        kind_ == library::Kind::Theme ? App::get().config().themeSort : App::get().config().splashSort;
    const library::Sort next = current == library::Sort::Name
                                   ? library::Sort::Author
                                   : (current == library::Sort::Author ? library::Sort::FileName : library::Sort::Name);
    const char* nextName = next == library::Sort::Name ? "Sort by name"
                                                       : (next == library::Sort::Author ? "Sort by author"
                                                                                        : "Sort by file name");
    actions.push_back({Icon::Sort, tr(nextName), {}, state_ == State::Ready, false, [this, next] {
                           (kind_ == library::Kind::Theme ? App::get().config().themeSort
                                                          : App::get().config().splashSort) = next;
                           resort(true);
                       }});
    if (kind_ == library::Kind::Theme)
    {
        actions.push_back({Icon::Save, tr("Save the current theme to the SD card"), tr("Into /Themes"), true, false,
                           [this] { saveInstalled(); }});
        actions.push_back({Icon::Restore, tr("Go back to the default theme"), {}, true, false, [this] { resetTheme(); }});
    }
    else
    {
        actions.push_back({Icon::Save, tr("Save the current splash to the SD card"), tr("Into /Splashes"), true, false,
                           [this] { saveInstalled(); }});
        actions.push_back({Icon::Trash, tr("Remove the splash"), tr("Luma3DS starts without one"), true, true,
                           [this] { removeSplash(); }});
    }
    actions.push_back({Icon::Refresh, tr("Look at the SD card again"), {}, true, false, [this] { load(); }});
    return actions;
}

void LibraryPage::resetTheme()
{
    HomeStorage& storage = App::get().homeStorage();
    if (!storage.ready || storage.error || !storage.theme || !storage.home)
    {
        showError(themeStorageError(storage.error.value_or(Error{Errc::NotFound})));
        return;
    }
    Archive* theme = storage.theme.get();
    Archive* home = storage.home.get();
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Restore, accent().light, tr("Go back to the default theme?"),
        tr("The HOME Menu goes back to its own look. Your themes stay on the SD card."),
        std::vector<ui::DialogButton>{
            {tr("Cancel"), nullptr},
            {tr("Go back"),
             [this, theme, home] {
                 App::get().scenes().push(ui::ProgressOverlay::start<Expected<void>>(
                     {tr("Going back to the default theme"), Lane::Io, false, true, accent().solid},
                     [theme, home](ui::TaskProgress&, const CancelToken&) {
                         return themes::resetToDefault(*theme, *home);
                     },
                     [this, token = token_](Expected<void> result) {
                         if (token.cancelled())
                             return;
                         if (!result)
                         {
                             showError(describe(result.error()));
                             return;
                         }
                         installedBodies_.clear();
                         for (Entry& entry : entries_)
                             applyInstalled(entry);
                         App::get().themeChanged(false);
                         ui::toast(tr("The default theme is back"), ui::ToastKind::Success);
                     }));
             }},
        });
}

void LibraryPage::removeSplash()
{
    App::get().scenes().emplace<ui::Dialog>(
        Icon::Trash, color::kDanger, tr("Remove the splash?"),
        tr("Luma3DS will start without a splash screen. The splashes on your SD card stay."),
        std::vector<ui::DialogButton>{
            {tr("Keep"), nullptr},
            {tr("Remove"),
             [this] {
                 jobs::async(
                     Lane::Io, [](const CancelToken&) { return splashes::uninstall(App::get().sd()); },
                     [this, token = token_](Expected<void> result) {
                         if (token.cancelled())
                             return;
                         if (!result)
                         {
                             showError(describe(result.error()));
                             return;
                         }
                         installedTop_.reset();
                         installedBottom_.reset();
                         for (Entry& entry : entries_)
                             applyInstalled(entry);
                         ui::toast(tr("The splash is removed"), ui::ToastKind::Success);
                     });
             },
             true},
        });
}

void LibraryPage::saveInstalled()
{
    const library::Kind kind = kind_;
    HomeStorage& storage = App::get().homeStorage();
    Archive* theme = storage.theme.get();
    Archive* home = storage.home.get();
    if (kind == library::Kind::Theme && (!theme || !home))
    {
        showError(themeStorageError(storage.error.value_or(Error{Errc::NotFound})));
        return;
    }
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<std::string>>(
        {kind == library::Kind::Theme ? tr("Saving the current theme") : tr("Saving the current splash"), Lane::Io,
         false, false, accent().solid},
        [kind, theme, home](ui::TaskProgress&, const CancelToken&) -> Expected<std::string> {
            FileSystem& sd = App::get().sd();
            // a new folder, named after when it was saved
            const time_t now = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M", gmtime(&now));
            const std::string folder = path::join(library::root(kind), std::string(kind == library::Kind::Theme
                                                                                        ? "Saved theme "
                                                                                        : "Saved splash ") +
                                                                            stamp);
            TZ_CHECK(sd.createDirectories(folder));
            if (kind == library::Kind::Theme)
            {
                const themes::ThemeFiles files = TZ_TRY(themes::readInstalled(*theme, *home));
                TZ_CHECK(sd.writeFile(path::join(folder, kBodyName), files.body));
                if (!files.bgm.empty())
                    TZ_CHECK(sd.writeFile(path::join(folder, kMusicName), files.bgm));
            }
            else
            {
                const splashes::Files files = TZ_TRY(splashes::readInstalled(sd));
                if (!files.top.empty())
                    TZ_CHECK(sd.writeFile(path::join(folder, splashes::kTopName), files.top));
                if (!files.bottom.empty())
                    TZ_CHECK(sd.writeFile(path::join(folder, splashes::kBottomName), files.bottom));
            }
            return folder;
        },
        [this, kind, token = token_](Expected<std::string> folder) {
            if (token.cancelled())
                return;
            if (!folder)
            {
                if (folder.error().code == Errc::NotFound)
                    showError({kind == library::Kind::Theme ? tr("No theme to save") : tr("No splash to save"),
                               kind == library::Kind::Theme
                                   ? tr("The HOME Menu is on its default look or a basic colour, which aren't files.")
                                   : tr("Luma3DS has no splash screen installed.")});
                else
                    showError(describe(folder.error()));
                return;
            }
            ui::toast(trf("Saved in {}", *folder), ui::ToastKind::Success);
            load();
        }));
}

} // namespace tz::scenes
