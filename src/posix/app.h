#pragma once

#include "config.h"
#include "effects.h"
#include "games.h"
#include "gpu.h"
#include "platform.h"
#include "setup.h"

#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;
struct ImGuiContext;

// A window with its own swapchain and Dear ImGui context: the launcher, or the overlay over the game.
struct UiWindow
{
    GLFWwindow* window = nullptr;
    Surface surface;
    ImGuiContext* context = nullptr;
    float scale = 1.0f;
};

// The host: the launcher outside the game, and the overlay that redraws the game with effects and shows the
// menu. The same design as on Windows, where ReShade draws on the host's own swapchain.
class App
{
public:
    bool Init(std::string& error);
    void Run();
    void Shutdown();

    // For the launcher and the menu.
    void OpenMenu();
    // Gives input back to the game, unless the user went to another window.
    void CloseMenu(bool returnToGame = true);
    void ToggleOverlay();
    // An empty window goes back to finding saved games automatically.
    void Select(std::optional<platform::Window> window);
    void AddActiveGame();
    void SaveGames();
    std::vector<fs::path> Presets() const;
    // Switches presets, saving or dropping unsaved changes as told. Returns false when there are unsaved changes
    // and neither was asked for.
    bool SwitchPreset(const fs::path& path, bool save, bool discard);
    bool NewPreset(const std::string& name, bool copyCurrent, std::string& error);
    void StepPreset(int step);
    void RequestScreenshot(bool beforeAfter);
    // Registers the new shortcuts and saves them. Keeps the old ones and returns false when one is taken.
    bool ChangeHotkeys(const InputHotkeys& hotkeys, std::string& error);
    // While the menu records a shortcut, every shortcut is let go so its keys reach the menu.
    void SuspendHotkeys(bool suspended);
    bool EffectsInstalled();
    void ShowToast(std::string text, double seconds = 3.0);
    std::string HotkeyText(int id) const;
    const std::vector<platform::Window>& Windows();

    Settings settings;
    fx::Runtime runtime;
    EffectSetup setup;
    std::vector<AutoGame> autoGames;
    std::optional<platform::Window> selected;
    std::optional<platform::Window> active;
    bool captureEnabled = true;
    bool menuOpen = false;
    bool effectsEnabled = true;
    bool comparing = false;     // effects off while the compare shortcut is held
    bool compareButton = false; // or the menu's compare button
    bool overlayVisible = false;
    std::string toast;
    double toastUntil = 0;
    std::string lastCaptureError;
    UiWindow launcher;
    UiWindow overlay;

private:
    void OnHotkey(int id, bool pressed);
    void UpdateTarget();
    void UpdateOverlay();
    void UpdateGameHotkeys();
    void RenderOverlay();
    void RenderLauncher();
    void SaveScreenshot();
    void StartCapture(const platform::Window& window);
    void StopCapture();
    bool RegisterShortcut(const Shortcut& shortcut, const Hotkey& hotkey);

    bool inFront = false; // the game or the overlay has focus
    double effectsCheckTime = -10;
    bool effectsInstalled = false;

    platform::Frame frame;
    platform::Rect overlayBounds;
    bool gameHotkeys = false;
    bool hotkeysSuspended = false;
    bool menuFocused = false; // the overlay had focus since the menu opened
    bool startHintShown = false;
    bool screenshotRequested = false;
    bool beforeAfterRequested = false;
    double nextSearch = 0;
    double lastOverlayFrame = 0;
    double lastLauncherFrame = 0;
    double lastAutoSave = 0;
    double captureRetry = 0;
    std::vector<platform::Window> windows;
    double windowsTime = -10;
};

extern App app;
