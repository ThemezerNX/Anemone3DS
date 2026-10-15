// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#include "ui/scene.hpp"

namespace tz::ui
{

void SceneStack::push(std::unique_ptr<Scene> scene)
{
    // scenes pushed while the stack updates join it after the update, so they start with the next frame's input
    pending_.push_back(std::move(scene));
}

void SceneStack::update(Input& input, float dt)
{
    for (auto& scene : pending_)
        scenes_.push_back(std::move(scene));
    pending_.clear();

    for (auto& scene : scenes_)
        scene->tick(dt);

    if (Scene* scene = top(); scene && !scene->closed())
        scene->update(input, dt);

    // close what asked to be closed, and tell the scene that comes back
    bool removed = false;
    for (size_t i = scenes_.size(); i-- > 0;)
    {
        if (scenes_[i]->closed())
        {
            scenes_.erase(scenes_.begin() + static_cast<ptrdiff_t>(i));
            removed = true;
        }
    }
    if (removed && pending_.empty())
        if (Scene* scene = top())
            scene->onResume();
}

size_t SceneStack::firstVisible() const
{
    for (size_t i = scenes_.size(); i-- > 0;)
        if (scenes_[i]->covers())
            return i;
    return 0;
}

void SceneStack::drawTop()
{
    for (size_t i = firstVisible(); i < scenes_.size(); ++i)
        scenes_[i]->drawTop();
}

void SceneStack::drawBottom()
{
    for (size_t i = firstVisible(); i < scenes_.size(); ++i)
        scenes_[i]->drawBottom();
}

} // namespace tz::ui
