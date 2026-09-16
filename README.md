# RobloxShadeHost

Use ReShade with Roblox. Install ReShade on RobloxShadeHost, run the host next to Roblox, and the effects draw over your game.

> [!WARNING]
> DLSS5 can break with NVIDEA Driver updates, you will need to wait for a new version for it to start working again.
> Last DLSS5 update: 13/09/2026

## Read this first

**How do I get it?** Download [RobloxShadeHost-Setup.exe](https://github.com/OMouta/RobloxShadeHost/releases/latest/download/RobloxShadeHost-Setup.exe) and run it like any other installer. It downloads ReShade and its effects for you. Keep the folder it suggests, or pick any folder of your own. Do not install it inside the Roblox folder, and do not install ReShade onto Roblox itself. The host is a separate program that runs beside Roblox and never touches Roblox's files.

**Will it slow my game down?** Yes. This is experimental and lowers your FPS, because the host copies Roblox's picture every frame and draws the effects on top.
**How do I open the ReShade menu?** While you play, your keyboard and mouse go to Roblox. Press **Ctrl+Home** to hand them to RobloxShadeHost instead. A small badge at the bottom of the screen confirms it. Now press **Home** to open ReShade, pick a preset or change effects, and press **Home** again to close it. Press **Ctrl+Home** once more to go back to playing. Your effects stay on.

## Download and set up

Use 64-bit Windows 10 version 1903 or newer, or Windows 11. Windows Graphics Capture must be available on your system.

### Installer (recommended)

1. Download [**RobloxShadeHost-Setup.exe**](https://github.com/OMouta/RobloxShadeHost/releases/latest/download/RobloxShadeHost-Setup.exe).
2. Run the installer and choose an installation folder.
3. Keep **ReShade with full add-on support** and **RobloxShadeHost presets** selected. Optionally select the **DLSS5 add-on**, which requires ReShade.
4. Accept the ReShade license and finish installation. If the optional downloads are unavailable, the installer skips them and installs the other components.
5. Open Roblox and launch **RobloxShadeHost** from the Start menu. Either can be started first; the host waits if Roblox is not open yet.

The installer downloads every effect package from ReShade's official list. The presets component installs the presets from this repository's `presets` folder next to the host; load one from the ReShade menu.

### Manual installation

1. Download **RobloxShadeHost.exe** from the [latest release](https://github.com/OMouta/RobloxShadeHost/releases/latest).
2. Put it in its own folder somewhere you can write files, such as `Desktop\RobloxShadeHost`. Keep it in that folder after installing ReShade.
3. Download a current ReShade installer from [reshade.me](https://reshade.me/#download). **The version with full add-on support is recommended.** The standard version also works with the host's input toggle and indicator.
4. Run the ReShade installer. Browse to **RobloxShadeHost.exe**, not the Roblox executable.
5. Select **Microsoft DirectX 10/11/12** as the rendering API.
6. Select your preset if you have one, choose the effect packages you want, and finish installation.
7. Open Roblox and run **RobloxShadeHost.exe**. Either can be started first. If Roblox is not open yet, the host waits for it.

**Old DirectX ReShade installations, such as DirectX 9 or earlier, will not work with this host.** Choose DirectX 10/11/12 even if a guide for another program tells you otherwise. Use a current ReShade release rather than copying an old installation from another game.

Keep the files ReShade installs beside RobloxShadeHost.exe, including `dxgi.dll`, `ReShade.ini`, and the shader folders. ReShade is downloaded separately and is not bundled with the EXE.

## Depth-based effects

Roblox's depth buffer is not available outside its process, so the host estimates depth from the captured image with Depth Anything V2. Ambient occlusion, depth of field, fog and other effects that read depth then work from that estimate. Objects that look close in the image are close in the estimate, but distances are relative and edges are softer than a real depth buffer.

The installer does not offer depth estimation while it is being fixed. To try it anyway, put `onnxruntime.dll` and `DirectML.dll` from the [depth-assets release](https://github.com/OMouta/RobloxShadeHost/releases/tag/depth-assets) and the [model](https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/onnx/model_fp16.onnx) saved as `depth-anything-v2-small.onnx` into the host folder. The add-on needs ReShade with full add-on support and a DirectX 12 capable GPU.

The model shares the GPU with Roblox and costs some frame rate. If that matters more than depth effects, disable **RobloxShadeHost depth** in ReShade's Add-ons tab or delete the model file.

## Open the menu and adjust effects

The host's input shortcut and ReShade's menu shortcut are separate. With the defaults:

1. Click into Roblox and press **Ctrl+Home**. The host now receives mouse and keyboard input. A badge at the bottom says **Input captured** and shows the shortcut to return to Roblox.
2. Press **Home** to open ReShade. Follow its first-run tutorial, choose a preset, or adjust your effects.
3. Press **Home** again to close the ReShade menu.
4. Press **Ctrl+Home** again to return input to Roblox and keep playing. Your effects remain visible.

If you changed ReShade's menu shortcut, use that instead of Home. Closing the ReShade menu does not release input by itself. Use the shortcut shown in the badge to return to Roblox.

ReShade's effect toggle and individual effect shortcuts work while the host has input. To toggle effects, capture input, press the shortcut configured in ReShade, then return input to Roblox. The host does not forward those shortcuts while you are playing.

Switching to another application releases input capture. To edit again, return to Roblox and press the input shortcut.

## Change the input shortcut

The first launch creates **RobloxShadeHost.ini** beside the EXE:

```ini
[Input]
ToggleKey=Ctrl+Home
```

Close the host, edit `ToggleKey` in a text editor, save, and start the host again. For example:

```ini
[Input]
ToggleKey=F8
```

Or use a combination such as `Ctrl+Shift+F8` or `Alt+Insert`.

Supported keys:

- Letters `A` through `Z` and digits `0` through `9`.
- `F1` through `F24`, except `F12`, which Windows reserves.
- `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, `Pause`, `ScrollLock`, `Space`, `Tab`, and `Escape`.
- Optional modifiers `Ctrl`, `Alt`, `Shift`, and `Win`, placed before the key and separated by `+`.

Names are case-insensitive. Use a shortcut different from ReShade's menu and effect shortcuts. Avoid keys you use for gameplay and Windows shortcuts such as Alt+Tab. Windows or another application may already own a combination; the host will show an error if it cannot register yours.

The shortcut is reserved while the host is running. Exit the host to free it for other applications.

## Everyday use

- Keep the host running while you play. Its console window shows capture status and errors; you can minimize it.
- The overlay follows the Roblox window. It hides when Roblox is minimized or you switch away after releasing input.
- If Roblox closes, the host waits for it to open again.
- To stop, close the host's console window. Roblox continues running.
- To update, close the host and replace RobloxShadeHost.exe with the new download. Keep your INI files, presets, and shader folders.
- To uninstall, close the host and delete its folder after saving any presets you want to keep.

## Troubleshooting

### ReShade does not appear

Check that you installed it on **RobloxShadeHost.exe** using **DirectX 10/11/12**, then restart the host. Start a Roblox experience and bring its window to the foreground. Press the input shortcut before pressing ReShade's menu shortcut.

### I cannot move or click in Roblox

If the **Input captured** badge is visible, press the shortcut shown there to return input to Roblox. Closing the ReShade menu alone does not do this.

### My shortcut does not work

Bring Roblox to the foreground. Check `ToggleKey` in RobloxShadeHost.ini and restart the host after changing it. If the host reports that the shortcut is unavailable, choose another combination and check that you have not started a second copy of the host.

### A shader needs depth information

See [Depth-based effects](#depth-based-effects). Without it, the host only has Roblox's visible image, and effects that require depth will not work as intended. The estimate is not Roblox's own depth buffer, so effects that expect exact distances can need retuning.

### The host reports that capture is unsupported

Check your Windows version and graphics drivers. The host needs Windows Graphics Capture. If reporting another failure, include the error printed in the console and your Windows version in a [GitHub issue](https://github.com/OMouta/RobloxShadeHost/issues).

## Build from source

For development, install Visual Studio 2022 with **Desktop development with C++**, a recent Windows SDK, and CMake 3.20 or newer. From the repository folder:

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The EXE is at `build\Release\RobloxShadeHost.exe`.

GitHub Actions builds and tests the EXE for pushes and pull requests. You can also run **Build and release** manually from the Actions tab. Successful builds provide a `RobloxShadeHost-windows-x64` artifact containing the EXE.

To publish a release, push a version tag such as `v0.1.0`. After the build and tests pass, the workflow creates a GitHub release with the EXE attached. Branch pushes and manual builds do not publish releases.

Want to help? See [CONTRIBUTING.md](CONTRIBUTING.md) for bug reports, presets, and code changes.

## License

[MIT](LICENSE).
