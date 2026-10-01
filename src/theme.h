#pragma once

// Unishade's colors, shared by Setup, the launcher, the menu and the macOS and Linux host. 0xRRGGBB.
namespace theme
{
constexpr unsigned kBackground = 0x111217;
constexpr unsigned kSidebar = 0x0C0D11;
constexpr unsigned kCard = 0x191A21;
constexpr unsigned kCardHover = 0x1F2029;
constexpr unsigned kBorder = 0x282A35;
constexpr unsigned kBorderStrong = 0x4A4D5E;
constexpr unsigned kText = 0xECECF1;
constexpr unsigned kDim = 0x9698A7;
constexpr unsigned kAccent = 0x707AFF;
constexpr unsigned kAccentHover = 0x848DFF;
constexpr unsigned kAccentActive = 0x5F68EB;
constexpr unsigned kWarning = 0xF5C05C;
constexpr unsigned kError = 0xFF7676;
constexpr unsigned kSuccess = 0x68D694;
// The ring in the logo.
constexpr unsigned kRainbow[] = { 0xFF4860, 0xFF9E36, 0xF8E44C, 0x54DE7A, 0x3EC6FF, 0x546EFF, 0xBA5CFF };
} // namespace theme
