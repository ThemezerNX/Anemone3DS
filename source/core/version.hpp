// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace tz
{

// A semantic version like "4.1.0", "v4.1.0" or "4.1.0-dev+abc1234". Build metadata after '+' is ignored; a
// pre-release ("-dev", "-beta.2") sorts before the release it precedes.
struct Version
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;

    static std::optional<Version> parse(std::string_view text);
    std::string toString() const;

    std::strong_ordering operator<=>(const Version& other) const;
    bool operator==(const Version& other) const = default;
};

} // namespace tz
