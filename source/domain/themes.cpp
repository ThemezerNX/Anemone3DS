// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "domain/themes.hpp"

#include "core/bcstm.hpp"
#include "core/lz11.hpp"
#include "core/zip.hpp"

#include <algorithm>

namespace tz::themes
{

namespace
{

constexpr size_t kChunk = 0x10000;
constexpr uint64_t kSaveDataMaxSize = 0x10000;
// Clearing the start of a music cache is enough to leave it without music: the HOME Menu checks the BCSTM header.
constexpr uint64_t kBgmClearSize = 0x10000;
// The largest decompressed body accepted when the music flag has to be changed.
constexpr size_t kMaxPlainBody = 8 * 1024 * 1024;

const home::ThemeDescriptor kCustomTheme{0xFF, 0, home::type::kCustom, 0, 0};

Expected<home::ThemeManage> readManage(FileSystem& themeExt)
{
    auto data = TZ_TRY(themeExt.readFile(home::kManagePath, home::kManageSize));
    return home::ThemeManage::parse(data);
}

Expected<void> writeWhole(FileSystem& fs, const char* path, ByteSpan data)
{
    auto file = TZ_TRY(fs.open(path, OpenMode::ReadWrite));
    TZ_CHECK(file->writeAt(0, data));
    return file->flush();
}

Expected<home::SaveData> readSaveData(FileSystem& homeExt)
{
    auto data = TZ_TRY(homeExt.readFile(home::kSaveDataPath, kSaveDataMaxSize));
    return home::SaveData::parse(std::move(data));
}

Expected<void> writeZeros(File& file, uint64_t offset, uint64_t length)
{
    static const uint8_t zeros[kChunk] = {};
    while (length > 0)
    {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(length, kChunk));
        TZ_CHECK(file.writeAt(offset, ByteSpan(zeros, chunk)));
        offset += chunk;
        length -= chunk;
    }
    return {};
}

// Opens one of the fixed-size cache files, creating it when it's missing. The HOME Menu only takes them at their
// exact size, so one of another size is made again.
Expected<std::unique_ptr<File>> openCache(FileSystem& themeExt, const std::string& path, uint64_t size)
{
    auto entry = themeExt.stat(path);
    if (entry && entry->size != size)
    {
        TZ_CHECK(themeExt.removeFile(path));
        entry = fail(Errc::NotFound);
    }
    if (!entry)
    {
        if (entry.error().code != Errc::NotFound)
            return fail(entry.error());
        TZ_CHECK(themeExt.createFile(path, size));
    }
    return themeExt.open(path, OpenMode::ReadWrite);
}

// Every theme manager so far also wrote a Theme Shop entry (index 0xFF) at the top of the downloaded-theme list.
// The HOME Menu doesn't need it to show a custom theme as far as is known, and writing it over a real entry moves
// that theme in the HOME Menu's list; so it's only written where the list is empty, which keeps a fresh console
// exactly where every earlier tool left it.
void markCustomTheme(home::ThemeManage& manage)
{
    if (manage.shopTheme(0) == home::ThemeDescriptor{})
        manage.setShopTheme(0, home::ThemeDescriptor{0xFF, 0, home::type::kShop, 0, 0});
}

// Before cache files are overwritten that the HOME Menu may be reading, it is pointed at its default theme. A
// failure halfway then leaves the default theme instead of a cache that doesn't match its sizes.
Expected<void> leaveCaches(FileSystem& homeExt, home::SaveData& save, bool shuffleCaches)
{
    const bool inUse = shuffleCaches ? save.shuffle() : (save.selected().type >= home::type::kShop && !save.shuffle());
    if (!inUse)
        return {};
    save.setSelected({});
    save.setShuffle(false);
    return writeWhole(homeExt, home::kSaveDataPath, save.bytes());
}

Fingerprint fingerprintOf(ByteSpan body)
{
    return Fingerprint{static_cast<uint32_t>(body.size()), crc32(body)};
}

} // namespace

Expected<void> validateBody(ByteSpan body)
{
    if (body.empty())
        return fail(Errc::InvalidData, "empty body");
    if (body.size() > home::kBodyCacheSize)
        return fail(Errc::TooLarge, "body of " + std::to_string(body.size()) + " bytes");
    if (lz11::decompressedSize(body) <= kBodyBgmFlagOffset)
        return fail(Errc::InvalidData, "body isn't LZ11");
    return {};
}

Expected<void> validateBgm(ByteSpan bgm)
{
    if (bgm.size() > home::kBgmCacheSize)
        return fail(Errc::TooLarge, "music of " + std::to_string(bgm.size()) + " bytes");
    if (!startsWith(bgm, "CSTM"))
        return fail(Errc::InvalidData, "music isn't a BCSTM");
    return {};
}

Expected<Current> readCurrent(FileSystem& themeExt, FileSystem& homeExt)
{
    const home::SaveData save = TZ_TRY(readSaveData(homeExt));
    Current current;
    current.selected = save.selected();
    current.shuffle = save.shuffle();

    auto manage = readManage(themeExt);
    if (!manage)
        return current; // no theme storage yet: nothing installed

    if (current.shuffle)
    {
        auto cache = themeExt.open(home::kShuffleBodyCachePath, OpenMode::Read);
        if (!cache)
            return current;
        for (int i = 0; i < home::kShuffleSlots; ++i)
        {
            if (save.shuffleTheme(i).type != home::type::kCustom)
                continue;
            const uint8_t slotByte = save.bytes()[home::SaveData::kShuffleSlotsOffset + i];
            const int slot = slotByte < home::kShuffleSlots ? slotByte : i;
            const uint32_t size = manage->shuffleBodySize(slot);
            if (size == 0 || size > home::kBodyCacheSize)
                continue;
            Bytes body(size);
            if ((*cache)->readAt(static_cast<uint64_t>(slot) * home::kBodyCacheSize, body))
                current.bodies.push_back(fingerprintOf(body));
        }
    }
    else if (current.selected.type == home::type::kCustom)
    {
        const uint32_t size = manage->bodySize();
        if (size > 0 && size <= home::kBodyCacheSize)
        {
            auto cache = themeExt.open(home::kBodyCachePath, OpenMode::Read);
            Bytes body(size);
            if (cache && (*cache)->readAt(0, body))
                current.bodies.push_back(fingerprintOf(body));
        }
    }
    return current;
}

Expected<void> ensureFiles(FileSystem& themeExt, FileSystem& homeExt)
{
    struct Required
    {
        const char* path;
        uint32_t size;
        bool missing = false;
    };
    // the caches before the manager, which is also the order they are created in
    Required files[] = {
        {home::kBodyCachePath, home::kBodyCacheSize},
        {home::kBgmCachePath, home::kBgmCacheSize},
        {home::kManagePath, home::kManageSize},
    };

    bool anyMissing = false;
    for (Required& file : files)
    {
        auto entry = themeExt.stat(file.path);
        if (entry)
        {
            if (entry->size != file.size)
                return fail(Errc::InvalidData, std::string(file.path) + " has the wrong size");
            continue;
        }
        if (entry.error().code != Errc::NotFound)
            return fail(entry.error());
        file.missing = anyMissing = true;
    }
    if (!anyMissing)
        return {};

    // Missing files can only be made empty when no cached theme can be in use.
    const home::SaveData save = TZ_TRY(readSaveData(homeExt));
    bool cacheInUse = save.selected().type >= home::type::kShop || save.shuffle();
    if (!files[2].missing)
    {
        const home::ThemeManage manage = TZ_TRY(readManage(themeExt));
        cacheInUse = cacheInUse || manage.bodySize() || manage.bgmSize();
        for (int i = 0; i < home::kShuffleSlots; ++i)
            cacheInUse = cacheInUse || manage.shuffleBodySize(i) || manage.shuffleBgmSize(i);
    }
    if (cacheInUse)
        return fail(Errc::InvalidData, "theme files are missing while a cached theme is selected");

    for (const Required& file : files)
    {
        if (!file.missing)
            continue;
        TZ_CHECK(themeExt.createFile(file.path, file.size));
        // the HOME Menu makes these files all zero; a new extdata file isn't known to be
        auto handle = TZ_TRY(themeExt.open(file.path, OpenMode::ReadWrite));
        TZ_CHECK(writeZeros(*handle, 0, file.size));
        TZ_CHECK(handle->flush());
    }
    return {};
}

Expected<void> install(FileSystem& themeExt, FileSystem& homeExt, ByteSpan body, ByteSpan bgm)
{
    TZ_CHECK(validateBody(body));
    if (!bgm.empty())
        TZ_CHECK(validateBgm(bgm));

    home::SaveData save = TZ_TRY(readSaveData(homeExt));
    home::ThemeManage manage = TZ_TRY(readManage(themeExt));
    TZ_CHECK(leaveCaches(homeExt, save, false));

    {
        auto cache = TZ_TRY(openCache(themeExt, home::kBodyCachePath, home::kBodyCacheSize));
        TZ_CHECK(cache->writeAt(0, body));
        TZ_CHECK(cache->flush());
    }
    {
        auto cache = TZ_TRY(openCache(themeExt, home::kBgmCachePath, home::kBgmCacheSize));
        if (bgm.empty())
            TZ_CHECK(writeZeros(*cache, 0, kBgmClearSize));
        else
            TZ_CHECK(cache->writeAt(0, bgm));
        TZ_CHECK(cache->flush());
    }

    manage.setBodySize(static_cast<uint32_t>(body.size()));
    manage.setBgmSize(static_cast<uint32_t>(bgm.size()));
    markCustomTheme(manage);
    TZ_CHECK(writeWhole(themeExt, home::kManagePath, manage.bytes()));

    save.setSelected(kCustomTheme);
    save.setShuffle(false);
    for (int i = 0; i < home::kShuffleSlots; ++i)
        save.setShuffleTheme(i, {});
    return writeWhole(homeExt, home::kSaveDataPath, save.bytes());
}

Expected<void> installMusic(FileSystem& themeExt, FileSystem& homeExt, ByteSpan bgm)
{
    TZ_CHECK(validateBgm(bgm));

    home::SaveData save = TZ_TRY(readSaveData(homeExt));
    home::ThemeManage manage = TZ_TRY(readManage(themeExt));
    const uint32_t bodySize = manage.bodySize();
    if (save.shuffle() || save.selected().type != home::type::kCustom || bodySize == 0 ||
        bodySize > home::kBodyCacheSize)
        return fail(Errc::NotFound, "no custom theme to add music to");

    Bytes body(bodySize);
    {
        auto cache = TZ_TRY(themeExt.open(home::kBodyCachePath, OpenMode::Read));
        TZ_CHECK(cache->readAt(0, body));
    }

    // The body says whether the theme plays music. A theme installed without music may have it off.
    Bytes plain = TZ_TRY(lz11::decompress(body, kMaxPlainBody));
    if (plain.size() <= kBodyBgmFlagOffset)
        return fail(Errc::InvalidData, "body too short");
    const bool flagChanges = plain[kBodyBgmFlagOffset] != 1;
    if (flagChanges)
    {
        plain[kBodyBgmFlagOffset] = 1;
        body = lz11::compress(plain);
        if (body.size() > home::kBodyCacheSize)
            return fail(Errc::TooLarge, "recompressed body doesn't fit");
    }

    TZ_CHECK(leaveCaches(homeExt, save, false));
    if (flagChanges)
    {
        auto cache = TZ_TRY(openCache(themeExt, home::kBodyCachePath, home::kBodyCacheSize));
        TZ_CHECK(cache->writeAt(0, body));
        TZ_CHECK(cache->flush());
        manage.setBodySize(static_cast<uint32_t>(body.size()));
    }
    {
        auto cache = TZ_TRY(openCache(themeExt, home::kBgmCachePath, home::kBgmCacheSize));
        TZ_CHECK(cache->writeAt(0, bgm));
        TZ_CHECK(cache->flush());
    }
    manage.setBgmSize(static_cast<uint32_t>(bgm.size()));
    TZ_CHECK(writeWhole(themeExt, home::kManagePath, manage.bytes()));

    save.setSelected(kCustomTheme);
    save.setShuffle(false);
    return writeWhole(homeExt, home::kSaveDataPath, save.bytes());
}

Expected<void> installShuffle(FileSystem& themeExt, FileSystem& homeExt, int count, const ShuffleLoader& load,
                              const Progress& progress)
{
    if (count < 2 || count > home::kShuffleSlots)
        return fail(Errc::InvalidData, "a shuffle has 2 to 10 themes");

    home::SaveData save = TZ_TRY(readSaveData(homeExt));
    home::ThemeManage manage = TZ_TRY(readManage(themeExt));
    TZ_CHECK(leaveCaches(homeExt, save, true));

    auto bodies = TZ_TRY(openCache(themeExt, home::kShuffleBodyCachePath, home::kShuffleBodyCacheSize));
    for (int slot = 0; slot < home::kShuffleSlots; ++slot)
    {
        if (progress && !progress(std::min(slot, count), count))
            return fail(Errc::Cancelled);

        auto music = TZ_TRY(openCache(themeExt, home::shuffleBgmCachePath(slot), home::kBgmCacheSize));
        if (slot >= count)
        {
            // unused slots only need to exist
            manage.setShuffleBodySize(slot, 0);
            manage.setShuffleBgmSize(slot, 0);
            continue;
        }

        ThemeFiles theme = TZ_TRY(load(slot));
        TZ_CHECK(validateBody(theme.body));
        if (!theme.bgm.empty())
            TZ_CHECK(validateBgm(theme.bgm));

        TZ_CHECK(bodies->writeAt(static_cast<uint64_t>(slot) * home::kBodyCacheSize, theme.body));
        if (theme.bgm.empty())
            TZ_CHECK(writeZeros(*music, 0, kBgmClearSize));
        else
            TZ_CHECK(music->writeAt(0, theme.bgm));
        TZ_CHECK(music->flush());

        manage.setShuffleBodySize(slot, static_cast<uint32_t>(theme.body.size()));
        manage.setShuffleBgmSize(slot, static_cast<uint32_t>(theme.bgm.size()));
    }
    TZ_CHECK(bodies->flush());
    if (progress)
        progress(count, count);

    markCustomTheme(manage);
    TZ_CHECK(writeWhole(themeExt, home::kManagePath, manage.bytes()));

    save.setSelected({});
    save.setShuffle(true);
    for (int i = 0; i < home::kShuffleSlots; ++i)
        save.setShuffleTheme(i, i < count ? home::ThemeDescriptor{static_cast<uint32_t>(i), 0, home::type::kCustom, 0, 0}
                                          : home::ThemeDescriptor{});
    save.resetShuffleOrder();
    return writeWhole(homeExt, home::kSaveDataPath, save.bytes());
}

Expected<void> resetToDefault(FileSystem& themeExt, FileSystem& homeExt)
{
    home::SaveData save = TZ_TRY(readSaveData(homeExt));
    save.setSelected({});
    save.setShuffle(false);
    for (int i = 0; i < home::kShuffleSlots; ++i)
        save.setShuffleTheme(i, {});
    // the selection first, so the HOME Menu stops looking at the cache before its sizes go
    TZ_CHECK(writeWhole(homeExt, home::kSaveDataPath, save.bytes()));

    auto manage = readManage(themeExt);
    if (!manage)
        return {}; // no theme storage: nothing more to reset
    manage->setBodySize(0);
    manage->setBgmSize(0);
    for (int i = 0; i < home::kShuffleSlots; ++i)
    {
        manage->setShuffleBodySize(i, 0);
        manage->setShuffleBgmSize(i, 0);
    }
    return writeWhole(themeExt, home::kManagePath, manage->bytes());
}

Expected<ThemeFiles> readInstalled(FileSystem& themeExt, FileSystem& homeExt)
{
    const home::SaveData save = TZ_TRY(readSaveData(homeExt));
    const home::ThemeManage manage = TZ_TRY(readManage(themeExt));
    const uint32_t bodySize = manage.bodySize();
    const uint32_t bgmSize = manage.bgmSize();
    // a custom theme or a cached Theme Shop theme, not a shuffle
    if (save.shuffle() || save.selected().type < home::type::kShop || bodySize == 0 ||
        bodySize > home::kBodyCacheSize || bgmSize > home::kBgmCacheSize)
        return fail(Errc::NotFound, "no single theme in the cache");

    ThemeFiles files;
    files.body.resize(bodySize);
    {
        auto cache = TZ_TRY(themeExt.open(home::kBodyCachePath, OpenMode::Read));
        TZ_CHECK(cache->readAt(0, files.body));
    }
    if (bgmSize > 0)
    {
        files.bgm.resize(bgmSize);
        auto cache = TZ_TRY(themeExt.open(home::kBgmCachePath, OpenMode::Read));
        TZ_CHECK(cache->readAt(0, files.bgm));
        if (!startsWith(files.bgm, "CSTM"))
            files.bgm.clear();
    }
    return files;
}

} // namespace tz::themes
