// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Themezer-3DS contributors

#pragma once

#include "ui/input.hpp"

#include <functional>
#include <utility>

namespace tz::ui
{

// A vertical list on the bottom screen: the D-pad moves the selection (left and right by a page), a touch selects a
// row and a second one opens it, and a drag scrolls with a fling. Only the visible rows are drawn.
class ListView
{
public:
    enum class Event : uint8_t
    {
        None,
        Moved,     // the selection changed
        Activated, // A, or a tap on the selected row
    };

    void setBounds(float x, float y, float width, float height);
    void setRowHeight(float height) { rowHeight_ = height; }
    // Keeps the selection in range; the list doesn't jump.
    void setCount(int count);
    int count() const { return count_; }

    int selected() const { return selected_; }
    void select(int index, bool animate = true);

    Event update(Input& input, float dt);

    // drawRow(index, x, y, width, height, selected)
    using RowPainter = std::function<void(int, float, float, float, float, bool)>;
    void draw(const RowPainter& drawRow) const;

    // The rows that are at least partly on screen, for loading what they show first.
    std::pair<int, int> visibleRange() const;
    int rowsPerPage() const;
    bool settled() const;

private:
    float maxScroll() const;
    void keepSelectionVisible();

    float x_ = 0;
    float y_ = 0;
    float width_ = 320;
    float height_ = 200;
    float rowHeight_ = 44;
    int count_ = 0;
    int selected_ = 0;
    float scroll_ = 0;       // pixels from the top
    float target_ = 0;       // where the scroll is heading
    float velocity_ = 0;     // of a fling, pixels a second
    bool dragging_ = false;
    bool pressedInside_ = false;
};

} // namespace tz::ui
