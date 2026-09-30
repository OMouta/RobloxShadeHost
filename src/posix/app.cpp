#include "app.h"
#include "log.h"
#include "theme.h"
#include "ui.h"

#include <GLFW/glfw3.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>

App app;

namespace
{
double Now()
{
    return glfwGetTime();
}

const Shortcut* FindShortcut(int id)
{
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.id == id)
            return &shortcut;
    return nullptr;
}
} // namespace

bool App::Init(std::string& error)
{
    settings = LoadSettings();
    try
    {
        autoGames = LoadAutoGames(DataDirectory() / "games.ini");
    }
    catch (const std::exception& e)
    {
        Log(LogLevel::Warning, "Could not load games.ini: %s", e.what());
        autoGames = DefaultAutoGames();
    }

    if (!platform::Init(error) || !gpu.Init(false, error))
        return false;
    if (!runtime.Init(settings))
    {
        error = "Could not prepare the graphics card for effects.";
        return false;
    }
    runtime.LoadPreset(settings.preset);

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    launcher.window = glfwCreateWindow(600, 680, "Unishade", nullptr, nullptr);
    if (!launcher.window || !launcher.surface.Create(launcher.window, false, error) || !InitUi(launcher, error))
    {
        if (error.empty())
            error = "Could not open the launcher window.";
        return false;
    }

    // The overlay covers the game's window exactly. Clicks go through to the game until the menu opens.
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
    overlay.window = glfwCreateWindow(64, 64, "Unishade overlay", nullptr, nullptr);
    if (!overlay.window)
    {
        error = "Could not create the overlay window.";
        return false;
    }
    platform::SetupOverlayWindow(overlay.window);
    if (!overlay.surface.Create(overlay.window, true, error) || !InitUi(overlay, error))
        return false;

    platform::SetHotkeyCallback([this](int id, bool pressed) { OnHotkey(id, pressed); });
    for (const Shortcut& shortcut : kShortcuts)
        if (shortcut.always && !RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member))
            Report(LogLevel::Warning, "%s is used by another program, so it cannot %s.", FormatHotkey(settings.hotkeys.*shortcut.member).c_str(),
                   Lowercase(shortcut.label).c_str());
    if (!platform::HasCapturePermission())
        Report(LogLevel::Warning, "Unishade needs permission to record the screen before it can copy the game's picture.");
    return true;
}

void App::Run()
{
    Log(LogLevel::Info, "Waiting for a supported game...");
    while (!glfwWindowShouldClose(launcher.window))
    {
        glfwWaitEventsTimeout(overlayVisible ? (menuOpen ? 1.0 / 60 : 0.1) : 0.25);
        platform::PollHotkeys();
        runtime.Update();
        if (setup.TakeFinished())
        {
            effectsCheckTime = -10;
            runtime.LoadPreset(settings.preset);
            runtime.Reload();
        }

        UpdateTarget();
        inFront = active && (platform::ProcessInFront(active->pid) || glfwGetWindowAttrib(overlay.window, GLFW_FOCUSED));
        UpdateGameHotkeys();
        UpdateOverlay();

        if (menuOpen)
        {
            // Another window took focus, as the menu closes on Windows when the host loses it.
            if (glfwGetWindowAttrib(overlay.window, GLFW_FOCUSED))
                menuFocused = true;
            else if (menuFocused)
                CloseMenu(false);
        }
        else if (overlayVisible && glfwGetWindowAttrib(overlay.window, GLFW_FOCUSED) && active)
            platform::Activate(*active); // the window manager focused the overlay, which should never have it

        const bool newFrame = active && platform::TakeFrame(frame);
        const bool toastShowing = !toast.empty() && Now() < toastUntil;
        if (overlayVisible && !frame.pixels.empty() && (newFrame || menuOpen || toastShowing || Now() - lastOverlayFrame > 0.1))
            RenderOverlay();

        if (settings.autoSavePresets && runtime.Dirty() && Now() - lastAutoSave > 1.0)
        {
            runtime.SavePreset();
            lastAutoSave = Now();
        }

        if (!glfwGetWindowAttrib(launcher.window, GLFW_ICONIFIED) &&
            (glfwGetWindowAttrib(launcher.window, GLFW_FOCUSED) || glfwGetWindowAttrib(launcher.window, GLFW_HOVERED) ||
             Now() - lastLauncherFrame > 0.25))
            RenderLauncher();
    }
}

void App::Shutdown()
{
    StopCapture();
    if (runtime.Dirty() && settings.autoSavePresets)
        runtime.SavePreset();
    setup.Cancel();
    if (gpu.device)
        vkDeviceWaitIdle(gpu.device);
    for (UiWindow* ui : { &overlay, &launcher })
    {
        ShutdownUi(*ui);
        ui->surface.Destroy();
        if (ui->window)
            glfwDestroyWindow(ui->window);
        ui->window = nullptr;
    }
    runtime.Shutdown();
    platform::Shutdown();
    gpu.Shutdown();
}

// Finding the game

void App::StartCapture(const platform::Window& window)
{
    if (!platform::HasCapturePermission())
    {
        lastCaptureError = "Unishade needs permission to record the screen.";
        captureRetry = Now() + 2;
        return;
    }
    std::string error;
    if (!platform::StartCapture(window, error))
    {
        lastCaptureError = "Could not capture " + window.title + ": " + error;
        Log(LogLevel::Warning, "%s", lastCaptureError.c_str());
        captureRetry = Now() + 2;
        return;
    }
    active = window;
    frame = {};
    lastCaptureError.clear();
    Log(LogLevel::Info, "Capturing %s", window.title.c_str());
    if (!startHintShown)
    {
        ShowToast("Press " + HotkeyText(kEditModeHotkey) + " to open the Unishade menu", 6);
        startHintShown = true;
    }
}

void App::StopCapture()
{
    if (menuOpen)
        CloseMenu(false);
    platform::StopCapture();
    if (active)
        Log(LogLevel::Info, "Stopped capturing %s.", active->title.c_str());
    active.reset();
    frame = {};
    if (overlayVisible)
        platform::ShowOverlay(overlay.window, false);
    overlayVisible = false;
}

void App::UpdateTarget()
{
    if (!captureEnabled)
    {
        if (active)
            StopCapture();
        return;
    }
    if (active && !platform::WindowExists(*active))
    {
        Log(LogLevel::Info, "%s closed.", active->title.c_str());
        StopCapture();
    }
    else if (active && !platform::Capturing())
    {
        lastCaptureError = platform::CaptureError();
        if (!lastCaptureError.empty())
            Log(LogLevel::Warning, "%s", lastCaptureError.c_str());
        StopCapture();
        captureRetry = Now() + 2;
    }

    if ((active && (selected || menuOpen)) || Now() < nextSearch || Now() < captureRetry)
        return;
    nextSearch = Now() + 0.5;
    const platform::WindowId foreground = platform::ForegroundWindow();
    const std::optional<platform::Window> game = FindGameTarget(selected, autoGames, foreground);
    if (game && (!active || game->id != active->id) && (!active || game->id == foreground))
    {
        if (active)
            StopCapture();
        StartCapture(*game);
    }
}

void App::Select(std::optional<platform::Window> window)
{
    selected = std::move(window);
    nextSearch = 0;
    captureRetry = 0;
    if (active && (!selected || selected->id != active->id))
        StopCapture();
}

void App::AddActiveGame()
{
    if (!active)
        return;
    if (!AddAutoGame(autoGames, *active))
    {
        Report(LogLevel::Warning, "%s has closed.", active->title.c_str());
        return;
    }
    SaveGames();
}

void App::SaveGames()
{
    if (!SaveAutoGames(DataDirectory() / "games.ini", autoGames))
        Report(LogLevel::Warning, "Could not save the game list.");
}

const std::vector<platform::Window>& App::Windows()
{
    if (Now() - windowsTime > 2)
    {
        windows = platform::ListWindows();
        windowsTime = Now();
    }
    return windows;
}

// The overlay

void App::UpdateOverlay()
{
    platform::Rect bounds;
    const bool visible = captureEnabled && active && !frame.pixels.empty() && (menuOpen || inFront) && platform::WindowBounds(active->id, bounds);
    if (!visible)
    {
        if (overlayVisible)
            platform::ShowOverlay(overlay.window, false);
        overlayVisible = false;
        return;
    }
    if (!overlayVisible || !(bounds == overlayBounds))
    {
        glfwSetWindowPos(overlay.window, bounds.x, bounds.y);
        glfwSetWindowSize(overlay.window, bounds.width, bounds.height);
        overlayBounds = bounds;
    }
    if (!overlayVisible)
    {
        platform::ShowOverlay(overlay.window, true);
        overlayVisible = true;
    }
}

void App::RenderOverlay()
{
    Surface& surface = overlay.surface;
    if (!surface.BeginFrame())
        return;
    lastOverlayFrame = Now();
    runtime.SetSize(frame.width, frame.height);
    runtime.menuOpen = menuOpen;
    double x = 0, y = 0;
    int width = 1, height = 1;
    glfwGetCursorPos(overlay.window, &x, &y);
    glfwGetWindowSize(overlay.window, &width, &height);
    runtime.mouseX = static_cast<float>(x * frame.width / std::max(width, 1));
    runtime.mouseY = static_cast<float>(y * frame.height / std::max(height, 1));

    if (runtime.Width() == frame.width && runtime.Height() == frame.height)
    {
        runtime.Render(surface.commands, frame.pixels.data(), effectsEnabled && !comparing && !compareButton);
        FullBarrier(surface.commands);
        const GpuImage& output = runtime.Output();
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { int(output.width), int(output.height), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { int(surface.extent.width), int(surface.extent.height), 1 };
        vkCmdBlitImage(surface.commands, output.image, VK_IMAGE_LAYOUT_GENERAL, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }
    else
    {
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(surface.commands, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }

    BeginUi(overlay);
    DrawOverlay(*this);
    EndUi(overlay);

    if (screenshotRequested)
        SaveScreenshot();
}

void App::RenderLauncher()
{
    Surface& surface = launcher.surface;
    if (!surface.BeginFrame())
        return;
    lastLauncherFrame = Now();
    VkClearColorValue background{};
    background.float32[0] = ((theme::kBackground >> 16) & 0xFF) / 255.0f;
    background.float32[1] = ((theme::kBackground >> 8) & 0xFF) / 255.0f;
    background.float32[2] = (theme::kBackground & 0xFF) / 255.0f;
    background.float32[3] = 1.0f;
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(surface.commands, surface.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1, &range);
    BeginUi(launcher);
    DrawLauncher(*this);
    EndUi(launcher);
}

void App::OpenMenu()
{
    if (menuOpen || !captureEnabled || !active || !overlayVisible)
        return;
    menuOpen = true;
    menuFocused = false;
    glfwSetWindowAttrib(overlay.window, GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
    platform::FocusOverlay(overlay.window);
    Log(LogLevel::Info, "Menu opened.");
}

void App::CloseMenu(bool returnToGame)
{
    if (!menuOpen)
        return;
    menuOpen = false;
    ResetMenu(*this);
    glfwSetWindowAttrib(overlay.window, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
    if (settings.autoSavePresets && runtime.Dirty())
        runtime.SavePreset();
    if (returnToGame && active)
        platform::Activate(*active);
    Log(LogLevel::Info, "Input returned to the game.");
}

void App::ToggleOverlay()
{
    captureEnabled = !captureEnabled;
    if (!captureEnabled && menuOpen)
        CloseMenu();
    Log(LogLevel::Info, captureEnabled ? "Overlay on." : "Overlay off. Frame capture stopped.");
}

void App::ShowToast(std::string text, double seconds)
{
    toast = std::move(text);
    toastUntil = Now() + seconds;
}

// Shortcuts

std::string App::HotkeyText(int id) const
{
    const Shortcut* shortcut = FindShortcut(id);
    return shortcut ? FormatHotkey(settings.hotkeys.*shortcut->member) : std::string();
}

bool App::RegisterShortcut(const Shortcut& shortcut, const Hotkey& hotkey)
{
    return platform::RegisterHotkey(shortcut.id, hotkey);
}

void App::OnHotkey(int id, bool pressed)
{
    switch (id)
    {
    case kEditModeHotkey:
        if (pressed)
            menuOpen ? CloseMenu() : OpenMenu();
        break;
    case kOverlayToggleHotkey:
        if (pressed)
            ToggleOverlay();
        break;
    case kCompareHotkey:
        comparing = pressed;
        break;
    case kScreenshotHotkey:
    case kBeforeAfterHotkey:
        if (pressed)
            RequestScreenshot(id == kBeforeAfterHotkey);
        break;
    case kNextPresetHotkey:
    case kPreviousPresetHotkey:
        if (pressed)
            StepPreset(id == kNextPresetHotkey ? 1 : -1);
        break;
    }
}

// Holding a bare key such as Home all the time would break it in every other program, so most shortcuts are only
// held while the game or the menu is in front.
void App::UpdateGameHotkeys()
{
    const bool wanted = !hotkeysSuspended && active && (menuOpen || inFront);
    if (wanted == gameHotkeys)
        return;
    gameHotkeys = wanted;
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (shortcut.always)
            continue;
        if (!wanted)
            platform::UnregisterHotkey(shortcut.id);
        else if (!RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member))
            Report(LogLevel::Warning, "%s is used by another program, so it cannot %s.", FormatHotkey(settings.hotkeys.*shortcut.member).c_str(),
                   Lowercase(shortcut.label).c_str());
    }
    if (!wanted)
        comparing = false;
}

void App::SuspendHotkeys(bool suspended)
{
    if (suspended == hotkeysSuspended)
        return;
    hotkeysSuspended = suspended;
    for (const Shortcut& shortcut : kShortcuts)
    {
        if (suspended)
            platform::UnregisterHotkey(shortcut.id);
        else if (shortcut.always || gameHotkeys)
            RegisterShortcut(shortcut, settings.hotkeys.*shortcut.member);
    }
}

bool App::ChangeHotkeys(const InputHotkeys& hotkeys, std::string& error)
{
    for (const Shortcut& a : kShortcuts)
        for (const Shortcut& b : kShortcuts)
            if (&a < &b && hotkeys.*a.member == hotkeys.*b.member)
            {
                error = std::string(a.label) + " and " + Lowercase(b.label) + " cannot share " + FormatHotkey(hotkeys.*a.member) + ".";
                return false;
            }
    for (const Shortcut& shortcut : kShortcuts)
        platform::UnregisterHotkey(shortcut.id);
    const auto registerAll = [this](const InputHotkeys& keys, const Shortcut** failed) {
        for (const Shortcut& shortcut : kShortcuts)
            if ((shortcut.always || gameHotkeys) && !RegisterShortcut(shortcut, keys.*shortcut.member))
            {
                if (failed)
                    *failed = &shortcut;
                return false;
            }
        return true;
    };
    const Shortcut* failed = nullptr;
    if (!registerAll(hotkeys, &failed))
    {
        error = FormatHotkey(hotkeys.*failed->member) + " is used by another program.";
        for (const Shortcut& shortcut : kShortcuts)
            platform::UnregisterHotkey(shortcut.id);
        registerAll(settings.hotkeys, nullptr);
        return false;
    }
    settings.hotkeys = hotkeys;
    SaveSettings(settings);
    return true;
}

// Presets

std::vector<fs::path> App::Presets() const
{
    std::vector<fs::path> presets;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(PresetsDirectory(), error))
        if (entry.is_regular_file() && Lowercase(entry.path().extension().string()) == ".ini")
            presets.push_back(entry.path());
    const fs::path& current = runtime.PresetPath();
    if (!current.empty() && std::find(presets.begin(), presets.end(), current) == presets.end())
        presets.push_back(current);
    std::sort(presets.begin(), presets.end(), [](const fs::path& a, const fs::path& b) {
        return strcasecmp(a.stem().c_str(), b.stem().c_str()) < 0;
    });
    return presets;
}

bool App::SwitchPreset(const fs::path& path, bool save, bool discard)
{
    if (path == runtime.PresetPath() && !discard)
        return true;
    if (runtime.Dirty() && path != runtime.PresetPath())
    {
        if (save)
            runtime.SavePreset();
        else if (!discard)
            return false;
    }
    runtime.LoadPreset(path);
    settings.preset = path;
    SaveSettings(settings);
    ShowToast("Preset: " + path.stem().string());
    return true;
}

bool App::NewPreset(const std::string& name, bool copyCurrent, std::string& error)
{
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name[0] == '.')
    {
        error = "Preset names cannot contain slashes or start with a dot.";
        return false;
    }
    const fs::path path = PresetsDirectory() / (name + ".ini");
    if (fs::exists(path))
    {
        error = "A preset with that name already exists.";
        return false;
    }
    if (copyCurrent)
    {
        // The active preset is copied as it is on screen, unsaved changes included.
        if (!runtime.SavePresetAs(path))
        {
            error = "Could not write the preset.";
            return false;
        }
        settings.preset = path;
        SaveSettings(settings);
        return true;
    }
    if (runtime.Dirty() && !settings.autoSavePresets)
    {
        error = "Save or discard the current preset's changes first.";
        return false;
    }
    if (!WriteFile(path, "Techniques=\n"))
    {
        error = "Could not write the preset.";
        return false;
    }
    return SwitchPreset(path, true, false);
}

void App::StepPreset(int step)
{
    const std::vector<fs::path> presets = Presets();
    if (presets.empty())
        return;
    if (runtime.Dirty() && !settings.autoSavePresets)
    {
        ShowToast("Save or discard the preset's changes first");
        return;
    }
    const auto current = std::find(presets.begin(), presets.end(), runtime.PresetPath());
    const long index = current == presets.end() ? 0 : current - presets.begin();
    const long count = static_cast<long>(presets.size());
    SwitchPreset(presets[((index + step) % count + count) % count], true, false);
}

// Screenshots

void App::RequestScreenshot(bool beforeAfter)
{
    screenshotRequested = true;
    beforeAfterRequested = beforeAfter;
}

void App::SaveScreenshot()
{
    screenshotRequested = false;
    std::vector<uint8_t> after = runtime.ReadOutput();
    if (after.empty())
        return;
    const uint32_t width = runtime.Width(), height = runtime.Height();
    std::vector<uint8_t> image;
    uint32_t imageWidth = width;
    if (beforeAfterRequested && frame.width == width && frame.height == height)
    {
        // The game's own picture on the left, with effects on the right.
        imageWidth = width * 2;
        image.resize(size_t(imageWidth) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t pixel = frame.pixels[size_t(y) * width + x];
                uint8_t* before = &image[(size_t(y) * imageWidth + x) * 4];
                before[0] = (pixel >> 16) & 0xFF;
                before[1] = (pixel >> 8) & 0xFF;
                before[2] = pixel & 0xFF;
                before[3] = 255;
                std::memcpy(&image[(size_t(y) * imageWidth + width + x) * 4], &after[(size_t(y) * width + x) * 4], 4);
            }
    }
    else
        image = std::move(after);

    char stamp[64];
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
    const std::string game = active ? active->title : "Unishade";
    const fs::path path = ScreenshotDirectory() / (game + " " + stamp + (beforeAfterRequested ? " before-after" : "") + ".png");
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (stbi_write_png(path.c_str(), int(imageWidth), int(height), 4, image.data(), int(imageWidth) * 4))
    {
        Log(LogLevel::Info, "Screenshot saved to %s", path.c_str());
        std::string folder = ScreenshotDirectory().string();
        if (const char* home = getenv("HOME"); home && *home && folder.rfind(home, 0) == 0)
            folder = "~" + folder.substr(strlen(home));
        ShowToast("Screenshot saved to " + folder);
    }
    else
        Report(LogLevel::Warning, "Could not save the screenshot to %s.", path.c_str());
}

bool App::EffectsInstalled()
{
    if (Now() - effectsCheckTime < 5)
        return effectsInstalled;
    effectsCheckTime = Now();
    effectsInstalled = false;
    for (const fs::path& root : settings.effectPaths)
    {
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, error), end; it != end && !effectsInstalled; it.increment(error))
            effectsInstalled = !error && Lowercase(it->path().extension().string()) == ".fx";
    }
    return effectsInstalled;
}
