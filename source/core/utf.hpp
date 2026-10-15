// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tz::utf
{

// Invalid sequences become U+FFFD; conversion stops at the first NUL in UTF-16 input.
std::string fromUtf16(std::span<const char16_t> text);
std::u16string toUtf16(std::string_view text);

// UTF-16LE stored in a file, at most maxUnits code units, up to the first NUL.
std::string fromUtf16Le(const uint8_t* data, size_t maxUnits);
// Writes as UTF-16LE into exactly maxUnits code units, NUL padded; cut at a character boundary if too long.
void toUtf16Le(std::string_view text, uint8_t* out, size_t maxUnits);

// Decodes the code point at text[index] and moves index past it. Returns U+FFFD for invalid input.
char32_t decodeNext(std::string_view text, size_t& index);
void appendUtf8(std::string& out, char32_t codePoint);

// The number of code points.
size_t length(std::string_view text);
// The first maxCodePoints code points.
std::string_view prefix(std::string_view text, size_t maxCodePoints);

} // namespace tz::utf
