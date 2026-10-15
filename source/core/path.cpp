// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/path.hpp"

#include <algorithm>

namespace tz::path
{

namespace
{
char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
} // namespace

std::string_view filename(std::string_view path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view parent(std::string_view path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return {};
    if (slash == 0)
        return path.substr(0, 1);
    return path.substr(0, slash);
}

std::string_view extension(std::string_view path)
{
    const std::string_view name = filename(path);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return name.substr(dot + 1);
}

std::string_view stem(std::string_view path)
{
    const std::string_view name = filename(path);
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return name;
    return name.substr(0, dot);
}

std::string join(std::string_view base, std::string_view name)
{
    std::string result(base);
    while (!result.empty() && result.back() == '/')
        result.pop_back();
    while (!name.empty() && name.front() == '/')
        name.remove_prefix(1);
    result += '/';
    result += name;
    return result;
}

std::vector<std::string_view> split(std::string_view path)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        if (end > start)
            parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool hasExtension(std::string_view path, std::string_view ext)
{
    return equalsIgnoreCase(extension(path), ext);
}

std::string sanitizeFilename(std::string_view name, size_t maxBytes)
{
    std::string result;
    result.reserve(name.size());
    for (char c : name)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
            continue;
        if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
            result += '-';
        else
            result += c;
    }

    if (result.size() > maxBytes)
    {
        size_t cut = maxBytes;
        // don't end inside a multi-byte UTF-8 character
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80)
            --cut;
        result.resize(cut);
    }

    while (!result.empty() && (result.back() == '.' || result.back() == ' '))
        result.pop_back();
    size_t leading = 0;
    while (leading < result.size() && result[leading] == ' ')
        ++leading;
    result.erase(0, leading);

    return result.empty() ? std::string("untitled") : result;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

int compareIgnoreCase(std::string_view a, std::string_view b)
{
    const size_t length = std::min(a.size(), b.size());
    for (size_t i = 0; i < length; ++i)
    {
        const char x = lower(a[i]);
        const char y = lower(b[i]);
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

} // namespace tz::path
