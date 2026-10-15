// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "app/config.hpp"

#include <jansson.h>
#include <string_view>

namespace tz
{

namespace
{

const char* sortName(library::Sort sort)
{
    switch (sort)
    {
    case library::Sort::Author:
        return "author";
    case library::Sort::FileName:
        return "file";
    case library::Sort::Name:
        break;
    }
    return "name";
}

library::Sort parseSort(const char* name, library::Sort fallback)
{
    if (!name)
        return fallback;
    const std::string_view value(name);
    if (value == "author")
        return library::Sort::Author;
    if (value == "file")
        return library::Sort::FileName;
    if (value == "name")
        return library::Sort::Name;
    return fallback;
}

void readBool(json_t* root, const char* key, bool& out)
{
    json_t* value = json_object_get(root, key);
    if (json_is_boolean(value))
        out = json_is_true(value);
}

} // namespace

Config Config::load(FileSystem& sd)
{
    Config config;
    auto data = sd.readFile(kConfigPath, 64 * 1024);
    if (!data)
        return config;
    json_error_t error;
    json_t* root = json_loadb(reinterpret_cast<const char*>(data->data()), data->size(), 0, &error);
    if (!json_is_object(root))
    {
        json_decref(root);
        return config;
    }
    readBool(root, "backgroundMusic", config.backgroundMusic);
    readBool(root, "checkForUpdates", config.checkForUpdates);
    // the names the app used before this version
    readBool(root, "Background Music", config.backgroundMusic);
    readBool(root, "Check For Updates", config.checkForUpdates);
    config.themeSort = parseSort(json_string_value(json_object_get(root, "themeSort")), config.themeSort);
    config.splashSort = parseSort(json_string_value(json_object_get(root, "splashSort")), config.splashSort);
    if (json_t* tab = json_object_get(root, "lastTab"); json_is_integer(tab))
        config.lastTab = static_cast<int>(json_integer_value(tab));
    json_decref(root);
    return config;
}

Expected<void> Config::save(FileSystem& sd) const
{
    json_t* root = json_object();
    json_object_set_new(root, "backgroundMusic", json_boolean(backgroundMusic));
    json_object_set_new(root, "checkForUpdates", json_boolean(checkForUpdates));
    json_object_set_new(root, "themeSort", json_string(sortName(themeSort)));
    json_object_set_new(root, "splashSort", json_string(sortName(splashSort)));
    json_object_set_new(root, "lastTab", json_integer(lastTab));
    char* text = json_dumps(root, JSON_INDENT(2) | JSON_SORT_KEYS);
    json_decref(root);
    if (!text)
        return fail(Errc::OutOfMemory);
    const std::string_view content(text);
    TZ_CHECK(sd.createDirectories(kAppFolder));
    auto result = sd.writeFile(kConfigPath, ByteSpan(reinterpret_cast<const uint8_t*>(content.data()), content.size()));
    free(text);
    return result;
}

} // namespace tz
