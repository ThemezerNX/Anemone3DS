// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// What the app tells the user when something fails: what happened and what to do, in plain words.

#pragma once

#include "core/result.hpp"

#include <string>

namespace tz::scenes
{

struct ErrorInfo
{
    std::string title;
    std::string message;
};

// Any error, in general terms.
ErrorInfo describe(const Error& error);
// Installing something called name failed.
ErrorInfo installError(const Error& error, const std::string& name);
// The HOME Menu's theme storage can't be used.
ErrorInfo themeStorageError(const Error& error);
// A download failed.
ErrorInfo downloadError(const Error& error);
// Why the HOME Menu's badges can't be changed.
ErrorInfo badgeStorageError(const Error& error);

// Shows it in a dialog.
void showError(const ErrorInfo& info);

} // namespace tz::scenes
