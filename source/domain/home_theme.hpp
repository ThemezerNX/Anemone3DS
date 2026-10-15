// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The HOME Menu's theme files, as far as a theme manager needs them.
//
// The theme extdata (0x2CC Japan, 0x2CD America, 0x2CE Europe and Australia, 0x2CF Korea) holds the cached theme:
//   ThemeManage.bin    0x800 bytes: the cache's sizes, and the list of downloaded Theme Shop themes
//   BodyCache.bin      0x150000 bytes: the theme's LZ11-compressed body
//   BgmCache.bin       0x337000 bytes: its music (a BCSTM)
//   BodyCache_rd.bin   10 * 0x150000 bytes: the bodies of a shuffle, one per slot
//   BgmCache_00..09.bin  0x337000 bytes each: the music of a shuffle
// The HOME Menu's own extdata (0x82, 0x8F, 0x98, 0xA9) holds SaveData.dat, which says which theme is selected.
//
// The field meanings come from reading the HOME Menu (EUR, version 29696) in October 2026, see
// research/theme-management in the Themezer-3DS history, and from 3dbrew and yellows8's
// 3ds_homemenu_extdatatool. Bytes whose use isn't established are kept as they are.

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

#include <array>
#include <string>

namespace tz::home
{

constexpr uint32_t kManageSize = 0x800;
constexpr uint32_t kBodyCacheSize = 0x150000;
constexpr uint32_t kBgmCacheSize = 0x337000;
constexpr int kShuffleSlots = 10;
constexpr uint32_t kShuffleBodyCacheSize = kBodyCacheSize * kShuffleSlots;

constexpr const char* kManagePath = "/ThemeManage.bin";
constexpr const char* kBodyCachePath = "/BodyCache.bin";
constexpr const char* kBgmCachePath = "/BgmCache.bin";
constexpr const char* kShuffleBodyCachePath = "/BodyCache_rd.bin";
constexpr const char* kSaveDataPath = "/SaveData.dat";

// "/BgmCache_00.bin" ... "/BgmCache_09.bin"
std::string shuffleBgmCachePath(int slot);

// Eight bytes that name a theme: in SaveData.dat for the selected theme and the shuffle, and in ThemeManage.bin for
// the downloaded Theme Shop themes.
struct ThemeDescriptor
{
    uint32_t index = 0;   // the Theme Shop content index, or the built-in colour
    uint8_t selector = 0; // which Theme Shop title it is in
    uint8_t type = 0;
    uint8_t aux6 = 0; // not interpreted by the HOME Menu as far as is known; kept
    uint8_t aux7 = 0;

    bool operator==(const ThemeDescriptor&) const = default;
};

namespace type
{
constexpr uint8_t kDefault = 0; // no theme: the HOME Menu's default look
constexpr uint8_t kBasic = 1;   // one of the five basic colours
constexpr uint8_t kShop = 2;    // a downloaded Theme Shop theme
// What every theme manager writes for a custom theme: the HOME Menu loads it from the cache without checking for a
// Theme Shop download, and keeps it after a restart.
constexpr uint8_t kCustom = 3;
} // namespace type

// The cached theme's sizes and the Theme Shop's list. The bytes not named here are kept as read.
class ThemeManage
{
public:
    // An all-zero file, as the HOME Menu makes it.
    ThemeManage() { raw_.fill(0); }
    static Expected<ThemeManage> parse(ByteSpan data);

    ByteSpan bytes() const { return raw_; }

    uint32_t bodySize() const { return readU32(raw_.data() + 0x08); }
    void setBodySize(uint32_t size) { writeU32(raw_.data() + 0x08, size); }
    uint32_t bgmSize() const { return readU32(raw_.data() + 0x0C); }
    void setBgmSize(uint32_t size) { writeU32(raw_.data() + 0x0C, size); }

    uint32_t shuffleBodySize(int slot) const { return readU32(raw_.data() + 0x338 + slot * 4); }
    void setShuffleBodySize(int slot, uint32_t size) { writeU32(raw_.data() + 0x338 + slot * 4, size); }
    uint32_t shuffleBgmSize(int slot) const { return readU32(raw_.data() + 0x360 + slot * 4); }
    void setShuffleBgmSize(int slot, uint32_t size) { writeU32(raw_.data() + 0x360 + slot * 4, size); }

    // The 100 downloaded Theme Shop themes, in their order on the HOME Menu (0x18).
    ThemeDescriptor shopTheme(int i) const;
    void setShopTheme(int i, const ThemeDescriptor& descriptor);

    static constexpr int kShopThemes = 100;

private:
    std::array<uint8_t, kManageSize> raw_;
};

// The theme part of the HOME Menu's SaveData.dat. The rest of the file is kept as read.
class SaveData
{
public:
    static constexpr size_t kSelectedOffset = 0x13B8;
    static constexpr size_t kShuffleThemesOffset = 0x13C0; // ten descriptors
    static constexpr size_t kShuffleSeedOffset = 0x1410;   // eleven bytes
    static constexpr size_t kShuffleOffset = 0x141B;       // shuffle on or off
    static constexpr size_t kShuffleSlotsOffset = 0x141C;  // ten bytes: the cache slot of each shuffle theme
    static constexpr size_t kMinimumSize = 0x1426;

    // InvalidData when the file is too short to hold the theme fields.
    static Expected<SaveData> parse(Bytes data);

    const Bytes& bytes() const { return raw_; }

    ThemeDescriptor selected() const;
    void setSelected(const ThemeDescriptor& descriptor);
    bool shuffle() const { return raw_[kShuffleOffset] != 0; }
    void setShuffle(bool on) { raw_[kShuffleOffset] = on ? 1 : 0; }
    ThemeDescriptor shuffleTheme(int i) const;
    void setShuffleTheme(int i, const ThemeDescriptor& descriptor);

    // Writes the seed and slot bytes as the theme managers before this one did: slots 0 to 9 in order.
    void resetShuffleOrder();

private:
    explicit SaveData(Bytes data) : raw_(std::move(data)) {}
    Bytes raw_;
};

ThemeDescriptor readDescriptor(const uint8_t* p);
void writeDescriptor(uint8_t* p, const ThemeDescriptor& descriptor);

} // namespace tz::home
