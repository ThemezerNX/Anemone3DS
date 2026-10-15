// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "services/themezer.hpp"

#include "core/path.hpp"

#include <algorithm>
#include <jansson.h>
#include <limits>
#include <memory>
#include <optional>

namespace tz::themezer
{

namespace
{

constexpr uint64_t kMaxPageSize = 2 * 1024 * 1024;
// sanitizeFilename()'s length for a whole name, ".zip" included
constexpr size_t kMaxFilenameBytes = 200;
constexpr std::string_view kZipExtension = ".zip";

struct JsonDelete
{
    void operator()(json_t* json) const { json_decref(json); }
};
using JsonPtr = std::unique_ptr<json_t, JsonDelete>;

const char* kindPath(Kind kind)
{
    switch (kind)
    {
    case Kind::Themes:
        return "themes";
    case Kind::Splashes:
        return "splashes";
    case Kind::Badges:
        return "badges";
    }
    return "themes";
}

std::string_view trim(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";
    const size_t start = text.find_first_not_of(kSpace);
    if (start == std::string_view::npos)
        return {};
    return text.substr(start, text.find_last_not_of(kSpace) - start + 1);
}

// A string field; "" when it is missing, null or something else.
std::string text(json_t* object, const char* key)
{
    json_t* value = json_object_get(object, key);
    if (!json_is_string(value))
        return {};
    return std::string(json_string_value(value), json_string_length(value));
}

// A count of at least 1, or the fallback when there is none.
int count(json_t* object, const char* key, int fallback)
{
    json_t* value = json_object_get(object, key);
    if (!json_is_number(value))
        return fallback;
    const double number = json_number_value(value);
    if (!(number >= 1)) // NaN too
        return fallback;
    if (number >= std::numeric_limits<int>::max())
        return std::numeric_limits<int>::max();
    return static_cast<int>(number);
}

// Themezer's quick IDs are strings ("T1ED69"); a number is taken too.
std::string identifier(json_t* item)
{
    json_t* id = json_object_get(item, "id");
    if (json_is_integer(id))
        return std::to_string(json_integer_value(id));
    return std::string(trim(text(item, "id")));
}

// The zip's name on the SD card: safe for FAT, and ending in ".zip" so that the library recognizes it.
std::string zipFilename(std::string_view name)
{
    if (path::hasExtension(name, "zip"))
        name.remove_suffix(kZipExtension.size());
    return path::sanitizeFilename(name, kMaxFilenameBytes - kZipExtension.size()) + std::string(kZipExtension);
}

std::optional<Item> parseItem(json_t* value)
{
    if (!json_is_object(value))
        return std::nullopt;

    Item item;
    item.id = identifier(value);
    item.downloadUrl = text(value, "downloadUrl");
    if (item.id.empty() || item.downloadUrl.empty())
        return std::nullopt;

    // names come with stray spaces at times ("Peelingflesh ")
    item.name = std::string(trim(text(value, "name")));
    item.author = std::string(trim(text(value, "author")));
    item.description = std::string(trim(text(value, "description")));
    item.iconUrl = text(value, "iconUrl");
    item.previewUrl = text(value, "previewUrl");
    item.audioUrl = text(value, "audioUrl");

    std::string filename(trim(text(value, "filename")));
    if (filename.empty())
    {
        filename = item.name;
        if (!item.author.empty())
            filename += filename.empty() ? item.author : " by " + item.author;
        if (filename.empty())
            filename = item.id;
    }
    item.filename = zipFilename(filename);
    return item;
}

} // namespace

std::string pageUrl(Kind kind, int page, std::string_view query)
{
    return std::string(kApiUrl) + '/' + kindPath(kind) + "?page=" + std::to_string(page) +
           "&q=" + net::urlEncode(query);
}

Expected<Page> parsePage(ByteSpan json)
{
    json_error_t error;
    const JsonPtr root(json_loadb(reinterpret_cast<const char*>(json.data()), json.size(), 0, &error));
    if (!root)
        return fail(Errc::InvalidData, std::string("page: ") + error.text);
    json_t* items = json_object_get(root.get(), "items");
    if (!json_is_array(items))
        return fail(Errc::InvalidData, "page without items");

    Page page;
    page.page = count(root.get(), "page", 1);
    // a page past the end (an empty one) still counts as one
    page.pages = std::max(count(root.get(), "pages", 1), page.page);
    page.items.reserve(json_array_size(items));
    size_t index = 0;
    json_t* value = nullptr;
    json_array_foreach(items, index, value)
    {
        if (std::optional<Item> item = parseItem(value))
            page.items.push_back(std::move(*item));
    }
    return page;
}

Expected<Page> fetchPage(net::Client& client, Kind kind, int page, std::string_view query, const CancelToken& cancel)
{
    net::Request request;
    request.url = pageUrl(kind, page, query);
    request.accept = {"application/json"};
    request.maxSize = kMaxPageSize;
    request.cancel = &cancel;
    const net::Response response = TZ_TRY(client.get(request));
    return parsePage(response.body);
}

} // namespace tz::themezer
