// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "sys/log.hpp"

#include "sys/thread.hpp"

#include <3ds.h>
#include <array>
#include <cstdio>
#include <string>

namespace tz::log
{

namespace
{
constexpr size_t kKeptLines = 128;
Mutex mutex;
std::array<std::string, kKeptLines> lines;
size_t next = 0;
size_t count = 0;
} // namespace

void init()
{
    consoleDebugInit(debugDevice_SVC);
}

void write(const char* level, const char* format, ...)
{
    char text[512];
    const u64 ms = osGetTime();
    int prefix = std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu.%03llu %s ", (ms / 3600000) % 24,
                               (ms / 60000) % 60, (ms / 1000) % 60, ms % 1000, level);
    if (prefix < 0)
        prefix = 0;
    va_list args;
    va_start(args, format);
    std::vsnprintf(text + prefix, sizeof(text) - prefix, format, args);
    va_end(args);

    std::fprintf(stderr, "%s\n", text);

    std::lock_guard lock(mutex);
    lines[next] = text;
    next = (next + 1) % kKeptLines;
    if (count < kKeptLines)
        ++count;
}

void writeError(const char* what, const Error& error)
{
    write("E", "%s: %s (%08lX) %s", what, errcName(error.code), static_cast<unsigned long>(error.system),
          error.detail.c_str());
}

void save(const char* path)
{
    std::lock_guard lock(mutex);
    FILE* file = std::fopen(path, "w");
    if (!file)
        return;
    std::fprintf(file, "%s %s\n", APP_TITLE, APP_VERSION);
    for (size_t i = 0; i < count; ++i)
        std::fprintf(file, "%s\n", lines[(next + kKeptLines - count + i) % kKeptLines].c_str());
    std::fclose(file);
}

} // namespace tz::log
