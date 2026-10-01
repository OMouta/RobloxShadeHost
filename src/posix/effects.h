#pragma once

#include "config.h"
#include "gpu.h"

#include <effect_module.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Runs ReShade effects on the game's picture. ReShade's own compiler turns each .fx file into SPIR-V, the same
// way ReShade does for Vulkan games, and this runs the passes it describes. Presets are ReShade's .ini files,
// so they move between Windows and here unchanged.
namespace fx
{
struct Uniform
{
    std::string name;
    std::string label;
    std::string tooltip;
    std::string uiType; // slider, drag, combo, radio, list, color, input or empty
    std::string category;
    std::string items; // separated by '\0' and ending in two, as ImGui::Combo takes them
    std::string text;  // ui_text, shown above the control
    std::string source; // set by the host each frame, such as the timer, and not shown
    reshadefx::type type;
    uint32_t offset = 0;
    uint32_t size = 0;
    float min = 0;
    float max = 0;
    float step = 0;
    bool hidden = false;
    size_t declaration = 0; // index into the module's uniforms, for annotations
};

struct Technique
{
    std::string name;
    std::string label;
    std::string tooltip;
    size_t effect = 0; // index into Runtime::Effects
    size_t index = 0;  // index into the effect's module
    bool enabled = false;
    bool hidden = false;
    bool enabledByDefault = false;
};

struct EffectGpu;

struct Effect
{
    std::filesystem::path path;
    std::string file; // the filename, which presets use as the section name
    bool compiled = false;
    std::string errors; // the compiler's errors and warnings
    reshadefx::effect_module module;
    std::unordered_map<std::string, std::vector<uint32_t>> spirv; // SPIR-V code by entry point
    std::vector<Uniform> uniforms;
    std::vector<uint8_t> uniformData;
    std::shared_ptr<EffectGpu> gpu;
    bool gpuFailed = false;

    Effect();
    Effect(Effect&&) noexcept;
    Effect& operator=(Effect&&) noexcept;
    ~Effect();
};

using Definitions = std::vector<std::pair<std::string, std::string>>;

// A preset's preprocessor definitions: the ones for every effect and the ones in each effect's section.
struct PresetDefinitions
{
    Definitions global;
    std::map<std::string, Definitions> effects;
    bool operator==(const PresetDefinitions&) const = default;
};

// What effects can read through uniform sources such as "key", "mousebutton" and "overlay_active", and what
// technique shortcuts react to. Key codes are Windows virtual-key codes, as ReShade's annotations and presets
// write them. The host fills this in every frame before Render.
struct EffectInput
{
    std::array<bool, 256> keysDown{};
    std::array<bool, 256> keysPressed{}; // went down since the previous frame
    std::array<bool, 5> buttonsDown{};   // left, right, middle, back, forward
    std::array<bool, 5> buttonsPressed{};
    float cursorDeltaX = 0; // in frame pixels since the previous frame
    float cursorDeltaY = 0;
    float wheelDelta = 0;
    bool overlayActive = false;  // a menu control is being used
    bool overlayHovered = false; // the cursor is over the menu
};

// "Name@File.fx", as presets list techniques.
std::string TechniqueKey(const Technique& technique, const Effect& effect);

class Runtime
{
public:
    bool Init(const Settings& settings);
    void Shutdown();

    // The size of the game's picture. Effects are compiled for it, so a new size compiles them again.
    void SetSize(uint32_t width, uint32_t height);
    uint32_t Width() const { return width; }
    uint32_t Height() const { return height; }

    // Finds the effects again and compiles them on worker threads, starting at the next Update, since the frame
    // being recorded may still use the current ones. Effects the current preset uses come first.
    void Reload() { reloadRequested = true; }
    bool Loading() const { return loaderRunning || reloadRequested; }
    // How many effect files are compiled out of how many were found.
    std::pair<size_t, size_t> LoadingProgress() const { return { loadedCount.load(), totalCount.load() }; }
    // Takes in compiled effects. Call once per frame on the main thread.
    void Update();

    // The game's picture: pixels in memory, or the part at (x, y) of an image on the graphics card, in
    // VK_IMAGE_LAYOUT_GENERAL. Either way it is the size given to SetSize.
    struct Source
    {
        const uint32_t* pixels = nullptr;
        VkImage image = VK_NULL_HANDLE;
        uint32_t x = 0;
        uint32_t y = 0;
        bool foreign = false; // owned by something outside Vulkan, such as the X server
    };
    // Copies the game's picture in and runs every enabled technique on it, or none when effects is false.
    void Render(VkCommandBuffer commands, const Source& source, bool effects);
    // The picture after Render, in VK_IMAGE_LAYOUT_GENERAL. BGRA.
    const GpuImage& Output() const { return backbuffer; }
    // Reads the output back, as RGBA pixels. Waits for the graphics card.
    std::vector<uint8_t> ReadOutput();
    // Reads the game's own picture from an image source, as RGBA pixels, for before and after screenshots.
    std::vector<uint8_t> ReadSource(const Source& source);

    // Presets. Switching compiles effects again when the preset has other preprocessor definitions.
    bool LoadPreset(const fs::path& path);
    bool SavePreset();
    // Saves the effects as they are to a new preset and switches to it.
    bool SavePresetAs(const fs::path& path);
    const fs::path& PresetPath() const { return presetPath; }
    // Changes since the preset was loaded or saved.
    bool Dirty() const { return dirty; }
    void SetDirty() { dirty = true; }
    // Techniques the preset turns on whose effect file was not found.
    std::vector<std::string> MissingTechniques() const;

    std::vector<Effect>& Effects() { return effects; }
    // In the order they run.
    std::vector<Technique>& Techniques() { return techniques; }
    void SetEnabled(size_t technique, bool enabled);
    void MoveTechnique(size_t from, size_t to);

    // Values as floats or ints, converted from the variable's own type. Arrays and matrices keep ReShade's
    // 16-byte rows.
    void GetValue(const Effect& effect, const Uniform& uniform, float* values, size_t count) const;
    void GetValue(const Effect& effect, const Uniform& uniform, int* values, size_t count) const;
    void SetValue(Effect& effect, const Uniform& uniform, const float* values, size_t count);
    void SetValue(Effect& effect, const Uniform& uniform, const int* values, size_t count);
    void ResetValue(Effect& effect, const Uniform& uniform);

    // For uniforms with a source.
    bool menuOpen = false;
    float mouseX = 0;
    float mouseY = 0;
    EffectInput input;

private:
    struct SharedTexture
    {
        GpuImage image;
        reshadefx::texture_desc desc;
    };

    void StopLoader();
    void ReloadNow();
    void AddEffect(Effect&& effect);
    void SortTechniques();
    void ApplyPreset(Effect& effect, size_t effectIndex);
    bool CreateGpu(Effect& effect);
    void DestroyGpu(Effect& effect);
    void DestroyAllGpu();
    bool CreateTargets();
    void UpdateSpecialUniforms(Effect& effect);
    VkSampler Sampler(const reshadefx::sampler_desc& desc);
    GpuImage* Texture(const reshadefx::texture& texture, Effect& effect);
    fs::path FindTexture(const std::string& source);
    void GenerateMipmaps(VkCommandBuffer commands, const GpuImage& image);
    std::vector<uint8_t> ReadImage(VkImage image, uint32_t x, uint32_t y, bool foreign);

    Settings settings;
    uint32_t width = 0;
    uint32_t height = 0;

    GpuImage backbuffer; // what passes without a render target write to
    GpuImage color;      // a copy of backbuffer, which effects read as COLOR
    GpuImage depth;      // effects read DEPTH from here, which stays empty: the game's depth is out of reach
    GpuImage blank;      // bound where an effect reads a texture it is writing in the same pass
    GpuBuffer staging;
    GpuBuffer readback;
    std::map<std::string, SharedTexture> sharedTextures;
    std::vector<std::pair<std::vector<uint8_t>, VkSampler>> samplers;
    std::vector<std::pair<std::string, fs::path>> textureFiles; // lowercase relative path, full path
    bool textureFilesScanned = false;

    std::vector<Effect> effects;
    std::vector<Technique> techniques;
    std::vector<std::string> sorting; // technique keys in the order they run
    fs::path presetPath;
    PresetDefinitions presetDefinitions;
    bool dirty = false;

    // Compiling happens on a loader thread with workers of its own. Compiled effects wait in finished until
    // Update takes them.
    std::thread loader;
    std::atomic<bool> cancel = false;
    std::atomic<bool> loaderRunning = false;
    bool reloadRequested = false;
    std::atomic<size_t> loadedCount = 0;
    std::atomic<size_t> totalCount = 0;
    std::mutex finishedMutex;
    std::deque<Effect> finished;

    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastFrame = startTime;
    float frameTime = 0;
    uint32_t frameCount = 0;
};
} // namespace fx
