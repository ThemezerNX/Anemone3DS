// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace tz
{

// What went wrong, in terms the UI can explain to the user.
enum class Errc : uint8_t
{
    NotFound,     // a file, folder or item that should be there isn't
    InvalidData,  // a file is there but isn't what it should be
    TooLarge,     // a file is bigger than its destination allows
    Unsupported,  // valid, but not something the app can handle (a zip64, a 3-channel BCSTM...)
    Io,           // the SD card or the system storage failed
    NoSpace,      // the SD card is full
    OutOfMemory,
    Cancelled,    // the user cancelled
    NoNetwork,    // not connected to the internet
    Network,      // a connection or HTTP error
    Tls,          // the server's certificate couldn't be verified (often the console's clock)
    Http,         // the server answered with an error status
    Busy,         // something else is using it (the camera, the HOME Menu's files...)
    NotAllowed,   // the system refused (missing service access, region without themes...)
};

struct Error
{
    Errc code = Errc::Io;
    // The system's code (a libctru Result, a curl code, an HTTP status), for diagnostics.
    int32_t system = 0;
    // A short note for the log and for bug reports, in English. Never shown as the only explanation.
    std::string detail;
};

template <class T = void>
using Expected = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(Errc code, std::string detail = {}, int32_t system = 0)
{
    return std::unexpected(Error{code, system, std::move(detail)});
}

[[nodiscard]] inline std::unexpected<Error> fail(Error error)
{
    return std::unexpected(std::move(error));
}

const char* errcName(Errc code);

} // namespace tz

// Returns the error of an Expected expression from the calling function, or evaluates to its value.
#define TZ_TRY(expr)                                                                                                   \
    ({                                                                                                                 \
        auto&& tz_try_result_ = (expr);                                                                                \
        if (!tz_try_result_)                                                                                           \
            return ::std::unexpected(std::move(tz_try_result_).error());                                              \
        std::move(tz_try_result_).value();                                                                             \
    })

// The same, for Expected<void>.
#define TZ_CHECK(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        auto&& tz_check_result_ = (expr);                                                                              \
        if (!tz_check_result_)                                                                                         \
            return ::std::unexpected(std::move(tz_check_result_).error());                                            \
    } while (0)
