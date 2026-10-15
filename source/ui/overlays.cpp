// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "ui/overlays.hpp"

#include "gfx/renderer.hpp"
#include "gfx/text.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace tz::ui
{

using namespace gfx;

namespace
{

float ease(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

void approach(float& value, float target, float dt, float speed = 7.0f)
{
    value += (target - value) * std::min(1.0f, dt * speed);
    if (std::fabs(target - value) < 0.002f)
        value = target;
}

constexpr float kSheetRow = 36;
constexpr float kSheetPadding = 6;
constexpr int kSheetMaxRows = 6;
constexpr float kButtonHeight = 30;

} // namespace

void drawCaption(std::string_view title, std::string_view detail, float alpha)
{
    fill(withAlpha(color::kScrim, 0.55f * alpha));
    const float height = detail.empty() ? 40 : 60;
    const float top = metric::kHeight - height;
    rect(0, top, metric::kTopWidth, height, withAlpha(color::kSurfaceSolid, 0.95f * alpha));
    rect(0, top, metric::kTopWidth, 1, withAlpha(color::kDivider, alpha));
    const float width = metric::kTopWidth - 40;
    drawText(title, 20, textTop(top + 20, metric::kTextLarge), metric::kTextLarge, withAlpha(color::kText, alpha),
             Align::Left, width);
    if (!detail.empty())
        drawText(detail, 20, textTop(top + 42, metric::kTextNormal), metric::kTextNormal,
                 withAlpha(color::kTextDim, alpha), Align::Left, width);
}

// ---------------------------------------------------------------------------------------------------------------------
// ActionSheet

ActionSheet::ActionSheet(std::string title, std::vector<Action> actions, Color accent)
    : title_(std::move(title)), actions_(std::move(actions)), accent_(accent)
{
    // start on the first action that can be taken
    for (size_t i = 0; i < actions_.size(); ++i)
    {
        if (actions_[i].enabled)
        {
            selected_ = static_cast<int>(i);
            break;
        }
    }
}

int ActionSheet::visibleRows() const
{
    return std::min(static_cast<int>(actions_.size()), kSheetMaxRows);
}

float ActionSheet::panelTop() const
{
    const float height = kSheetRow * static_cast<float>(visibleRows()) + 2 * kSheetPadding;
    return std::round(metric::kHeight - height * ease(appear_));
}

void ActionSheet::update(Input& input, float dt)
{
    if (closing_)
    {
        approach(appear_, 0, dt, 12);
        if (appear_ < 0.05f)
        {
            close();
            if (chosen_)
                chosen_();
        }
        return;
    }
    approach(appear_, 1, dt, 12);

    const int count = static_cast<int>(actions_.size());
    auto step = [&](int direction) {
        for (int i = 1; i <= count; ++i)
        {
            const int next = (selected_ + direction * i + count) % count;
            if (actions_[next].enabled)
            {
                selected_ = next;
                return;
            }
        }
    };
    if (input.repeated(KEY_UP))
        step(-1);
    else if (input.repeated(KEY_DOWN))
        step(1);
    // the selection stays in the rows shown
    first_ = std::clamp(first_, selected_ - visibleRows() + 1, selected_);

    auto choose = [&](int index) {
        if (index < 0 || index >= count || !actions_[index].enabled)
            return;
        chosen_ = actions_[index].run;
        closing_ = true;
    };

    if (input.pressed(KEY_A))
        choose(selected_);
    else if (input.pressed(KEY_B))
        closing_ = true;
    else if (input.tap)
    {
        const float top = panelTop() + kSheetPadding;
        if (input.y < panelTop())
        {
            closing_ = true;
        }
        else if (input.y >= top)
        {
            const int row = first_ + static_cast<int>((input.y - top) / kSheetRow);
            if (row >= first_ && row < first_ + visibleRows() && actions_[row].enabled)
            {
                selected_ = row;
                choose(row);
            }
        }
        input.consumeTouch();
    }
}

void ActionSheet::drawTop()
{
    const Action& action = actions_[static_cast<size_t>(selected_)];
    drawCaption(title_, action.detail, ease(appear_));
}

void ActionSheet::drawBottom()
{
    fill(withAlpha(color::kScrim, ease(appear_)));
    const float top = panelTop();
    roundedRect(4, top, metric::kBottomWidth - 8, metric::kHeight - top + 12, 12, color::kSurfaceSolid);

    float y = top + kSheetPadding;
    for (int i = first_; i < first_ + visibleRows(); ++i)
    {
        const Action& action = actions_[static_cast<size_t>(i)];
        const bool selected = i == selected_;
        if (selected)
            roundedRect(10, y + 2, metric::kBottomWidth - 20, kSheetRow - 4, metric::kRadius, color::kSelection);
        const float middle = y + kSheetRow / 2;
        const Color tint = !action.enabled ? color::kTextFaint
                                           : (action.danger ? color::kDanger : (selected ? accent_ : color::kTextDim));
        icon(action.icon, 22, std::round(middle - metric::kIcon / 2), metric::kIcon, tint);
        const Color textColor = action.enabled ? (action.danger ? color::kDanger : color::kText) : color::kTextFaint;
        drawText(action.label, 48, textTop(middle, metric::kTextNormal), metric::kTextNormal, textColor, Align::Left,
                 metric::kBottomWidth - 68);
        y += kSheetRow;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Dialog

Dialog::Dialog(Icon icon, Color iconColor, std::string title, std::string message, std::vector<DialogButton> buttons)
    : icon_(icon), iconColor_(iconColor), title_(std::move(title)), message_(std::move(message)),
      buttons_(std::move(buttons))
{
    if (buttons_.empty())
        buttons_.push_back({"OK", nullptr});
}

void Dialog::choose(size_t button)
{
    close();
    if (button < buttons_.size() && buttons_[button].run)
        buttons_[button].run();
}

void Dialog::update(Input& input, float dt)
{
    approach(appear_, 1, dt, 12);
    if (buttons_.size() == 1)
    {
        if (input.pressed(KEY_A | KEY_B))
            choose(0);
    }
    else
    {
        if (input.pressed(KEY_A))
            choose(1);
        else if (input.pressed(KEY_B))
            choose(0);
    }
    if (input.tap && input.y >= buttonsY_ && input.y < buttonsY_ + kButtonHeight)
    {
        const float width = (metric::kBottomWidth - 40) / static_cast<float>(buttons_.size());
        const int index = static_cast<int>((input.x - 20) / width);
        if (input.x >= 20 && index >= 0 && index < static_cast<int>(buttons_.size()))
            choose(static_cast<size_t>(index));
        input.consumeTouch();
    }
}

void Dialog::drawTop()
{
    fill(withAlpha(color::kScrim, 0.45f * appear_));
}

void Dialog::drawBottom()
{
    fill(withAlpha(color::kScrim, appear_));
    constexpr float kIconSize = 32;
    const float width = metric::kBottomWidth - 24;
    const float messageWidth = width - 32;
    const auto lines = wrap(message_, messageWidth, metric::kTextSmall, 6);
    const float messageHeight = static_cast<float>(lines.size()) * lineHeight(metric::kTextSmall);
    const float height = 16 + kIconSize + 10 + lineHeight(metric::kTextNormal) + 6 + messageHeight + 16 +
                         kButtonHeight + 14;
    const float x = 12;
    const float y = std::round((metric::kHeight - height) / 2 + (1 - ease(appear_)) * 20);

    roundedRect(x, y, width, height, 12, color::kSurfaceSolid);
    float at = y + 16;
    icon(icon_, x + (width - kIconSize) / 2, at, kIconSize, iconColor_);
    at += kIconSize + 10;
    drawText(title_, x + width / 2, at, metric::kTextNormal, color::kText, Align::Center, messageWidth);
    at += lineHeight(metric::kTextNormal) + 6;
    drawWrapped(message_, x + 16, at, messageWidth, metric::kTextSmall, color::kTextDim, 6, Align::Center);

    buttonsY_ = y + height - 14 - kButtonHeight;
    const float slotWidth = (metric::kBottomWidth - 40) / static_cast<float>(buttons_.size());
    const float middle = buttonsY_ + kButtonHeight / 2;
    for (size_t i = 0; i < buttons_.size(); ++i)
    {
        const DialogButton& button = buttons_[i];
        const bool primary = i + 1 == buttons_.size() && buttons_.size() > 1;
        const float bx = std::round(20 + slotWidth * static_cast<float>(i) + 3);
        const float bw = std::round(slotWidth - 6);
        const Color fillColor = primary ? (button.danger ? color::kDanger : color::kPrimary) : color::kSurfaceHigh;
        roundedRect(bx, buttonsY_, bw, kButtonHeight, kButtonHeight / 2, fillColor);
        const u32 key = buttons_.size() == 1 ? KEY_A : (i == 0 ? KEY_B : KEY_A);
        const Color textColor = primary ? color::kOnAccent : color::kText;
        const std::string label = ellipsize(button.label, bw - 12 - hintWidth(key, ""), metric::kTextNormal);
        drawHint(key, label, bx + (bw - hintWidth(key, label)) / 2, middle, textColor, textColor);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// ProgressOverlay

void TaskProgress::setStatus(std::string status)
{
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
}

std::string TaskProgress::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

ProgressOverlay::ProgressOverlay(Options options) : options_(std::move(options))
{
    if (options_.blockHome)
        aptSetHomeAllowed(false);
}

ProgressOverlay::~ProgressOverlay()
{
    if (options_.blockHome)
        aptSetHomeAllowed(true);
}

void ProgressOverlay::update(Input& input, float dt)
{
    approach(appear_, 1, dt, 10);
    const float fraction = progress_->fraction();
    if (fraction >= 0)
        approach(shown_, fraction, dt, 8);
    if (options_.cancellable && !cancelRequested_ && input.pressed(KEY_B))
    {
        cancelRequested_ = true;
        token_.cancel();
    }
    if (options_.blockHome && aptCheckHomePressRejected())
        toast("Wait a moment: this has to finish first", ToastKind::Warning);
    if (done_->load())
        close();
}

void ProgressOverlay::drawTop()
{
    fill(withAlpha(color::kScrim, 0.55f * appear_));
}

void ProgressOverlay::drawBottom()
{
    fill(withAlpha(color::kScrim, appear_));
    const float width = metric::kBottomWidth - 24;
    const float height = 112;
    const float x = 12;
    const float y = (metric::kHeight - height) / 2;
    roundedRect(x, y, width, height, 12, color::kSurfaceSolid);

    drawText(options_.title, x + 16, textTop(y + 24, metric::kTextNormal), metric::kTextNormal, color::kText,
             Align::Left, width - 32);
    const std::string status = progress_->status();
    drawText(status, x + 16, textTop(y + 44, metric::kTextSmall), metric::kTextSmall, color::kTextDim, Align::Left,
             width - 32);

    const float fraction = progress_->fraction();
    if (fraction >= 0)
    {
        progressBar(x + 16, y + 64, width - 32, 8, shown_, color::kSurfaceHigh, options_.accent);
    }
    else
    {
        // nothing to measure: a stripe going back and forth
        const float t = std::fmod(time() * 0.8f, 2.0f);
        const float position = t < 1 ? t : 2 - t;
        roundedRect(x + 16, y + 64, width - 32, 8, 4, color::kSurfaceHigh);
        roundedRect(x + 16 + (width - 32 - 70) * position, y + 64, 70, 8, 4, options_.accent);
    }

    if (options_.cancellable)
    {
        const char* label = cancelRequested_ ? "Cancelling..." : "Cancel";
        drawHint(KEY_B, label, x + width - 16 - hintWidth(KEY_B, label), y + height - 18, color::kText,
                 color::kTextDim);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Toasts

namespace
{
struct Toast
{
    std::string message;
    ToastKind kind;
    float age = 0;
};
std::vector<Toast> toasts;
constexpr float kToastTime = 2.8f;
constexpr float kToastFade = 0.25f;
} // namespace

void toast(std::string message, ToastKind kind)
{
    // the same message again restarts it instead of stacking
    for (Toast& t : toasts)
    {
        if (t.message == message)
        {
            t.age = std::min(t.age, kToastFade);
            return;
        }
    }
    toasts.push_back({std::move(message), kind});
    if (toasts.size() > 3)
        toasts.erase(toasts.begin());
}

void updateToasts(float dt)
{
    for (Toast& t : toasts)
        t.age += dt;
    std::erase_if(toasts, [](const Toast& t) { return t.age > kToastTime; });
}

void drawToasts()
{
    float bottom = metric::kHeight - metric::kHintBarHeight - 6;
    for (auto it = toasts.rbegin(); it != toasts.rend(); ++it)
    {
        const Toast& t = *it;
        const float in = std::clamp(t.age / kToastFade, 0.0f, 1.0f);
        const float out = std::clamp((kToastTime - t.age) / kToastFade, 0.0f, 1.0f);
        const float alpha = std::min(in, out);
        const float size = metric::kTextSmall;
        const auto lines = wrap(t.message, metric::kBottomWidth - 64, size, 2);
        const float height = static_cast<float>(lines.size()) * lineHeight(size) + 12;
        float width = 0;
        for (const std::string& line : lines)
            width = std::max(width, textWidth(line, size));
        width += 44;
        const float x = (metric::kBottomWidth - width) / 2;
        const float y = bottom - height + (1 - ease(in)) * 8;

        Icon which = Icon::Info;
        Color tint = color::kTextDim;
        switch (t.kind)
        {
        case ToastKind::Success:
            which = Icon::CheckCircle;
            tint = color::kPrimaryLight;
            break;
        case ToastKind::Warning:
            which = Icon::Warning;
            tint = color::kWarning;
            break;
        case ToastKind::Error:
            which = Icon::Error;
            tint = color::kDanger;
            break;
        case ToastKind::Info:
            break;
        }
        roundedRect(x, y, width, height, std::min(14.0f, height / 2), withAlpha(color::kSurfaceRaised, alpha));
        icon(which, x + 12, std::round(y + height / 2 - metric::kIcon / 2), metric::kIcon, withAlpha(tint, alpha));
        const float textTopY =
            lines.size() == 1 ? textTop(y + height / 2, size) : y + (height - lineHeight(size) * 2) / 2;
        drawWrapped(t.message, x + 34, textTopY, width - 44, size, withAlpha(color::kText, alpha), 2);
        bottom = y - 6;
    }
}

} // namespace tz::ui
