// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "app/strings.hpp"

#include <3ds.h>
#include <jansson.h>
#include <string>
#include <unordered_map>

namespace tz
{

namespace
{
std::unordered_map<std::string, std::string> translations;

const char* languageCode(int language)
{
    switch (language)
    {
    case CFG_LANGUAGE_JP:
        return "ja";
    case CFG_LANGUAGE_FR:
        return "fr";
    case CFG_LANGUAGE_DE:
        return "de";
    case CFG_LANGUAGE_IT:
        return "it";
    case CFG_LANGUAGE_ES:
        return "es";
    case CFG_LANGUAGE_ZH:
        return "zh-Hans";
    case CFG_LANGUAGE_KO:
        return "ko";
    case CFG_LANGUAGE_NL:
        return "nl";
    case CFG_LANGUAGE_PT:
        return "pt";
    case CFG_LANGUAGE_RU:
        return "ru";
    case CFG_LANGUAGE_TW:
        return "zh-Hant";
    default:
        return "en";
    }
}
} // namespace

void loadTranslations(int systemLanguage)
{
    translations.clear();
    const std::string path = std::string("romfs:/lang/") + languageCode(systemLanguage) + ".json";
    json_error_t error;
    json_t* root = json_load_file(path.c_str(), 0, &error);
    if (!root)
        return;
    const char* key;
    json_t* value;
    json_object_foreach(root, key, value)
    {
        if (json_is_string(value) && json_string_length(value) > 0)
            translations.emplace(key, json_string_value(value));
    }
    json_decref(root);
}

const char* tr(const char* english)
{
    if (translations.empty())
        return english;
    auto it = translations.find(english);
    return it == translations.end() ? english : it->second.c_str();
}

std::string format(std::string_view pattern, std::initializer_list<std::string_view> args)
{
    std::string out;
    out.reserve(pattern.size() + 32);
    auto arg = args.begin();
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}' && arg != args.end())
        {
            out.append(*arg++);
            ++i;
        }
        else
        {
            out += pattern[i];
        }
    }
    return out;
}

} // namespace tz
