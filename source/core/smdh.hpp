// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// SMDH: the name, description, author and icon of a theme, splash or app (info.smdh).

#pragma once

#include "core/bytes.hpp"
#include "core/result.hpp"

#include <array>
#include <string>

namespace tz
{

constexpr size_t kSmdhSize = 0x36C0;

struct SmdhText
{
    std::string name;        // short description, up to 64 characters
    std::string description; // long description, up to 128 characters
    std::string author;      // publisher, up to 64 characters
};

struct Smdh
{
    std::array<SmdhText, 16> titles; // by CFG_Language
    std::array<uint16_t, 48 * 48> largeIcon{};
    std::array<uint16_t, 24 * 24> smallIcon{};

    // The title in this system language, else in English, else the first one that has a name.
    const SmdhText& title(int language) const;
};

// InvalidData unless it is at least kSmdhSize bytes starting with "SMDH". Titles are trimmed of whitespace.
Expected<Smdh> parseSmdh(ByteSpan data);

// An SMDH with this text in every language and this icon (the small icon is scaled down from it).
Bytes buildSmdh(const SmdhText& text, const std::array<uint16_t, 48 * 48>& largeIcon);

} // namespace tz
