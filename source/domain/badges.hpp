// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The HOME Menu's badges: BadgeData.dat (names and pictures) and BadgeMngFile.dat (sets, badges and where they are
// placed) in extdata 0x14D1. The layout follows GYTB by MrCheeze and Advanced Badge Editor by AntiMach.

#pragma once

#include "core/image.hpp"
#include "core/io.hpp"
#include "core/cancel.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tz::badges
{

constexpr uint32_t kExtdataId = 0x14D1;
constexpr uint32_t kDataSize = 0xF4DF80;
constexpr uint32_t kManagerSize = 0xD4A8;
constexpr int kMaxBadges = 1000;
constexpr int kMaxSets = 100;
constexpr const char* kSetIconName = "_seticon.png";

// One set on the SD card. A folder and a zip with the same name are one set; the PNGs directly in the badge folder
// make one more set of their own.
struct SetSource
{
    std::string name;                    // shown on the HOME Menu
    std::string author;                  // when the zip's name says (Themezer's downloads do)
    std::string folderPath;              // "/Badges/<name>", or empty
    std::string zipPath;                 // "/Badges/<name>.zip", or empty
    std::vector<std::string> loosePaths; // the PNGs directly in /Badges, for the set of loose badges

    bool loose() const { return !loosePaths.empty(); }
};

// The sets in root, sorted by name, with the loose PNGs last as a set named looseName.
Expected<std::vector<SetSource>> findSets(FileSystem& sd, std::string_view root, std::string_view looseName);

struct SetSummary
{
    int images = 0; // PNG files, not counting the set icon; a sheet of several badges counts once
    bool hasIcon = false;
};

// Counts a set's pictures without decoding them: a folder listing and zip directories.
Expected<SetSummary> summarize(FileSystem& sd, const SetSource& set);

// The set's icon (_seticon.png, 48x48). NotFound without one.
Expected<Image> setIcon(FileSystem& sd, const SetSource& set);

// The set's first badges as 64x64 images, sheets cut into their badges, for a preview.
Expected<std::vector<Image>> previewBadges(FileSystem& sd, const SetSource& set, int max, const CancelToken& cancel);

struct InstallResult
{
    int sets = 0;
    int badges = 0;
    int skipped = 0;   // pictures that couldn't be read or have the wrong size
    bool full = false; // stopped at the HOME Menu's limit of kMaxBadges badges or kMaxSets sets
};

// Called with how many of total steps are done; returning false cancels.
using ProgressFn = std::function<bool(int done, int total)>;

// Replaces the HOME Menu's badges with these sets, in this order. principalId is the console's Nintendo Network ID
// number (0 when it has none); defaultIcon is a 48x48 PNG for sets without an icon. The badges placed on the HOME
// Menu are kept when they still exist afterwards.
Expected<InstallResult> install(FileSystem& sd, FileSystem& badgeExt, std::span<const SetSource> sets,
                                uint32_t principalId, ByteSpan defaultIcon, const ProgressFn& progress);

// Takes every badge off the HOME Menu.
Expected<void> uninstall(FileSystem& badgeExt);

// How many badges the HOME Menu has; 0 when it has no badge data.
int installedCount(FileSystem& badgeExt);

// The principal ID stored in the badge files, to keep when the account can't be read.
uint32_t storedPrincipalId(FileSystem& badgeExt);

// Creates the two badge files when they're missing, as the HOME Menu would.
Expected<void> ensureFiles(FileSystem& badgeExt);

// Copies the two badge files into dir on the SD card, unless dir already has a copy. Returns whether it copied:
// the first copy keeps the badges from before the app ever changed them, for restoreBackup.
Expected<bool> backupOnce(FileSystem& badgeExt, FileSystem& sd, std::string_view dir);
Expected<void> restoreBackup(FileSystem& sd, std::string_view dir, FileSystem& badgeExt);
bool hasBackup(FileSystem& sd, std::string_view dir);

// Saves the HOME Menu's badges as PNGs in dir, a folder per set with its icon, so they can be installed again.
// Returns how many badges it saved.
Expected<int> exportBadges(FileSystem& badgeExt, FileSystem& sd, std::string_view dir, const ProgressFn& progress);

} // namespace tz::badges
