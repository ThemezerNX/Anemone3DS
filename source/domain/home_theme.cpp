// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "domain/home_theme.hpp"

#include <algorithm>
#include <cstdio>

namespace tz::home
{

std::string shuffleBgmCachePath(int slot)
{
    char path[24];
    std::snprintf(path, sizeof(path), "/BgmCache_%02d.bin", slot);
    return path;
}

ThemeDescriptor readDescriptor(const uint8_t* p)
{
    return ThemeDescriptor{readU32(p), p[4], p[5], p[6], p[7]};
}

void writeDescriptor(uint8_t* p, const ThemeDescriptor& descriptor)
{
    writeU32(p, descriptor.index);
    p[4] = descriptor.selector;
    p[5] = descriptor.type;
    p[6] = descriptor.aux6;
    p[7] = descriptor.aux7;
}

Expected<ThemeManage> ThemeManage::parse(ByteSpan data)
{
    // The HOME Menu requires the file to be exactly this size; a shorter one is no file it made.
    if (data.size() != kManageSize)
        return fail(Errc::InvalidData, "ThemeManage.bin is " + std::to_string(data.size()) + " bytes");
    ThemeManage manage;
    std::copy(data.begin(), data.end(), manage.raw_.begin());
    return manage;
}

ThemeDescriptor ThemeManage::shopTheme(int i) const
{
    return readDescriptor(raw_.data() + 0x18 + i * 8);
}

void ThemeManage::setShopTheme(int i, const ThemeDescriptor& descriptor)
{
    writeDescriptor(raw_.data() + 0x18 + i * 8, descriptor);
}

Expected<SaveData> SaveData::parse(Bytes data)
{
    if (data.size() < kMinimumSize)
        return fail(Errc::InvalidData, "SaveData.dat is " + std::to_string(data.size()) + " bytes");
    return SaveData(std::move(data));
}

ThemeDescriptor SaveData::selected() const
{
    return readDescriptor(raw_.data() + kSelectedOffset);
}

void SaveData::setSelected(const ThemeDescriptor& descriptor)
{
    writeDescriptor(raw_.data() + kSelectedOffset, descriptor);
}

ThemeDescriptor SaveData::shuffleTheme(int i) const
{
    return readDescriptor(raw_.data() + kShuffleThemesOffset + i * 8);
}

void SaveData::setShuffleTheme(int i, const ThemeDescriptor& descriptor)
{
    writeDescriptor(raw_.data() + kShuffleThemesOffset + i * 8, descriptor);
}

void SaveData::resetShuffleOrder()
{
    for (int i = 0; i < kShuffleSlots; ++i)
    {
        raw_[kShuffleSeedOffset + i] = static_cast<uint8_t>(i);
        raw_[kShuffleSlotsOffset + i] = static_cast<uint8_t>(i);
    }
    raw_[kShuffleSeedOffset + kShuffleSlots] = 0;
}

} // namespace tz::home
