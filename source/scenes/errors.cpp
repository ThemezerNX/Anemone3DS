// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/errors.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "gfx/draw.hpp"
#include "sys/log.hpp"
#include "ui/overlays.hpp"

#include <cstdio>

namespace tz::scenes
{

namespace
{
// The system's code, for a bug report.
std::string code(const Error& error)
{
    if (error.system == 0)
        return {};
    char text[32];
    std::snprintf(text, sizeof(text), " (%08lX)", static_cast<unsigned long>(error.system));
    return text;
}
} // namespace

ErrorInfo describe(const Error& error)
{
    log::writeError("shown", error);
    switch (error.code)
    {
    case Errc::NotFound:
        return {tr("It isn't there"), tr("A file the app needs is missing from the SD card.") + code(error)};
    case Errc::InvalidData:
        return {tr("Something's wrong with a file"), tr("A file is damaged, or isn't what its name says.")};
    case Errc::TooLarge:
        return {tr("It's too big"), tr("A file is bigger than the HOME Menu can take.")};
    case Errc::Unsupported:
        return {tr("Not supported"), tr("This kind of file isn't supported.")};
    case Errc::NoSpace:
        return {tr("The SD card is full"), tr("Make some room on the SD card and try again.")};
    case Errc::OutOfMemory:
        return {tr("Out of memory"), tr("Close the app and try again.")};
    case Errc::Cancelled:
        return {tr("Cancelled"), {}};
    case Errc::NoNetwork:
        return {tr("You're offline"), tr("Connect your console to the internet in System Settings and try again.")};
    case Errc::Network:
        return {tr("Can't reach the server"), tr("Check your connection and try again in a moment.") + code(error)};
    case Errc::Tls:
        return {tr("Can't connect securely"),
                tr("Check the console's date and time in System Settings: a wrong clock makes secure connections "
                   "fail.")};
    case Errc::Http:
        return {tr("The server said no"),
                trf("It answered with error {}. Try again later.", std::to_string(error.system))};
    case Errc::Busy:
        return {tr("It's busy"), tr("Something else is using it. Try again in a moment.") + code(error)};
    case Errc::NotAllowed:
        return {tr("Not allowed"), tr("The system didn't allow it.") + code(error)};
    case Errc::Io:
        break;
    }
    return {tr("Something went wrong"), tr("The SD card or the system storage gave an error.") + code(error)};
}

ErrorInfo installError(const Error& error, const std::string& name)
{
    ErrorInfo info = describe(error);
    info.title = trf("{} wasn't installed", name);
    if (error.code == Errc::InvalidData)
        info.message = tr("Its files are damaged or incomplete. Try downloading it again.");
    else if (error.code == Errc::TooLarge)
        info.message = tr("Its files are bigger than the HOME Menu can take.");
    return info;
}

ErrorInfo themeStorageError(const Error& error)
{
    log::writeError("theme storage", error);
    if (error.code == Errc::NotAllowed)
        return {tr("This console has no themes"),
                tr("Its HOME Menu (Chinese or Taiwanese) doesn't support themes.")};
    if (error.code == Errc::NotFound)
        return {tr("Themes aren't set up yet"),
                tr("Open the HOME Menu's theme settings once and choose any theme, then come back.") + code(error)};
    if (error.code == Errc::InvalidData)
        return {tr("The theme storage looks wrong"),
                tr("Choose a theme in the HOME Menu's theme settings once, then come back.")};
    return {tr("Can't open the theme storage"), tr("The system gave an error.") + code(error)};
}

ErrorInfo badgeStorageError(const Error& error)
{
    log::writeError("badge storage", error);
    if (error.code == Errc::Busy)
        return {tr("The HOME Menu is using its badges"),
                tr("Started from the Homebrew Launcher, the app can't change them. Install the CIA version and start "
                   "Themezer from the HOME Menu.")};
    return {tr("Can't open the badge storage"), describe(error).message};
}

ErrorInfo downloadError(const Error& error)
{
    ErrorInfo info = describe(error);
    if (error.code == Errc::InvalidData)
        info = {tr("That's not a download"), tr("The link doesn't lead to a zip file.")};
    return info;
}

void showError(const ErrorInfo& info)
{
    if (info.title == tr("Cancelled"))
    {
        ui::toast(info.title);
        return;
    }
    App::get().scenes().emplace<ui::Dialog>(gfx::Icon::Warning, gfx::color::kWarning, info.title, info.message,
                                            std::vector<ui::DialogButton>{{tr("OK"), nullptr}});
}

} // namespace tz::scenes
