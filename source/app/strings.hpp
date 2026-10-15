// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Translations. The English text is the key: tr("Install") gives the text in the console's language when a
// translation file has it (romfs:/lang/<code>.json, an object of English to translated text), and the English
// otherwise. "{}" in a text is filled in by format(), in order.

#pragma once

#include <string>
#include <string_view>

namespace tz
{

void loadTranslations(int systemLanguage);
const char* tr(const char* english);

// Replaces each "{}" in pattern with the next argument.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> args);

template <class... Args>
std::string trf(const char* english, const Args&... args)
{
    return format(tr(english), {std::string_view(args)...});
}

} // namespace tz
