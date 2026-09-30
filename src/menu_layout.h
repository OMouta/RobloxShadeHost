#pragma once

#include <algorithm>

namespace menu_layout
{
// Layout in pixels at 1080p.
constexpr float kWidth = 460;
constexpr float kMargin = 16;

constexpr float Scale(float width, float height)
{
    if (width <= 0 || height <= 0)
        return 0;
    // Height sets the scale; narrow windows must also fit the menu and its margins.
    return std::min(height / 1080.0f, width / (kWidth + kMargin * 2));
}
} // namespace menu_layout
