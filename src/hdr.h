#pragma once

#include <windows.h>

// Whether Windows HD Color (Advanced Color) is currently on for the monitor a window is on. SDR
// capture and presentation stay exactly as they were without this fix when it is off; only when it
// is on does the capture path need FP16 to avoid clipping.
bool IsAdvancedColorEnabled(HWND window);

// The nits Windows currently maps SDR white to on that window's monitor (the "SDR content
// brightness" setting, only meaningful while Advanced Color is on). Falls back to the scRGB
// standard's own nominal SDR white (80 nits) if it cannot be queried, which is a safe no-op: it
// leaves a captured value of 1.0 mapping to 1.0 in the encoded output.
float GetSdrWhiteLevelNits(HWND window);
