// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Scanning QR codes with the outer camera.

#pragma once

#include "core/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz
{

class QrScanner
{
public:
    static constexpr int kWidth = 400;
    static constexpr int kHeight = 240;

    // Starts the outer camera, and looks for QR codes in its pictures on a thread of its own.
    // Busy when the camera is in use, NotAllowed when the camera service can't be used (some ways of starting
    // homebrew don't allow it).
    static Expected<std::unique_ptr<QrScanner>> start();
    ~QrScanner(); // stops the camera

    QrScanner(const QrScanner&) = delete;
    QrScanner& operator=(const QrScanner&) = delete;

    // Copies the newest picture, kWidth x kHeight RGB565 in rows from the top, if there is one since the last call.
    bool latestFrame(std::vector<uint16_t>& frame);

    // The text of the first QR code found.
    std::optional<std::string> result() const;

    struct Impl;

private:
    explicit QrScanner(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace tz
