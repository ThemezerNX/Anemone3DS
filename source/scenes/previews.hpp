// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "core/result.hpp"
#include "domain/preview.hpp"
#include "gfx/texture.hpp"
#include "sys/jobs.hpp"

#include <algorithm>
#include <functional>
#include <list>
#include <memory>

namespace tz::scenes
{

// The pictures of an item's preview, ready to draw.
struct PreviewPictures
{
    std::shared_ptr<gfx::Picture> top;
    std::shared_ptr<gfx::Picture> bottom;
    std::shared_ptr<gfx::Picture> blur; // for the backdrop
    bool missing = false;               // the item has no preview to show
};

// The previews of the last few items looked at, loaded on a worker one at a time: asking for another item drops a
// load still waiting.
class Previews
{
public:
    // Reads and decodes the preview, on a worker thread. NotFound when there is none.
    using Loader = std::function<Expected<preview::Screens>(const CancelToken&)>;

    void want(uint64_t key, Loader loader, Lane lane = Lane::Io);
    // nullptr until it has loaded.
    const PreviewPictures* get(uint64_t key);
    bool loading(uint64_t key) const { return loadingKey_ == key && job_.active(); }
    void clear();

private:
    static constexpr size_t kKept = 5;
    std::list<std::pair<uint64_t, PreviewPictures>> cache_; // the most recent first
    uint64_t loadingKey_ = 0;
    JobGuard job_;
};

// How far the selected item's preview has faded in over the top screen: from nothing for each item selected, once
// its preview is there.
class PreviewFade
{
public:
    void update(uint64_t key, bool ready, float dt)
    {
        if (key != key_)
        {
            key_ = key;
            alpha_ = 0;
        }
        alpha_ = ready ? std::min(1.0f, alpha_ + dt * 6) : 0;
    }
    float alpha() const { return alpha_; }

private:
    uint64_t key_ = 0;
    float alpha_ = 0;
};

} // namespace tz::scenes
