// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

// Work off the main thread. The main thread only draws and reacts to input; everything that reads the SD card,
// decodes images or talks to the network runs on a worker and hands its result back with post(), so the UI never
// waits for it. State the UI uses is only ever touched on the main thread.

#pragma once

#include "core/cancel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace tz
{

enum class Lane : uint8_t
{
    Io,      // the SD card, zips, images
    Network, // downloads, so a slow server never holds up the SD card work
    Count,
};

namespace jobs
{

void init();
// Cancels what is queued, waits for the running jobs and stops the workers.
void shutdown();

// Runs work on the lane's worker. Higher priorities run first, in the order they were queued.
void run(Lane lane, CancelToken token, std::function<void(const CancelToken&)> work, int priority = 0);

// Runs on the main thread during the next pump(). Any thread may call it.
void post(std::function<void()> function);

// Main thread, once a frame: runs what was posted, until budgetMs has passed.
void pump(uint32_t budgetMs);

// The usual shape: work on a lane, then done() on the main thread with its result, unless cancelled by then.
template <class Work, class Done>
CancelToken async(Lane lane, Work work, Done done, int priority = 0)
{
    CancelToken token;
    run(
        lane, token,
        [work = std::move(work), done = std::move(done)](const CancelToken& t) mutable {
            auto result = work(t);
            if (t.cancelled())
                return;
            post([t, done = std::move(done), result = std::move(result)]() mutable {
                if (!t.cancelled())
                    done(std::move(result));
            });
        },
        priority);
    return token;
}

} // namespace jobs

// Cancels its token when it goes out of scope or gets another one; for the jobs an object starts for itself.
class JobGuard
{
public:
    JobGuard() = default;
    JobGuard(CancelToken token) : token_(std::move(token)), active_(true) {}
    ~JobGuard() { cancel(); }
    JobGuard(JobGuard&& other) noexcept : token_(std::move(other.token_)), active_(std::exchange(other.active_, false))
    {
    }
    JobGuard& operator=(JobGuard&& other) noexcept
    {
        if (this != &other)
        {
            cancel();
            token_ = std::move(other.token_);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }
    JobGuard(const JobGuard&) = delete;
    JobGuard& operator=(const JobGuard&) = delete;

    void cancel()
    {
        if (active_)
            token_.cancel();
        active_ = false;
    }
    bool active() const { return active_ && !token_.cancelled(); }
    // Marks it finished without cancelling (from the done callback).
    void release() { active_ = false; }

private:
    CancelToken token_;
    bool active_ = false;
};

} // namespace tz
