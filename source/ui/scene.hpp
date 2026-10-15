// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "ui/input.hpp"

#include <memory>
#include <vector>

namespace tz::ui
{

// A screen of the app, drawn on both screens. Only the scene on top gets input; overlays (dialogs, menus) let the
// scenes under them show through.
class Scene
{
public:
    virtual ~Scene() = default;

    virtual void update(Input& input, float dt) = 0;
    virtual void drawTop() {}
    virtual void drawBottom() {}
    // False for overlays: the scenes under them are drawn first.
    virtual bool covers() const { return true; }
    // Called when the scene above this one has closed.
    virtual void onResume() {}
    // Called every frame, also when covered, for things that keep going (loading, animations).
    virtual void tick(float /*dt*/) {}

    bool closed() const { return closed_; }
    void close() { closed_ = true; }

private:
    bool closed_ = false;
};

class SceneStack
{
public:
    void push(std::unique_ptr<Scene> scene);
    template <class T, class... Args>
    T& emplace(Args&&... args)
    {
        auto scene = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *scene;
        push(std::move(scene));
        return ref;
    }

    void update(Input& input, float dt);
    void drawTop();
    void drawBottom();

    bool empty() const { return scenes_.empty(); }
    size_t size() const { return scenes_.size(); }
    Scene* top() const { return scenes_.empty() ? nullptr : scenes_.back().get(); }

private:
    size_t firstVisible() const;
    std::vector<std::unique_ptr<Scene>> scenes_;
    std::vector<std::unique_ptr<Scene>> pending_;
};

} // namespace tz::ui
