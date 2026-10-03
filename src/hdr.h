#pragma once

#include <windows.h>

#include <optional>

// How bright Windows makes SDR white on the window's display, in nits, while HDR is on there. Nothing while it is off,
// including wide color on an SDR display, or when the display cannot be found.
std::optional<float> HdrWhiteLevel(HWND window);
