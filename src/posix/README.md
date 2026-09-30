# Unishade for macOS and Linux

ReShade is Windows only, so this host runs ReShade effects itself. It follows the Windows design: it copies the game's window, runs effects on the copy and shows the result in a window over the game, with clicks passing through until the menu opens.

| | macOS | Linux |
| --- | --- | --- |
| Capture | ScreenCaptureKit | XComposite with XShm (X11 and XWayland) |
| Windows and processes | CoreGraphics, `proc_pidpath` | `_NET_CLIENT_LIST`, XRes, `/proc` |
| Shortcuts | Carbon hot keys | `XGrabKey` on the root window |
| Vulkan | MoltenVK, linked directly and copied into the app | The system's loader and driver |

Effects compile with ReShade's own compiler (`effect_*.cpp` from ReShade 6.8.0, BSD-3-Clause) to SPIR-V, the same path ReShade takes in Vulkan games. `effects.cpp` runs what it produces: textures, render targets, storage, compute passes, blending, mipmaps and the uniforms ReShade sets itself, such as `timer` and `frametime`. Presets are ReShade's `.ini` files. The launcher and the menu use Dear ImGui with GLFW.

## Building

Linux (Debian and Ubuntu package names):

```sh
sudo apt install cmake g++ libvulkan-dev libx11-dev libxcomposite-dev libxext-dev libxres-dev \
    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

macOS:

```sh
brew install cmake molten-vk vulkan-headers
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

The Linux build is `build/unishade`. The macOS build is `build/Unishade.app`, with MoltenVK inside and an ad-hoc signature.

## Checking effects without a game

`unishade --render in.png preset.ini out.png` applies a preset to an image with the overlay's own code and no window. It exits with 1 when a technique the preset turns on is missing or fails to start. CI renders the repository's presets this way on Mesa's software Vulkan driver.

Set `UNISHADE_VALIDATION=1` to run with the Vulkan validation layers.

## Files

| File | Does |
| --- | --- |
| `main.cpp` | Command line, single instance, starts `App` |
| `app.cpp` | Finds the game, moves the overlay, shortcuts, presets, screenshots |
| `ui.cpp` | Launcher and menu |
| `effects.cpp` | ReShade effect runtime on Vulkan |
| `gpu.cpp` | Vulkan device, images and window swapchains |
| `platform_x11.cpp`, `platform_macos.mm` | Everything in `platform.h` |
| `setup.cpp` | `--install-effects` and the launcher's download button |
| `config.cpp`, `games.cpp`, `hotkeys.cpp` | `Unishade.ini`, `games.ini` and shortcuts |

Shared with the Windows host, in `src/`: `preset_ini.h` (ReShade presets), `game_list.h` (`games.ini`), `hotkey_text.h` (how shortcuts are written), `ini_text.h` and `theme.h`.
