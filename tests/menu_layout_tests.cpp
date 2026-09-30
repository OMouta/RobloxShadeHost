#include "../src/menu_layout.h"

#include <cmath>
#include <cstdio>

namespace
{
bool Check(bool condition, const char* message)
{
    if (!condition)
        std::printf("Failed: %s\n", message);
    return condition;
}

bool Near(float actual, float expected)
{
    return std::abs(actual - expected) < 0.001f;
}
} // namespace

int main()
{
    bool ok = true;
    const struct
    {
        float width;
        float height;
        float expectedScale;
    } resolutions[] = {
        { 1280, 720, 2.0f / 3 },
        { 1920, 1080, 1 },
        { 2560, 1440, 4.0f / 3 },
        { 3840, 2160, 2 },
        { 7680, 4320, 4 },
        { 3440, 1440, 4.0f / 3 },
        { 5120, 1440, 4.0f / 3 },
        { 640, 480, 4.0f / 9 },
        { 320, 1080, 320.0f / 492 },
    };
    for (const auto& resolution : resolutions)
    {
        const float scale = menu_layout::Scale(resolution.width, resolution.height);
        if (!Near(scale, resolution.expectedScale))
        {
            std::printf("Failed: scale for %.0fx%.0f is %f, expected %f\n",
                        resolution.width, resolution.height, scale, resolution.expectedScale);
            ok = false;
        }
        ok &= Check((menu_layout::kWidth + menu_layout::kMargin * 2) * scale <= resolution.width + 0.001f,
                    "menu and margins fit the viewport width");
        ok &= Check((menu_layout::kMargin * 2 + 74 + 42 + 64) * scale + 1 < resolution.height,
                    "header, tabs and footer leave space for scrollable content");
    }
    ok &= Check(menu_layout::Scale(0, 1080) == 0 && menu_layout::Scale(1920, 0) == 0,
                "empty viewports do not draw");
    ok &= Check(menu_layout::Scale(-1, 1080) == 0 && menu_layout::Scale(1920, -1) == 0,
                "invalid viewports do not draw");
    return ok ? 0 : 1;
}
