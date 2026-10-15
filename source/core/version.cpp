// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/version.hpp"

#include <charconv>
#include <vector>

namespace tz
{

namespace
{

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isNumeric(std::string_view text)
{
    if (text.empty())
        return false;
    for (char c : text)
    {
        if (!isDigit(c))
            return false;
    }
    return true;
}

// The characters SemVer allows in a pre-release identifier: [0-9A-Za-z-].
bool isIdentifier(std::string_view text)
{
    if (text.empty())
        return false;
    for (char c : text)
    {
        if (!isDigit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-')
            return false;
    }
    return true;
}

// "1.2" -> {"1", "2"}; empty parts are kept, so that "1..2" can be refused.
std::vector<std::string_view> splitDots(std::string_view text)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;)
    {
        const size_t dot = text.find('.', start);
        if (dot == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, dot - start));
        start = dot + 1;
    }
}

std::string_view withoutLeadingZeros(std::string_view number)
{
    while (number.size() > 1 && number.front() == '0')
        number.remove_prefix(1);
    return number;
}

// SemVer's precedence of two pre-release identifiers: numbers by their value and before words, words in ASCII order.
std::strong_ordering compareIdentifiers(std::string_view a, std::string_view b)
{
    const bool numericA = isNumeric(a);
    const bool numericB = isNumeric(b);
    if (numericA && numericB)
    {
        // compared as text, so that numbers of any length work: without leading zeros, the longer one is larger
        a = withoutLeadingZeros(a);
        b = withoutLeadingZeros(b);
        if (a.size() != b.size())
            return a.size() <=> b.size();
        return a.compare(b) <=> 0;
    }
    if (numericA != numericB)
        return numericA ? std::strong_ordering::less : std::strong_ordering::greater;
    return a.compare(b) <=> 0;
}

} // namespace

std::optional<Version> Version::parse(std::string_view text)
{
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
        text.remove_prefix(1);
    // build metadata ("+abc1234") takes no part in comparisons; it may contain '-', so it goes first
    text = text.substr(0, text.find('+'));

    Version version;
    const size_t dash = text.find('-');
    if (dash != std::string_view::npos)
    {
        for (std::string_view identifier : splitDots(text.substr(dash + 1)))
        {
            if (!isIdentifier(identifier))
                return std::nullopt;
            // "rc.01" is "rc.1", so that versions equal in order are equal as values too
            if (isNumeric(identifier))
                identifier = withoutLeadingZeros(identifier);
            if (!version.prerelease.empty())
                version.prerelease += '.';
            version.prerelease += identifier;
        }
        text = text.substr(0, dash);
    }

    const std::vector<std::string_view> numbers = splitDots(text);
    if (numbers.size() > 3)
        return std::nullopt;
    int* const fields[] = {&version.major, &version.minor, &version.patch};
    for (size_t i = 0; i < numbers.size(); ++i)
    {
        const std::string_view number = numbers[i];
        if (!isNumeric(number))
            return std::nullopt;
        const char* end = number.data() + number.size();
        const auto [stop, error] = std::from_chars(number.data(), end, *fields[i]);
        if (error != std::errc() || stop != end)
            return std::nullopt;
    }
    return version;
}

std::string Version::toString() const
{
    std::string text = std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
    if (!prerelease.empty())
        text += '-' + prerelease;
    return text;
}

std::strong_ordering Version::operator<=>(const Version& other) const
{
    if (const auto order = major <=> other.major; order != 0)
        return order;
    if (const auto order = minor <=> other.minor; order != 0)
        return order;
    if (const auto order = patch <=> other.patch; order != 0)
        return order;

    // a pre-release comes before the release it leads up to
    if (prerelease.empty() != other.prerelease.empty())
        return prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    if (prerelease.empty())
        return std::strong_ordering::equal;

    const std::vector<std::string_view> mine = splitDots(prerelease);
    const std::vector<std::string_view> theirs = splitDots(other.prerelease);
    for (size_t i = 0; i < mine.size() && i < theirs.size(); ++i)
    {
        if (const auto order = compareIdentifiers(mine[i], theirs[i]); order != 0)
            return order;
    }
    // more identifiers sort after fewer that are otherwise equal: "alpha" < "alpha.1"
    return mine.size() <=> theirs.size();
}

} // namespace tz
