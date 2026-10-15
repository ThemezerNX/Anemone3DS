// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/qr_view.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "core/image.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "gfx/texture.hpp"
#include "scenes/frame.hpp"

namespace tz::scenes
{

using namespace gfx;

QrView::QrView()
{
    textureReady_ = C3D_TexInit(&texture_, 512, 256, GPU_RGB565);
    if (textureReady_)
        C3D_TexSetFilter(&texture_, GPU_LINEAR, GPU_LINEAR);
}

QrView::~QrView()
{
    scanner_.reset();
    if (textureReady_)
        C3D_TexDelete(&texture_);
}

void QrView::start()
{
    if (scanner_)
        return;
    scanned_ = false;
    hasFrame_ = false;
    problem_.reset();
    auto scanner = QrScanner::start();
    if (scanner)
    {
        scanner_ = std::move(*scanner);
        return;
    }
    if (scanner.error().code == Errc::NotAllowed && App::get().homebrew())
        problem_ = ErrorInfo{tr("The camera isn't available"),
                             tr("Started this way, the app can't use the camera. Install the CIA version to scan QR "
                                "codes.")};
    else if (scanner.error().code == Errc::Busy)
        problem_ = ErrorInfo{tr("The camera is busy"), tr("Something else is using the camera. Try again in a moment.")};
    else
        problem_ = describe(scanner.error());
}

void QrView::stop()
{
    scanner_.reset();
    hasFrame_ = false;
}

std::optional<std::string> QrView::update()
{
    if (!scanner_)
        return std::nullopt;
    if (textureReady_ && scanner_->latestFrame(frame_))
    {
        // rows of RGB565 into the GPU's tiles, a frame at a time
        auto* texels = static_cast<uint16_t*>(texture_.data);
        for (int y = 0; y < QrScanner::kHeight; ++y)
            for (int x = 0; x < QrScanner::kWidth; ++x)
                texels[tiledIndex(x, y, 512)] = frame_[static_cast<size_t>(y) * QrScanner::kWidth + x];
        C3D_TexFlush(&texture_);
        hasFrame_ = true;
    }
    if (auto text = scanner_->result())
    {
        scanned_ = true;
        stop(); // the camera stops at once
        return text;
    }
    return std::nullopt;
}

void QrView::drawTop()
{
    if (!showsPicture())
    {
        if (scanner_)
            spinner(metric::kTopWidth / 2, metric::kHeight / 2, 14, color::kText);
        return;
    }
    drawTexturePart(&texture_, 0, 0, QrScanner::kWidth, QrScanner::kHeight, 0, 0, metric::kTopWidth, metric::kHeight,
                    1.0f);

    // corners to aim with
    const float size = 150;
    const float x = (metric::kTopWidth - size) / 2;
    const float y = (metric::kHeight - size) / 2;
    const Color edge = withAlpha(color::kPrimaryLight, 0.9f);
    const float l = 24;
    const float t = 3;
    rect(x, y, l, t, edge);
    rect(x, y, t, l, edge);
    rect(x + size - l, y, l, t, edge);
    rect(x + size - t, y, t, l, edge);
    rect(x, y + size - t, l, t, edge);
    rect(x, y + size - l, t, l, edge);
    rect(x + size - l, y + size - t, l, t, edge);
    rect(x + size - t, y + size - l, t, l, edge);
}

void QrView::drawBottom(float top, float bottom)
{
    if (problem_)
    {
        drawEmptyState(Icon::Qr, color::kTextDim, problem_->title, problem_->message, top, bottom);
        return;
    }
    if (scanned_)
    {
        drawEmptyState(Icon::Qr, color::kPrimaryLight, tr("Scanned"), tr("Press A to scan another QR code."), top,
                       bottom);
        return;
    }
    drawEmptyState(Icon::Qr, color::kPrimaryLight, tr("Scan a QR code"),
                   tr("Point the outer camera at the QR code of a theme, splash screen or badge set on themezer.net. "
                      "It downloads as soon as the camera reads it."),
                   top, bottom);
}

} // namespace tz::scenes
