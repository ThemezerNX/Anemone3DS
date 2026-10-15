// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "gfx/text.hpp"

#include "core/utf.hpp"

#include <3ds.h>
#include <citro2d.h>
#include <cmath>
#include <unordered_map>

namespace tz::gfx
{

namespace
{

constexpr size_t kFrameGlyphs = 6144;
constexpr size_t kCacheLimit = 384;
constexpr const char* kEllipsis = "…";

C2D_TextBuf frameBuffer = nullptr;
CFNT_s* font = nullptr;
// the font's measures at size 1: its line, and the middle of its capitals below the line's top (the console's font
// has its baseline 25 pixels down and capitals 19 pixels high)
float baseLineFeed = 30.0f;
float capitalsMiddle = 15.5f;

// Results of ellipsize and wrap, which lists ask for every frame with the same arguments.
std::unordered_map<std::string, std::string> ellipsized;
std::unordered_map<std::string, std::vector<std::string>> wrapped;

std::string cacheKey(std::string_view text, float width, float size, int lines)
{
    std::string key;
    key.reserve(text.size() + 24);
    key.append(reinterpret_cast<const char*>(&width), sizeof(width));
    key.append(reinterpret_cast<const char*>(&size), sizeof(size));
    key.append(reinterpret_cast<const char*>(&lines), sizeof(lines));
    key.append(text);
    return key;
}

float advance(char32_t codePoint, float size)
{
    if (!font)
        return 8.0f * size;
    int glyph = fontGlyphIndexFromCodePoint(font, codePoint);
    const charWidthInfo_s* info = fontGetCharWidthInfo(font, glyph);
    return info ? info->charWidth * size : 0.0f;
}

bool breaksAnywhere(char32_t c)
{
    // CJK text has no spaces between words: a line can break between any two characters
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

void drawLine(std::string_view line, float x, float y, float size, Color color, Align align)
{
    if (line.empty() || !frameBuffer)
        return;
    C2D_Text text;
    const std::string copy(line); // C2D wants a terminated string
    C2D_TextParse(&text, frameBuffer, copy.c_str());
    if (align == Align::Center)
        x -= textWidth(line, size) / 2;
    else if (align == Align::Right)
        x -= textWidth(line, size);
    C2D_DrawText(&text, C2D_WithColor, std::round(x), std::round(y), 0.0f, size, size, color);
}

} // namespace

void initText()
{
    frameBuffer = C2D_TextBufNew(kFrameGlyphs);
    fontEnsureMapped();
    font = fontGetSystemFont();
    if (font)
    {
        baseLineFeed = fontGetInfo(font)->lineFeed;
        const float baseline = fontGetGlyphInfo(font)->baselinePos;
        capitalsMiddle = baseline - baseline * 0.76f / 2;
    }
}

void exitText()
{
    if (frameBuffer)
        C2D_TextBufDelete(frameBuffer);
    frameBuffer = nullptr;
}

void beginTextFrame()
{
    if (frameBuffer)
        C2D_TextBufClear(frameBuffer);
    if (ellipsized.size() > kCacheLimit)
        ellipsized.clear();
    if (wrapped.size() > kCacheLimit)
        wrapped.clear();
}

float lineHeight(float size)
{
    return baseLineFeed * size;
}

float textTop(float middle, float size)
{
    return std::round(middle - capitalsMiddle * size);
}

float textMiddle(float top, float size)
{
    return top + capitalsMiddle * size;
}

float textWidth(std::string_view text, float size)
{
    float width = 0;
    size_t i = 0;
    while (i < text.size())
        width += advance(utf::decodeNext(text, i), size);
    return width;
}

std::string ellipsize(std::string_view text, float maxWidth, float size)
{
    if (maxWidth <= 0)
        return std::string(text);
    const std::string key = cacheKey(text, maxWidth, size, 0);
    if (auto it = ellipsized.find(key); it != ellipsized.end())
        return it->second;

    std::string result;
    if (textWidth(text, size) <= maxWidth)
    {
        result = text;
    }
    else
    {
        const float room = maxWidth - textWidth(kEllipsis, size);
        float width = 0;
        size_t i = 0;
        size_t end = 0;
        while (i < text.size())
        {
            const float w = advance(utf::decodeNext(text, i), size);
            if (width + w > room)
                break;
            width += w;
            end = i;
        }
        // no space before the ellipsis
        while (end > 0 && text[end - 1] == ' ')
            --end;
        result = std::string(text.substr(0, end)) + kEllipsis;
    }
    ellipsized.emplace(key, result);
    return result;
}

std::vector<std::string> wrap(std::string_view text, float width, float size, int maxLines)
{
    const std::string key = cacheKey(text, width, size, maxLines);
    if (auto it = wrapped.find(key); it != wrapped.end())
        return it->second;

    std::vector<std::string> lines;
    std::string line;
    float lineWidth = 0;
    // where the current line could break: after its last space
    size_t breakAt = std::string::npos;
    float widthAtBreak = 0;

    auto flush = [&](std::string text) {
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        lines.push_back(std::move(text));
    };

    size_t i = 0;
    while (i < text.size())
    {
        const size_t start = i;
        const char32_t c = utf::decodeNext(text, i);
        if (c == '\n')
        {
            flush(std::move(line));
            line.clear();
            lineWidth = 0;
            breakAt = std::string::npos;
            continue;
        }
        const float w = advance(c, size);
        if (lineWidth + w > width && !line.empty())
        {
            if (breakAt != std::string::npos && c != ' ')
            {
                // carry the last word over to the next line
                std::string rest = line.substr(breakAt);
                line.resize(breakAt);
                flush(std::move(line));
                line = std::move(rest);
                lineWidth -= widthAtBreak;
            }
            else
            {
                flush(std::move(line));
                line.clear();
                lineWidth = 0;
            }
            breakAt = std::string::npos;
            if (c == ' ')
                continue; // no space at the start of a line
        }
        line.append(text.substr(start, i - start));
        lineWidth += w;
        if (c == ' ' || breaksAnywhere(c))
        {
            breakAt = line.size();
            widthAtBreak = lineWidth;
        }
    }
    if (!line.empty())
        flush(std::move(line));

    if (maxLines > 0 && static_cast<int>(lines.size()) > maxLines)
    {
        lines.resize(maxLines);
        // the last line ends in "…": shorten it until both fit
        std::string& last = lines.back();
        const float room = width - textWidth(kEllipsis, size);
        while (!last.empty() && (textWidth(last, size) > room || last.back() == ' '))
        {
            while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xC0) == 0x80)
                last.pop_back();
            if (!last.empty())
                last.pop_back();
        }
        last += kEllipsis;
    }
    wrapped.emplace(key, lines);
    return lines;
}

float drawText(std::string_view text, float x, float y, float size, Color color, Align align, float maxWidth)
{
    const std::string shown = maxWidth > 0 ? ellipsize(text, maxWidth, size) : std::string(text);
    drawLine(shown, x, y, size, color, align);
    return textWidth(shown, size);
}

float drawWrapped(std::string_view text, float x, float y, float width, float size, Color color, int maxLines,
                  Align align, float lineSpacing)
{
    const std::vector<std::string> lines = wrap(text, width, size, maxLines);
    const float step = lineHeight(size) * lineSpacing;
    float at = y;
    for (const std::string& line : lines)
    {
        const float lineX = align == Align::Center ? x + width / 2 : (align == Align::Right ? x + width : x);
        drawLine(line, lineX, at, size, color, align);
        at += step;
    }
    return at - y;
}

} // namespace tz::gfx
