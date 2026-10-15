// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/result.hpp"

#include <cstdarg>

namespace tz::log
{

// Lines go to the debug output (a debugger attached through Luma, or an emulator's log), and the last ones are
// kept in memory for /3ds/Themezer/log.txt, written when the app exits after an error.
void init();
void write(const char* level, const char* format, ...) __attribute__((format(printf, 2, 3)));
void writeError(const char* what, const Error& error);
// Writes the kept lines to the SD card.
void save(const char* path);

} // namespace tz::log

#define LOG_INFO(...) ::tz::log::write("I", __VA_ARGS__)
#define LOG_WARN(...) ::tz::log::write("W", __VA_ARGS__)
#define LOG_ERROR(...) ::tz::log::write("E", __VA_ARGS__)
