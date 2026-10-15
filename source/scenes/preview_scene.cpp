// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/preview_scene.hpp"

#include "app/strings.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "sys/audio.hpp"
#include "sys/log.hpp"
#include "ui/overlays.hpp"
#include "ui/widgets.hpp"

#include <algorithm>

namespace tz::scenes
{

using namespace gfx;

namespace
{

class FullPreview final : public ui::Scene
{
public:
    explicit FullPreview(PreviewSource source) : source_(std::move(source))
    {
        source_.previews->want(source_.key, source_.loader, source_.lane);
        startMusic();
    }

    ~FullPreview() override
    {
        musicJob_.cancel();
        player_.reset();
    }

    void update(ui::Input& input, float dt) override
    {
        hintsShown_ = std::max(0.0f, hintsShown_ - dt);
        hints_ = {{KEY_B, tr("Close")}};
        if (player_)
            hints_.push_back({KEY_X, paused_ ? tr("Play") : tr("Pause")});
        if (hintsShown_ > 0)
            hintBar_.update(input, hints_);
        if (input.pressed(KEY_B | KEY_Y | KEY_A) || (input.tap && hintsShown_ <= 0))
        {
            close();
            return;
        }
        if (input.pressed(KEY_X) && player_)
        {
            paused_ = !paused_;
            player_->setPaused(paused_);
        }
        if (input.touching || input.down)
            hintsShown_ = 2.5f;
    }

    void drawTop() override
    {
        fill(color::kBackground);
        const PreviewPictures* pictures = source_.previews->get(source_.key);
        if (pictures && pictures->top)
            pictures->top->draw(0, 0, metric::kTopWidth, metric::kHeight);
        else if (pictures && pictures->missing)
            drawText(tr("This one has no preview."), metric::kTopWidth / 2, textTop(metric::kHeight / 2, metric::kTextNormal),
                     metric::kTextNormal, color::kTextDim, Align::Center);
        else
            spinner(metric::kTopWidth / 2, metric::kHeight / 2, 14, color::kText);
    }

    void drawBottom() override
    {
        fill(color::kBackground);
        const PreviewPictures* pictures = source_.previews->get(source_.key);
        if (pictures && pictures->bottom)
            pictures->bottom->draw(0, 0, metric::kBottomWidth, metric::kHeight);
        // the hints for a moment, then the preview alone; a press or a touch brings them back
        hintBar_.draw(hints_, std::min(1.0f, hintsShown_ * 2));
    }

private:
    void startMusic()
    {
        if (source_.music && !audio::available())
            ui::toast(tr("No sound: the DSP firmware is missing"), ui::ToastKind::Warning);
        if (!source_.music || !audio::available())
            return;
        musicJob_ = jobs::async(
            source_.musicLane,
            [open = source_.music](const CancelToken&) {
                return std::make_shared<Expected<std::unique_ptr<StreamReader>>>(open());
            },
            [this](std::shared_ptr<Expected<std::unique_ptr<StreamReader>>> stream) {
                musicJob_.release();
                if (!*stream)
                {
                    log::writeError("preview music", stream->error());
                    return;
                }
                player_ = audio::Player::play(std::move(**stream), audio::Player::Options{});
            });
    }

    PreviewSource source_;
    std::unique_ptr<audio::Player> player_;
    JobGuard musicJob_;
    bool paused_ = false;
    float hintsShown_ = 2.5f;
    ui::HintBar hintBar_;
    std::vector<ui::Hint> hints_;
};

} // namespace

std::unique_ptr<ui::Scene> makePreviewScene(PreviewSource source)
{
    return std::make_unique<FullPreview>(std::move(source));
}

} // namespace tz::scenes
