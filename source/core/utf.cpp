// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "core/utf.hpp"

#include <cstring>

namespace tz::utf
{

namespace
{
constexpr char32_t kReplacement = 0xFFFD;

bool isContinuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

void appendUtf16(std::u16string& out, char32_t cp)
{
    if (cp >= 0x10000)
    {
        cp -= 0x10000;
        out += static_cast<char16_t>(0xD800 + (cp >> 10));
        out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
    }
    else
    {
        out += static_cast<char16_t>(cp);
    }
}
} // namespace

char32_t decodeNext(std::string_view text, size_t& index)
{
    const auto lead = static_cast<unsigned char>(text[index++]);
    if (lead < 0x80)
        return lead;

    int extra;
    char32_t cp;
    char32_t minimum;
    if ((lead & 0xE0) == 0xC0)
    {
        extra = 1;
        cp = lead & 0x1F;
        minimum = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        extra = 2;
        cp = lead & 0x0F;
        minimum = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        extra = 3;
        cp = lead & 0x07;
        minimum = 0x10000;
    }
    else
    {
        return kReplacement;
    }

    for (int i = 0; i < extra; ++i)
    {
        if (index >= text.size() || !isContinuation(static_cast<unsigned char>(text[index])))
            return kReplacement;
        cp = (cp << 6) | (static_cast<unsigned char>(text[index++]) & 0x3F);
    }

    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return kReplacement;
    return cp;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string fromUtf16(std::span<const char16_t> text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        char32_t unit = text[i];
        if (unit == 0)
            break;
        if (unit >= 0xD800 && unit <= 0xDBFF)
        {
            if (i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (text[i + 1] - 0xDC00);
                ++i;
            }
            else
            {
                unit = kReplacement;
            }
        }
        else if (unit >= 0xDC00 && unit <= 0xDFFF)
        {
            unit = kReplacement;
        }
        appendUtf8(out, unit);
    }
    return out;
}

std::u16string toUtf16(std::string_view text)
{
    std::u16string out;
    out.reserve(text.size());
    size_t index = 0;
    while (index < text.size())
        appendUtf16(out, decodeNext(text, index));
    return out;
}

std::string fromUtf16Le(const uint8_t* data, size_t maxUnits)
{
    std::u16string units;
    units.reserve(maxUnits);
    for (size_t i = 0; i < maxUnits; ++i)
    {
        const auto unit = static_cast<char16_t>(data[i * 2] | (data[i * 2 + 1] << 8));
        if (unit == 0)
            break;
        units += unit;
    }
    return fromUtf16(units);
}

void toUtf16Le(std::string_view text, uint8_t* out, size_t maxUnits)
{
    std::memset(out, 0, maxUnits * 2);
    const std::u16string units = toUtf16(text);
    size_t count = units.size() < maxUnits ? units.size() : maxUnits;
    // don't leave half of a surrogate pair at the end
    if (count > 0 && count < units.size() && units[count - 1] >= 0xD800 && units[count - 1] <= 0xDBFF)
        --count;
    for (size_t i = 0; i < count; ++i)
    {
        out[i * 2] = static_cast<uint8_t>(units[i]);
        out[i * 2 + 1] = static_cast<uint8_t>(units[i] >> 8);
    }
}

size_t length(std::string_view text)
{
    size_t count = 0;
    for (char c : text)
        count += !isContinuation(static_cast<unsigned char>(c));
    return count;
}

std::string_view prefix(std::string_view text, size_t maxCodePoints)
{
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (!isContinuation(static_cast<unsigned char>(text[i])))
        {
            if (count == maxCodePoints)
                return text.substr(0, i);
            ++count;
        }
    }
    return text;
}

} // namespace tz::utf
