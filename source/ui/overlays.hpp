// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// The scenes that open over another: a sheet of actions, a question, a task in progress, and passing messages.

#pragma once

#include "core/cancel.hpp"
#include "gfx/draw.hpp"
#include "sys/jobs.hpp"
#include "sys/net.hpp"
#include "sys/thread.hpp"
#include "ui/scene.hpp"
#include "ui/widgets.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tz::ui
{

// The actions for something, sliding up over the bottom screen.
struct Action
{
    gfx::Icon icon = gfx::Icon::Check;
    std::string label;
    std::string detail; // a second, smaller line
    bool enabled = true;
    bool danger = false;
    std::function<void()> run;
};

// The actions as rows sliding up over the bottom screen; the top screen, dimmed, names the item and says what the
// selected action does.
class ActionSheet final : public Scene
{
public:
    ActionSheet(std::string title, std::vector<Action> actions, gfx::Color accent);
    void update(Input& input, float dt) override;
    void drawTop() override;
    void drawBottom() override;
    bool covers() const override { return false; }

private:
    float panelTop() const;
    int visibleRows() const;
    std::string title_;
    std::vector<Action> actions_;
    gfx::Color accent_;
    int selected_ = 0;
    int first_ = 0; // the first row shown, when they don't all fit
    float appear_ = 0;
    std::function<void()> chosen_;
    bool closing_ = false;
};

// Along the bottom of the top screen, over what's there dimmed: a title and a line under it, for what's selected in
// a list of actions on the bottom screen.
void drawCaption(std::string_view title, std::string_view detail, float alpha);

// A question or a message with one or two buttons. B always picks the first (cancelling) button.
struct DialogButton
{
    std::string label;
    std::function<void()> run;
    bool danger = false;
};

class Dialog final : public Scene
{
public:
    // With one button, it's a message; with two, [0] is the cancelling one (B) and [1] the confirming one (A).
    Dialog(gfx::Icon icon, gfx::Color iconColor, std::string title, std::string message,
           std::vector<DialogButton> buttons);
    void update(Input& input, float dt) override;
    void drawTop() override;
    void drawBottom() override;
    bool covers() const override { return false; }

private:
    void choose(size_t button);
    gfx::Icon icon_;
    gfx::Color iconColor_;
    std::string title_;
    std::string message_;
    std::vector<DialogButton> buttons_;
    float appear_ = 0;
    float buttonsY_ = 0;
};

// What a task tells the progress overlay as it goes, from its worker thread.
class TaskProgress
{
public:
    void set(int done, int total)
    {
        done_ = done;
        total_ = total;
    }
    void setFraction(float fraction) { set(static_cast<int>(fraction * 1000), 1000); }
    void setStatus(std::string status);
    std::string status() const;
    float fraction() const
    {
        const uint64_t bytes = download.total.load(std::memory_order_relaxed);
        if (bytes > 0)
            return static_cast<float>(download.received.load(std::memory_order_relaxed)) / static_cast<float>(bytes);
        return total_ > 0 ? static_cast<float>(done_) / total_ : -1.0f;
    }

    // A download the task runs: its bytes make the progress while they're known.
    net::Progress download;

private:
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    mutable Mutex mutex_;
    std::string status_;
};

// A task that runs on a worker while this shows its progress. The HOME button is held off while it runs when
// the task writes the system's files, so nothing is left half-written.
class ProgressOverlay final : public Scene
{
public:
    struct Options
    {
        std::string title;
        Lane lane = Lane::Io;
        bool cancellable = false;
        bool blockHome = false;
        gfx::Color accent = gfx::color::kPrimary;
    };

    // work runs on the lane's worker and returns what finish() then gets on the main thread.
    template <class Result>
    static std::unique_ptr<ProgressOverlay> start(Options options,
                                                  std::function<Result(TaskProgress&, const CancelToken&)> work,
                                                  std::function<void(Result)> finish)
    {
        auto overlay = std::unique_ptr<ProgressOverlay>(new ProgressOverlay(std::move(options)));
        auto progress = overlay->progress_;
        auto done = overlay->done_;
        // The job is queued with a token of its own that is never cancelled, so it always runs and always reports
        // back; the overlay's token only asks the work to stop early.
        jobs::run(overlay->options_.lane, CancelToken{},
                  [work = std::move(work), finish = std::move(finish), progress, done,
                   token = overlay->token_](const CancelToken&) {
                      auto result = std::make_shared<Result>(work(*progress, token));
                      jobs::post([finish, result, done] {
                          done->store(true);
                          finish(std::move(*result));
                      });
                  });
        return overlay;
    }

    ~ProgressOverlay() override;
    void update(Input& input, float dt) override;
    void drawTop() override;
    void drawBottom() override;
    bool covers() const override { return false; }

private:
    explicit ProgressOverlay(Options options);
    Options options_;
    std::shared_ptr<TaskProgress> progress_ = std::make_shared<TaskProgress>();
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(false);
    CancelToken token_;
    float appear_ = 0;
    float shown_ = 0; // the bar eases towards the progress
    bool cancelRequested_ = false;
};

// Short messages that fade by themselves, over everything at the bottom of the bottom screen.
enum class ToastKind : uint8_t
{
    Info,
    Success,
    Warning,
    Error,
};

void toast(std::string message, ToastKind kind = ToastKind::Info);
void updateToasts(float dt);
void drawToasts();

} // namespace tz::ui
