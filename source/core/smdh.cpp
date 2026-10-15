// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The SMDH layout, from 3dbrew: "SMDH" at 0, a u16 version at 4, then 16 titles of 0x200 bytes from 0x8, one per
// system language (CFG_Language order), each a short description (0x80 bytes, 64 UTF-16LE units), a long description
// (0x100 bytes, 128 units) and a publisher (0x80 bytes, 64 units). The settings at 0x2008 (ratings, region lockout,
// flags) don't matter to themes. The icons are tiled RGB565: 24x24 at 0x2040 and 48x48 at 0x24C0, to 0x36C0.

#include "core/smdh.hpp"

#include "core/image.hpp"
#include "core/utf.hpp"

#include <cstring>
#include <string_view>

namespace tz
{

namespace
{

constexpr size_t kTitlesOffset = 0x8;
constexpr size_t kTitleSize = 0x200;
constexpr size_t kNameUnits = 0x40;
constexpr size_t kDescriptionOffset = 0x80;
constexpr size_t kDescriptionUnits = 0x80;
constexpr size_t kAuthorOffset = 0x180;
constexpr size_t kAuthorUnits = 0x40;
constexpr size_t kSmallIconOffset = 0x2040;
constexpr size_t kLargeIconOffset = 0x24C0;
constexpr int kSmallIconSize = 24;
constexpr int kLargeIconSize = 48;
constexpr int kEnglish = 1;

std::string trimmed(std::string text)
{
    const std::string_view space = " \t\r\n\v\f";
    const size_t first = text.find_first_not_of(space);
    if (first == std::string::npos)
        return {};
    const size_t last = text.find_last_not_of(space);
    return text.substr(first, last - first + 1);
}

} // namespace

const SmdhText& Smdh::title(int language) const
{
    const auto index = static_cast<size_t>(language);
    if (language >= 0 && index < titles.size() && !titles[index].name.empty())
        return titles[index];
    if (!titles[kEnglish].name.empty())
        return titles[kEnglish];
    for (const SmdhText& text : titles)
        if (!text.name.empty())
            return text;
    return titles[0];
}

Expected<Smdh> parseSmdh(ByteSpan data)
{
    if (data.size() < kSmdhSize || !startsWith(data, "SMDH"))
        return fail(Errc::InvalidData, "not an SMDH");

    Smdh smdh;
    for (size_t i = 0; i < smdh.titles.size(); ++i)
    {
        const uint8_t* title = data.data() + kTitlesOffset + i * kTitleSize;
        SmdhText& text = smdh.titles[i];
        text.name = trimmed(utf::fromUtf16Le(title, kNameUnits));
        text.description = trimmed(utf::fromUtf16Le(title + kDescriptionOffset, kDescriptionUnits));
        text.author = trimmed(utf::fromUtf16Le(title + kAuthorOffset, kAuthorUnits));
    }
    for (size_t i = 0; i < smdh.smallIcon.size(); ++i)
        smdh.smallIcon[i] = readU16(data.data() + kSmallIconOffset + i * 2);
    for (size_t i = 0; i < smdh.largeIcon.size(); ++i)
        smdh.largeIcon[i] = readU16(data.data() + kLargeIconOffset + i * 2);
    return smdh;
}

Bytes buildSmdh(const SmdhText& text, const std::array<uint16_t, 48 * 48>& largeIcon)
{
    Bytes data(kSmdhSize, 0);
    std::memcpy(data.data(), "SMDH", 4); // version 0 follows

    for (size_t i = 0; i < 16; ++i)
    {
        uint8_t* title = data.data() + kTitlesOffset + i * kTitleSize;
        utf::toUtf16Le(text.name, title, kNameUnits);
        utf::toUtf16Le(text.description, title + kDescriptionOffset, kDescriptionUnits);
        utf::toUtf16Le(text.author, title + kAuthorOffset, kAuthorUnits);
    }

    std::array<uint16_t, kSmallIconSize * kSmallIconSize> smallIcon{};
    imageToIcon(resize(iconToImage(largeIcon.data(), kLargeIconSize), kSmallIconSize, kSmallIconSize),
                smallIcon.data(), kSmallIconSize);
    for (size_t i = 0; i < smallIcon.size(); ++i)
        writeU16(data.data() + kSmallIconOffset + i * 2, smallIcon[i]);
    for (size_t i = 0; i < largeIcon.size(); ++i)
        writeU16(data.data() + kLargeIconOffset + i * 2, largeIcon[i]);
    return data;
}

} // namespace tz
