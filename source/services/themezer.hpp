// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Themezer's catalogue, through its remote API (v2):
//   GET <api>/themes|splashes|badges?page=<n>&q=<search>
//   { "page": 1, "pages": 3, "items": [ { "id", "name", "author", "description", "iconUrl", "previewUrl",
//     "downloadUrl", "audioUrl", "filename" } ] }

#pragma once

#include "core/result.hpp"
#include "sys/net.hpp"

#include <string>
#include <vector>

namespace tz::themezer
{

constexpr const char* kApiUrl = "https://3ds.themezer.net/anemone/v2";
constexpr const char* kWebsite = "https://themezer.net";
constexpr int kPageSize = 24;

enum class Kind : uint8_t
{
    Themes,
    Splashes,
    Badges,
};

struct Item
{
    std::string id; // Themezer's quick ID
    std::string name;
    std::string author;
    std::string description;
    std::string iconUrl;     // a 48x48 PNG
    std::string previewUrl;  // a PNG, usually both screens (400x480)
    std::string downloadUrl; // a zip
    std::string audioUrl;    // the theme's bgm.bcstm; empty for themes without music, splashes and badges
    std::string filename;    // the zip's name on the SD card, made safe for it, ending in .zip
};

struct Page
{
    int page = 1;
    int pages = 1;
    std::vector<Item> items;
};

std::string pageUrl(Kind kind, int page, std::string_view query);
// Items without an id or a download URL are left out. InvalidData for anything that isn't such a page.
Expected<Page> parsePage(ByteSpan json);
Expected<Page> fetchPage(net::Client& client, Kind kind, int page, std::string_view query, const CancelToken& cancel);

} // namespace tz::themezer
