// Unishade for macOS and Linux redraws the game's window with ReShade effects in a window of its own over the
// game, the same design as on Windows. The game is only observed from outside, through its window and
// ScreenCaptureKit or XComposite. No game memory is read and no code is injected.

#include "app.h"
#include "log.h"
#include "setup.h"

#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
void Usage()
{
    printf("Unishade %s\n\n"
           "  unishade                      Starts the launcher.\n"
           "  unishade --install-effects    Downloads effects and presets into %s.\n"
           "  unishade --render IN.png PRESET.ini OUT.png [--gpu-source]\n"
           "                                Applies a preset to an image without a window, to check effects.\n"
           "                                --gpu-source hands the image over on the graphics card, as zero-copy\n"
           "                                capture does.\n",
           UNISHADE_VERSION, DataDirectory().c_str());
}

// Applies a preset to an image with the same code the overlay uses, without any window. gpuSource hands the
// picture over the way zero-copy capture does: from an image on the graphics card, at an offset inside it.
int Render(const char* input, const char* preset, const char* output, bool gpuSource)
{
    std::string error;
    if (!gpu.Init(true, error))
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* rgba = stbi_load(input, &width, &height, &channels, 4);
    if (!rgba)
    {
        fprintf(stderr, "Could not read %s.\n", input);
        return 1;
    }
    std::vector<uint32_t> pixels(size_t(width) * height);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = 0xFF000000u | uint32_t(rgba[i * 4]) << 16 | uint32_t(rgba[i * 4 + 1]) << 8 | rgba[i * 4 + 2];
    stbi_image_free(rgba);

    fx::Runtime runtime;
    if (!runtime.Init(LoadSettings()))
        return 1;
    runtime.LoadPreset(fs::absolute(preset));
    runtime.SetSize(width, height);
    while (runtime.Loading())
    {
        runtime.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    runtime.Update();
    int failed = 0;
    for (const std::string& missing : runtime.MissingTechniques())
    {
        fprintf(stderr, "Missing: %s\n", missing.c_str());
        ++failed;
    }
    fx::Runtime::Source source{ pixels.data() };
    GpuImage sourceImage;
    GpuBuffer upload;
    if (gpuSource)
    {
        constexpr uint32_t kX = 7, kY = 3;
        if (!gpu.CreateImage(sourceImage, width + 16, height + 8, 1, VK_FORMAT_B8G8R8A8_UNORM,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !gpu.CreateBuffer(upload, pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
            return 1;
        std::memcpy(upload.mapped, pixels.data(), pixels.size() * 4);
        VkCommandBuffer commands = gpu.BeginCommands();
        InitLayout(commands, sourceImage);
        // Something else around the picture, which must not end up in it.
        VkClearColorValue magenta{};
        magenta.float32[0] = magenta.float32[2] = magenta.float32[3] = 1.0f;
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(commands, sourceImage.image, VK_IMAGE_LAYOUT_GENERAL, &magenta, 1, &range);
        FullBarrier(commands);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageOffset = { int32_t(kX), int32_t(kY), 0 };
        copy.imageExtent = { uint32_t(width), uint32_t(height), 1 };
        vkCmdCopyBufferToImage(commands, upload.buffer, sourceImage.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        gpu.SubmitAndWait(commands);
        source = { nullptr, sourceImage.image, kX, kY, false };
    }
    // A few frames, so effects that build on earlier frames settle.
    for (int frame = 0; frame < 3; ++frame)
    {
        VkCommandBuffer commands = gpu.BeginCommands();
        runtime.Render(commands, source, true);
        gpu.SubmitAndWait(commands);
    }
    if (gpuSource)
    {
        // The picture read back from the source image must be the input, exactly.
        const std::vector<uint8_t> original = runtime.ReadSource(source);
        for (size_t i = 0; i < pixels.size(); ++i)
            if (original[i * 4] != ((pixels[i] >> 16) & 0xFF) || original[i * 4 + 1] != ((pixels[i] >> 8) & 0xFF) ||
                original[i * 4 + 2] != (pixels[i] & 0xFF))
            {
                fprintf(stderr, "The picture read from the source image differs from the input.\n");
                ++failed;
                break;
            }
    }
    for (const fx::Technique& technique : runtime.Techniques())
        if (technique.enabled && (!runtime.Effects()[technique.effect].gpu || runtime.Effects()[technique.effect].gpuFailed))
        {
            fprintf(stderr, "Failed: %s\n", fx::TechniqueKey(technique, runtime.Effects()[technique.effect]).c_str());
            ++failed;
        }
    const std::vector<uint8_t> result = runtime.ReadOutput();
    const bool written = stbi_write_png(output, width, height, 4, result.data(), width * 4);
    gpu.DestroyImage(sourceImage);
    gpu.DestroyBuffer(upload);
    runtime.Shutdown();
    gpu.Shutdown();
    if (!written)
    {
        fprintf(stderr, "Could not write %s.\n", output);
        return 1;
    }
    return failed ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            Usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version"))
        {
            printf("%s\n", UNISHADE_VERSION);
            return 0;
        }
        // Tests replace the download locations, as with Setup on Windows.
        if (!strcmp(argv[i], "--effects-url") && i + 1 < argc)
            setupSources.effects = argv[++i];
        else if (!strcmp(argv[i], "--presets-url") && i + 1 < argc)
            setupSources.presets = argv[++i];
        else if (!strcmp(argv[i], "--install-effects"))
        {
            InitLog();
            return EffectSetup::RunInTerminal();
        }
        else if (!strcmp(argv[i], "--render"))
        {
            if (i + 3 >= argc)
            {
                Usage();
                return 2;
            }
            InitLog();
            const bool gpuSource = i + 4 < argc && !strcmp(argv[i + 4], "--gpu-source");
            return Render(argv[i + 1], argv[i + 2], argv[i + 3], gpuSource);
        }
    }

    // One host at a time, like the mutex on Windows.
    const int lock = open((DataDirectory() / "unishade.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0)
    {
        fprintf(stderr, "Unishade is already running.\n");
        return 0;
    }

    InitLog();
    glfwSetErrorCallback([](int code, const char* description) { Log(LogLevel::Warning, "GLFW error %d: %s", code, description); });
#ifndef __APPLE__
    // Only X11 lets the overlay sit over another program's window.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    // MoltenVK on macOS is linked directly, without a loader for GLFW to find.
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwInit())
    {
        fprintf(stderr, "Unishade could not open a window. It needs an X11 or XWayland display on Linux.\n");
        return 1;
    }

    std::string error;
    int result = 0;
    if (app.Init(error))
        app.Run();
    else
    {
        Log(LogLevel::Error, "%s", error.c_str());
        fprintf(stderr, "Unishade cannot start: %s\n", error.c_str());
        result = 1;
    }
    app.Shutdown();
    glfwTerminate();
    close(lock);
    return result;
}
