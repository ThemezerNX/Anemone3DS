// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "scenes/previews.hpp"

#include "sys/log.hpp"

namespace tz::scenes
{

namespace
{
struct Tiled
{
    gfx::TiledPixels top;
    std::optional<gfx::TiledPixels> bottom;
    gfx::TiledPixels blur;
    bool missing = false;
};
} // namespace

void Previews::want(uint64_t key, Loader loader, Lane lane)
{
    if (get(key) || (loadingKey_ == key && job_.active()))
        return;
    loadingKey_ = key;
    job_ = jobs::async(
        lane,
        [loader = std::move(loader)](const CancelToken& token) {
            auto result = std::make_shared<Tiled>();
            auto screens = loader(token);
            if (!screens)
            {
                if (screens.error().code != Errc::NotFound && screens.error().code != Errc::Cancelled)
                    log::writeError("preview", screens.error());
                result->missing = true;
                return result;
            }
            // the work of laying the pictures out for the GPU happens here, off the main thread
            result->top = gfx::tile(screens->top, true);
            if (screens->bottom)
                result->bottom = gfx::tile(*screens->bottom, true);
            result->blur = gfx::tile(preview::blurSource(screens->top), true);
            return result;
        },
        [this, key](std::shared_ptr<Tiled> tiled) {
            job_.release();
            PreviewPictures pictures;
            pictures.missing = tiled->missing;
            if (!tiled->missing)
            {
                pictures.top = gfx::Picture::upload(tiled->top);
                if (tiled->bottom)
                    pictures.bottom = gfx::Picture::upload(*tiled->bottom);
                pictures.blur = gfx::Picture::upload(tiled->blur);
                pictures.missing = !pictures.top;
            }
            cache_.emplace_front(key, std::move(pictures));
            while (cache_.size() > kKept)
                cache_.pop_back();
        },
        // the preview someone is looking at goes before everything else on its lane
        10);
}

const PreviewPictures* Previews::get(uint64_t key)
{
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
    {
        if (it->first == key)
        {
            if (it != cache_.begin())
                cache_.splice(cache_.begin(), cache_, it);
            return &cache_.front().second;
        }
    }
    return nullptr;
}

void Previews::clear()
{
    job_.cancel();
    cache_.clear();
    loadingKey_ = 0;
}

} // namespace tz::scenes
