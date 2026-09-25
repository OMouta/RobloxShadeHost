# Contributing

Bug reports, presets, and code changes are welcome. Open an issue before starting large changes so we can agree on the approach first.

## Report a bug

Use the bug report form. Include the host version, how you installed it, your Windows version, and `RobloxShadeHost.log` from the host's folder. Installer problems need the setup log, `%TEMP%\RobloxShadeHost-Setup.log`.

## Add a preset

Presets live in `presets/`. Every effect a preset uses must come from [ReShade's official package list](https://github.com/crosire/reshade-shaders/blob/list/EffectPackages.ini); the installer downloads all of those and nothing else. Do not reference shaders from other sources.

1. Save the preset from ReShade and copy the `.ini` into `presets/`.
2. Test it with the host on a Roblox experience. Remember that Roblox provides no depth buffer, so depth-based effects do nothing.
3. Add its filename and SHA-256 to `presets/downloads.ini`:

   ```powershell
   (Get-FileHash presets/MyPreset.ini).Hash.ToLower()
   ```

4. Open a pull request with a screenshot or short description of the look.

The installer verifies the hash, so the file must not change after the hash is recorded.

## Build

Visual Studio 2022 with **Desktop development with C++**, a recent Windows SDK, and CMake 3.20 or newer.

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

See `installer/README.md` for building and testing the installer.

## Code changes

- The host is C++20.
- Builds use `/W4`. Fix warnings rather than suppressing them.
- Hotkey parsing has tests in `tests/hotkey_tests.cpp`. Add a case when you change it.
- Installer changes must pass `tests/installer_tests.ps1`. It downloads ReShade and all effect packages, so it takes a few minutes.
- Keep pull requests focused. Separate unrelated fixes into their own pull requests.
- Do not bump the version. Releases are cut from tags by the maintainer.

## Pull requests

CI builds the host, runs the unit tests, and builds the installer on every pull request. Describe what changed and how you tested it. For anything that affects what the user sees, include a screenshot.

## License

Contributions are licensed under the MIT license in `LICENSE`.
