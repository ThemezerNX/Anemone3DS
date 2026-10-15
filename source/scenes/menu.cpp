// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/menu.hpp"

#include "app/app.hpp"
#include "app/strings.hpp"
#include "gfx/draw.hpp"
#include "gfx/renderer.hpp"
#include "gfx/text.hpp"
#include "scenes/errors.hpp"
#include "services/updater.hpp"
#include "sys/audio.hpp"
#include "ui/list_view.hpp"
#include "ui/overlays.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>

namespace tz::scenes
{

using namespace gfx;

namespace
{

constexpr float kRow = 36;

// A list of actions over the bottom screen, with the top screen describing the one selected.
class MenuScene final : public ui::Scene
{
public:
    MenuScene(std::string title, std::vector<ui::Action> actions) : title_(std::move(title)), actions_(std::move(actions))
    {
        list_.setRowHeight(kRow);
        list_.setCount(static_cast<int>(actions_.size()));
        for (size_t i = 0; i < actions_.size(); ++i)
        {
            if (actions_[i].enabled)
            {
                list_.select(static_cast<int>(i), false);
                break;
            }
        }
    }

    bool covers() const override { return false; }

    void update(ui::Input& input, float dt) override
    {
        appear_ = std::min(1.0f, appear_ + dt * 8);
        hintBar_.update(input, hints_);
        list_.setBounds(0, kHeader, metric::kBottomWidth, metric::kHeight - kHeader - metric::kHintBarHeight);
        const auto event = list_.update(input, dt);
        if (input.pressed(KEY_B | KEY_START))
        {
            close();
            return;
        }
        if (event == ui::ListView::Event::Activated)
        {
            const ui::Action& action = actions_[list_.selected()];
            if (!action.enabled)
                return;
            close();
            if (action.run)
                action.run();
        }
    }

    void drawTop() override
    {
        const ui::Action& action = actions_[list_.selected()];
        ui::drawCaption(action.label, action.detail, appear_);
    }

    void drawBottom() override
    {
        fill(withAlpha(color::kScrim, appear_));
        rect(0, 0, metric::kBottomWidth, metric::kHeight, withAlpha(color::kSurfaceSolid, 0.96f * appear_));
        drawText(title_, 16, textTop(kHeader / 2, metric::kTextNormal), metric::kTextNormal, color::kText);
        list_.draw([this](int index, float x, float y, float w, float h, bool selected) {
            const ui::Action& action = actions_[index];
            if (selected)
                roundedRect(x + 6, y + 2, w - 12, h - 4, metric::kRadius, color::kSelection);
            const float middle = y + h / 2;
            const Color tint = !action.enabled ? color::kTextFaint
                                               : (action.danger ? color::kDanger
                                                                : (selected ? color::kPrimaryLight : color::kTextDim));
            icon(action.icon, x + 18, std::round(middle - metric::kIcon / 2), metric::kIcon, tint);
            drawText(action.label, x + 44, textTop(middle, metric::kTextNormal), metric::kTextNormal,
                     action.enabled ? (action.danger ? color::kDanger : color::kText) : color::kTextFaint, Align::Left,
                     w - 60);
        });
        hintBar_.draw(hints_);
    }

private:
    static constexpr float kHeader = 32;
    std::string title_;
    std::vector<ui::Action> actions_;
    ui::ListView list_;
    ui::HintBar hintBar_;
    std::vector<ui::Hint> hints_{{KEY_A, tr("Choose")}, {KEY_B, tr("Back")}};
    float appear_ = 0;
};

void installUpdate()
{
    const auto& release = App::get().availableUpdate();
    if (!release)
        return;
    const update::InstallKind kind = update::installKind(App::get().appPath());
    if (kind == update::InstallKind::None)
    {
        showError({trf("Version {} is available", release->version),
                   trf("This copy can't update itself. Download the new version from {}.", update::kReleasesPage)});
        return;
    }
    const update::Release copy = *release;
    const char* appPath = App::get().appPath();
    App::get().scenes().push(ui::ProgressOverlay::start<Expected<void>>(
        {trf("Updating to version {}", copy.version), Lane::Network, true, kind == update::InstallKind::Cia,
         color::kPrimary},
        [copy, kind, appPath](ui::TaskProgress& task, const CancelToken& token) {
            task.setStatus(tr("Downloading and installing"));
            net::Client client;
            return update::install(client, copy, kind, appPath, &task.download, token);
        },
        [copy](Expected<void> result) {
            if (!result)
            {
                showError(describe(result.error()));
                return;
            }
            App::get().scenes().emplace<ui::Dialog>(
                Icon::CheckCircle, color::kPrimaryLight, trf("Version {} is installed", copy.version),
                tr("It starts the next time you open the app."),
                std::vector<ui::DialogButton>{{tr("Close the app"), [] { App::get().requestExit(); }}});
        }));
}

} // namespace

void openMenu(std::vector<ui::Action> pageActions)
{
    std::vector<ui::Action> actions = std::move(pageActions);
    if (const auto& release = App::get().availableUpdate())
        actions.push_back({Icon::Update, trf("Update to version {}", release->version),
                           trf("You have version {}", APP_VERSION), true, false, [] { installUpdate(); }});
    actions.push_back({Icon::Settings, tr("Settings"), {}, true, false, [] { openSettings(); }});
    actions.push_back({Icon::Info, tr("About Themezer-3DS"), tr("Version, credits and licenses"), true, false,
                       [] { openAbout(); }});
    actions.push_back({Icon::Power, tr("Close the app"), {}, true, false, [] { App::get().requestExit(); }});
    App::get().scenes().emplace<MenuScene>(tr("Menu"), std::move(actions));
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings

namespace
{

class SettingsScene final : public ui::Scene
{
public:
    SettingsScene() { list_.setRowHeight(48); }

    void update(ui::Input& input, float dt) override
    {
        hintBar_.update(input, hints_);
        list_.setBounds(0, kHeader, metric::kBottomWidth, metric::kHeight - kHeader - metric::kHintBarHeight);
        list_.setCount(2);
        const auto event = list_.update(input, dt);
        if (input.pressed(KEY_B))
        {
            App::get().saveConfig();
            close();
            return;
        }
        if (event == ui::ListView::Event::Activated)
            toggle(list_.selected());
    }

    void drawTop() override
    {
        fill(color::kBackground);
        gradientV(0, 0, metric::kTopWidth, metric::kHeight, mix(color::kBackground, color::kPrimary, 0.18f),
                  color::kBackground);
        brand(Brand::Icon, std::round((metric::kTopWidth - brandWidth(Brand::Icon)) / 2), 56);
        drawText(tr("Settings"), metric::kTopWidth / 2, textTop(146, metric::kTextLarge), metric::kTextLarge,
                 color::kText, Align::Center);
        drawText(tr("Saved in /3ds/Themezer/config.json"), metric::kTopWidth / 2, textTop(170, metric::kTextSmall),
                 metric::kTextSmall, color::kTextDim, Align::Center);
    }

    void drawBottom() override
    {
        fill(color::kSurfaceSolid);
        drawText(tr("Settings"), 16, textTop(kHeader / 2, metric::kTextNormal), metric::kTextNormal, color::kText);
        const Config& config = App::get().config();
        list_.draw([&](int index, float x, float y, float w, float h, bool selected) {
            if (selected)
                roundedRect(x + 6, y + 2, w - 12, h - 4, metric::kRadius, color::kSelection);
            const bool on = index == 0 ? config.backgroundMusic : config.checkForUpdates;
            const char* label = index == 0 ? "Background music" : "Check for updates";
            const char* detail = index == 0 ? "Themezer's own tune while you browse"
                                            : "When the app starts, it looks for a new version";
            const float middle = y + h / 2;
            icon(on ? Icon::SquareCheck : Icon::Square, x + 18, std::round(middle - metric::kIcon / 2), metric::kIcon,
                 on ? color::kPrimaryLight : color::kTextDim);
            drawText(tr(label), x + 44, textTop(middle - 7.5f, metric::kTextNormal), metric::kTextNormal,
                     color::kText, Align::Left, w - 60);
            drawText(tr(detail), x + 44, textTop(middle + 8, metric::kTextSmall), metric::kTextSmall,
                     color::kTextDim, Align::Left, w - 60);
        });
        hintBar_.draw(hints_);
    }

private:
    static constexpr float kHeader = 32;

    void toggle(int index)
    {
        Config& config = App::get().config();
        if (index == 0)
        {
            config.backgroundMusic = !config.backgroundMusic;
            if (config.backgroundMusic && audio::available())
                audio::startBackgroundMusic("romfs:/bgm.bcstm");
            else
                audio::stopBackgroundMusic();
        }
        else
        {
            config.checkForUpdates = !config.checkForUpdates;
        }
    }

    ui::ListView list_;
    ui::HintBar hintBar_;
    std::vector<ui::Hint> hints_{{KEY_A, tr("Change")}, {KEY_B, tr("Back")}};
};

// ---------------------------------------------------------------------------------------------------------------------
// About

class AboutScene final : public ui::Scene
{
public:
    AboutScene()
    {
        // the notices of the libraries, as built into the app
        if (FILE* file = std::fopen("romfs:/licenses.txt", "rb"))
        {
            char buffer[4096];
            size_t read;
            while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
                licenses_.append(buffer, read);
            std::fclose(file);
        }
        lines_ = wrap(credits() + "\n\n" + licenses_, metric::kBottomWidth - 32, metric::kTextSmall);
    }

    void update(ui::Input& input, float dt) override
    {
        hintBar_.update(input, hints_);
        const float lineStep = lineHeight(metric::kTextSmall);
        const float maxScroll = std::max(
            0.0f, static_cast<float>(lines_.size()) * lineStep - (metric::kHeight - kHeader - metric::kHintBarHeight));
        if (input.held & KEY_DOWN)
            scroll_ += 240 * dt;
        if (input.held & KEY_UP)
            scroll_ -= 240 * dt;
        if (input.repeated(KEY_RIGHT))
            scroll_ += metric::kHeight - 80;
        if (input.repeated(KEY_LEFT))
            scroll_ -= metric::kHeight - 80;
        if (input.dragging)
            scroll_ -= input.dy;
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
        if (input.pressed(KEY_B))
            close();
    }

    void drawTop() override
    {
        fill(color::kBackground);
        gradientV(0, 0, metric::kTopWidth, metric::kHeight, mix(color::kBackground, color::kSecondary, 0.16f),
                  color::kBackground);
        brand(Brand::Icon, std::round((metric::kTopWidth - brandWidth(Brand::Icon)) / 2), 22);
        drawText("Themezer-3DS", metric::kTopWidth / 2, textTop(104, metric::kTextLarge), metric::kTextLarge,
                 color::kText, Align::Center);
        drawText(trf("Version {}", APP_VERSION), metric::kTopWidth / 2, textTop(124, metric::kTextSmall),
                 metric::kTextSmall, color::kTextDim, Align::Center);
        drawWrapped(tr("A fork of Anemone3DS, rewritten. Free software under the GNU General Public License, "
                       "version 3 or later, with ABSOLUTELY NO WARRANTY. You may share and change it under that "
                       "license. Source code: github.com/ThemezerNX/themezer-3ds"),
                    32, 142, metric::kTopWidth - 64, metric::kTextSmall, color::kTextDim, 6, Align::Center);
    }

    void drawBottom() override
    {
        fill(color::kSurfaceSolid);
        drawText(tr("Credits and licenses"), 16, textTop(kHeader / 2, metric::kTextNormal), metric::kTextNormal,
                 color::kText);
        pushClip(0, kHeader, metric::kBottomWidth, metric::kHeight - kHeader - metric::kHintBarHeight);
        const float step = lineHeight(metric::kTextSmall);
        const int first = static_cast<int>(scroll_ / step);
        for (int i = std::max(0, first); i < static_cast<int>(lines_.size()); ++i)
        {
            const float y = kHeader + i * step - std::round(scroll_);
            if (y > metric::kHeight)
                break;
            drawText(lines_[i], 16, y, metric::kTextSmall, color::kTextDim);
        }
        popClip();
        hintBar_.draw(hints_);
    }

private:
    static std::string credits()
    {
        return std::string(tr("Themezer-3DS is made by Themezer. It is a fork of Anemone3DS by Alex Taber (astronautlevel2) and "
                  "the Anemone3DS contributors: LiquidFenrir, daedreth, helloman892, ZetaDesigns, KennLDN, Wizzrobes, "
                  "saibotu, TurtleP, FrozenChen, luigoalma, uyuiyu, GreatWizard, joel16, thedax, Wryyyong, and the "
                  "translators cooolgamer, iveurne, Tristanabs and Angelpro09_xd. Thank you.")) +
               "\n\n" +
               tr("The formats come from 3dbrew, yellows8's 3ds_homemenu_extdatatool (themes), GYTB by MrCheeze and "
                  "Advanced Badge Editor by AntiMach (badges), and Luma3DS (splashes). Icons: Lucide.");
    }

    static constexpr float kHeader = 32;
    std::string licenses_;
    std::vector<std::string> lines_;
    float scroll_ = 0;
    ui::HintBar hintBar_;
    std::vector<ui::Hint> hints_{{KEY_DOWN, tr("Scroll")}, {KEY_B, tr("Back")}};
};

} // namespace

void openSettings()
{
    App::get().scenes().emplace<SettingsScene>();
}

void openAbout()
{
    App::get().scenes().emplace<AboutScene>();
}

} // namespace tz::scenes
