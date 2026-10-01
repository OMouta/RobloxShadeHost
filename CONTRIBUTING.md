# Contributing

Bug reports, presets, and code changes are welcome. Open an issue before starting large changes so we can agree on the approach first.

## Report a bug

Use the bug report form. Include the host version, how you installed it, your Windows version, and `Unishade.log` from the host's folder. Installer problems need the setup log, `%TEMP%\Unishade-Setup.log`.

## Add a preset

Presets live in `presets/`. Every effect a preset uses must come from [ReShade's official package list](https://github.com/crosire/reshade-shaders/blob/list/EffectPackages.ini); the installer downloads all of those and nothing else. Do not reference shaders from other sources.

1. Save the preset from ReShade and copy the `.ini` into `presets/`.
2. Test it with the host on a supported game and name the game in your submission. Roblox provides no depth buffer; depth effects require the optional depth estimation add-on.
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

The EXE is at `build\Release\Unishade.exe`. See `installer/README.md` for building and testing the installer, and [src/posix](src/posix/README.md) for macOS and Linux.

GitHub Actions builds and tests the Windows, Linux and macOS versions for pushes and pull requests, and runs `tests/installer_tests.ps1` against the new Setup. You can also run **Build and release** manually from the Actions tab. Successful builds provide `Unishade-windows-x64` (the EXE and Setup), `Unishade-linux-x64` and `Unishade-macOS` artifacts.

To publish a release, push a tag `vX.Y.Z` that matches `project(Unishade VERSION X.Y.Z)` in `CMakeLists.txt`; the workflow refuses any other. After the builds and tests pass, it creates a GitHub release with Setup, the EXE, the macOS and Linux archives, `LICENSE` and `SHA256SUMS.txt`, which lists the SHA-256 of each file, and then rebuilds the website. Branch pushes and manual builds do not publish releases.

## Website

[unishade.me](https://unishade.me) is built from `website/` with Astro. The docs are the Markdown files in `website/src/content/docs/`.

```powershell
cd website
pnpm install
pnpm dev
```

The download buttons link to the newest release's files, which the build looks up on GitHub. If GitHub can't be reached, the build still succeeds and the buttons link to the Releases page. Set `GITHUB_TOKEN` to avoid GitHub's limit of 60 requests an hour, and `GITHUB_REPOSITORY` to build against another repository; links to the repository follow it too. Setting `GITHUB_API_URL` to an address that doesn't answer shows the site as it builds offline.

In the Markdown pages, link to Discord and the other addresses in `website/src/links.ts` by name, as `[Discord](links:discord)`. A name that isn't in `links.ts` fails the build.

Pull requests that change the site build it. A push to main that changes it deploys it, and publishing, editing or deleting a release deploys it again so the buttons point at the newest files.

## Code changes

Game discovery lives in `src/game_integration.cpp`. `FindGameTarget()` uses a manual window selection or the executable list saved in `games.ini`, with Roblox enabled by default. Custom games match their full executable path; Roblox matches its filename because its install folder changes with updates. Discovery queries executable metadata without reading game memory or injecting code.

- The host is C++20.
- Builds use `/W4`. Fix warnings rather than suppressing them.
- Hotkey parsing has tests in `tests/hotkey_tests.cpp`. Add a case when you change it.
- Installer changes must pass `tests/installer_tests.ps1`. It downloads ReShade and all effect packages, so it takes a few minutes. CI runs it too, with the presets from your commit.
- Keep pull requests focused. Separate unrelated fixes into their own pull requests.
- Do not bump the version. Releases are cut from tags by the maintainer.

## Pull requests

CI builds the Windows, Linux and macOS versions, runs the unit tests and the installer tests, renders the presets on Linux, and builds the website when it changes. Describe what changed and how you tested it. For anything that affects what the user sees, include a screenshot.

## License

Contributions are licensed under the MIT license in `LICENSE`.
