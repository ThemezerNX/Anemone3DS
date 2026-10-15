// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/frame.hpp"

#include "app/app.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tz::scenes
{

using namespace gfx;

void Backdrop::set(std::shared_ptr<Picture> blur, Color accent)
{
    if (blur == current_.picture && accent == current_.accent)
        return;
    previous_ = current_;
    current_ = Layer{std::move(blur), accent};
    fade_ = 0;
}

void Backdrop::tick(float dt)
{
    fade_ = std::min(1.0f, fade_ + dt * 3.5f);
    if (fade_ >= 1)
        previous_ = {};
}

void Backdrop::drawLayer(const Layer& layer, float alpha) const
{
    const float width = screenWidth();
    if (layer.picture)
    {
        // the blur is of the top screen: the bottom screen shows its middle
        const float offset = (metric::kTopWidth - width) / 2;
        layer.picture->draw(-offset, 0, metric::kTopWidth, metric::kHeight, alpha);
    }
    else
    {
        gradientV(0, 0, width, metric::kHeight, withAlpha(mix(color::kBackground, layer.accent, 0.22f), alpha),
                  withAlpha(color::kBackground, alpha));
    }
}

void Backdrop::draw(float dim) const
{
    fill(color::kBackground);
    if (fade_ < 1)
        drawLayer(previous_, 1.0f);
    drawLayer(current_, fade_);
    fill(withAlpha(color::kBackground, dim));
}

void drawStatusBar(float alpha)
{
    const float height = metric::kStatusBarHeight;
    const float middle = height / 2;
    gradientV(0, 0, metric::kTopWidth, height + 8, withAlpha(rgba(12, 10, 9, 190), alpha), rgba(12, 10, 9, 0));
    brand(Brand::Logo, 8, std::round(middle - brandHeight(Brand::Logo) / 2), alpha);

    const App::Status& status = App::get().status();
    const float iconTop = std::round(middle - metric::kIcon / 2);
    float x = metric::kTopWidth - 8;

    // battery: Lucide's outline, filled to the charge level
    x -= metric::kIcon;
    const Color text = withAlpha(color::kText, alpha);
    if (status.charging)
    {
        icon(Icon::BatteryCharging, x, iconTop, metric::kIcon, text);
    }
    else
    {
        icon(Icon::Battery, x, iconTop, metric::kIcon, text);
        // the outline's inside is from (4.5, 9.5) of its 24-unit grid, 11 by 5
        const float unit = metric::kIcon / 24.0f;
        const float level = static_cast<float>(status.battery) / 5.0f;
        const Color charge = status.battery <= 1 ? withAlpha(color::kDanger, alpha) : text;
        rect(x + 4.5f * unit, iconTop + 9.5f * unit, std::round(11.0f * unit * level), 5.0f * unit, charge);
    }

    x -= 6 + metric::kIcon;
    icon(status.online ? Icon::Wifi : Icon::WifiOff, x, iconTop, metric::kIcon,
         status.online ? text : withAlpha(color::kTextFaint, alpha));

    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", status.hour, status.minute);
    drawText(clock, x - 6, textTop(middle, metric::kTextSmall), metric::kTextSmall, text, Align::Right);
}

namespace row
{

void highlight(float x, float y, float w, float h, Color accent)
{
    roundedRect(x + 4, y + 2, w - 8, h - 4, metric::kRadius, color::kSelection);
    roundedRect(x + 6, y + 12, 3, h - 24, 1.5f, accent);
}

float iconTop(float y, float h)
{
    return std::round(y + (h - kIconSize) / 2);
}

void placeholder(float x, float y, float h, Icon kind)
{
    const float top = iconTop(y, h);
    roundedRect(x + kIconX, top, kIconSize, kIconSize, 6, color::kSurfaceRaised);
    icon(kind, x + kIconX + (kIconSize - metric::kIcon) / 2, top + (kIconSize - metric::kIcon) / 2, metric::kIcon,
         color::kTextFaint);
}

void lines(std::string_view name, std::string_view detail, float x, float y, float h, float right, bool dimmed)
{
    const float width = right - (x + kTextX);
    const float middle = y + h / 2;
    if (detail.empty())
    {
        drawText(name, x + kTextX, textTop(middle, metric::kTextNormal), metric::kTextNormal,
                 dimmed ? color::kTextFaint : color::kText, Align::Left, width);
        return;
    }
    drawText(name, x + kTextX, textTop(middle - 7.5f, metric::kTextNormal), metric::kTextNormal,
             dimmed ? color::kTextFaint : color::kText, Align::Left, width);
    drawText(detail, x + kTextX, textTop(middle + 8, metric::kTextSmall), metric::kTextSmall,
             dimmed ? color::kTextFaint : color::kTextDim, Align::Left, width);
}

float status(Icon which, std::string_view text, Color tint, float right, float y, float h)
{
    const float middle = y + h / 2;
    const float width = textWidth(text, metric::kTextSmall);
    right -= width;
    drawText(text, right, textTop(middle, metric::kTextSmall), metric::kTextSmall, tint);
    right -= 3 + metric::kIcon;
    icon(which, right, std::round(middle - metric::kIcon / 2), metric::kIcon, tint);
    return right - 8;
}

float mark(Icon which, Color tint, float right, float y, float h)
{
    right -= metric::kIcon;
    icon(which, right, std::round(y + h / 2 - metric::kIcon / 2), metric::kIcon, tint);
    return right - 8;
}

} // namespace row

void drawEmptyState(Icon which, Color iconColor, std::string_view title, std::string_view message, float top,
                    float bottom)
{
    const float width = metric::kBottomWidth;
    const auto lines = wrap(message, width - 48, metric::kTextSmall, 4);
    const float messageHeight = static_cast<float>(lines.size()) * lineHeight(metric::kTextSmall);
    const float height = metric::kIconLarge + 12 + lineHeight(metric::kTextNormal) + 6 + messageHeight;
    float y = std::round((top + bottom - height) / 2);
    icon(which, (width - metric::kIconLarge) / 2, y, metric::kIconLarge, iconColor);
    y += metric::kIconLarge + 12;
    drawText(title, width / 2, y, metric::kTextNormal, color::kText, Align::Center, width - 32);
    y += lineHeight(metric::kTextNormal) + 6;
    drawWrapped(message, 24, y, width - 48, metric::kTextSmall, color::kTextDim, 4, Align::Center);
}

} // namespace tz::scenes
