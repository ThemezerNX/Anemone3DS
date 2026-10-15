// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tz::path
{

// "/a/b/c.zip" -> "c.zip"
std::string_view filename(std::string_view path);
// "/a/b/c.zip" -> "/a/b"; "/c.zip" -> "/"
std::string_view parent(std::string_view path);
// "c.zip" -> "zip" (without the dot, as written); "" when there is none
std::string_view extension(std::string_view path);
// "c.tar.zip" -> "c.tar"
std::string_view stem(std::string_view path);
// Joins with exactly one '/' between the parts.
std::string join(std::string_view base, std::string_view name);
// The non-empty parts between the slashes.
std::vector<std::string_view> split(std::string_view path);

bool hasExtension(std::string_view path, std::string_view extension); // case-insensitive, without the dot

// Makes a name that FAT can store: characters it forbids become '-', control characters are dropped, trailing dots
// and spaces are trimmed, and it is cut to maxBytes without splitting a UTF-8 sequence. Never returns "".
std::string sanitizeFilename(std::string_view name, size_t maxBytes = 200);

// ASCII-only case-insensitive comparison, which is how the SD card compares names.
bool equalsIgnoreCase(std::string_view a, std::string_view b);
int compareIgnoreCase(std::string_view a, std::string_view b);

} // namespace tz::path
