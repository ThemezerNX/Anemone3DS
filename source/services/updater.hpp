// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Updates from the latest release, read in the format of GitHub's releases API from UPDATE_URL (Themezer redirects it
// to the releases in use).

#pragma once

#include "core/result.hpp"
#include "sys/net.hpp"

#include <optional>
#include <string>

namespace tz::update
{

struct Asset
{
    std::string url;
    uint64_t size = 0;
    std::string sha256; // lowercase hex from the release's "digest", empty when it has none
};

struct Release
{
    std::string version; // without the leading 'v'
    std::string notes;   // the release description, as Markdown
    Asset cia;           // the asset ending in .cia, if any
    Asset threedsx;      // the asset ending in .3dsx, if any
};

// Where people update by hand, when the app can't update itself (a 3dsx started without its path, say).
constexpr const char* kReleasesPage = "github.com/ThemezerNX/themezer-3ds/releases";

// How the running app was started, which decides what an update replaces.
enum class InstallKind : uint8_t
{
    Cia,      // installed as a title
    ThreeDsx, // a 3dsx on the SD card, at its argv[0]
    None,     // can't be updated from inside (no argv[0], started over the network...)
};

InstallKind installKind(const char* appPath);

// The latest release, when it is newer than this build. Development builds (with "-dev" in their version) are
// never offered updates.
Expected<std::optional<Release>> check(net::Client& client, const CancelToken& cancel);

// Downloads the asset for this kind of install, checks its size and digest, and installs it: a CIA is streamed
// into the system (cancelled without changes on any failure); a 3dsx is written next to the running one and swapped
// in once complete. The new version runs from the next start.
Expected<void> install(net::Client& client, const Release& release, InstallKind kind, const char* appPath,
                       net::Progress* progress, const CancelToken& cancel);

} // namespace tz::update
