// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "scenes/errors.hpp"
#include "services/qr.hpp"

#include <citro3d.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz::scenes
{

// The QR code tab: the outer camera on the top screen, read until it sees a QR code.
class QrView
{
public:
    QrView();
    ~QrView();
    QrView(const QrView&) = delete;
    QrView& operator=(const QrView&) = delete;

    // Turns the camera on, or says why it can't be; stop() turns it off.
    void start();
    void stop();
    bool scanning() const { return scanner_ != nullptr; }

    // Every frame while shown: shows the newest picture, and returns the text of a QR code once one is read (the
    // camera stops then, until start()).
    std::optional<std::string> update();

    // The camera's picture fills the top screen.
    bool showsPicture() const { return scanner_ && hasFrame_; }
    void drawTop();
    void drawBottom(float top, float bottom);

private:
    std::unique_ptr<QrScanner> scanner_;
    std::optional<ErrorInfo> problem_; // why the camera can't be used
    bool scanned_ = false;            // a code was read: the camera waits to be started again
    C3D_Tex texture_{};
    bool textureReady_ = false;
    bool hasFrame_ = false;
    std::vector<uint16_t> frame_;
};

} // namespace tz::scenes
