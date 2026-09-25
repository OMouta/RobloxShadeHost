# Installer

RobloxShadeHost Setup is a single native EXE built with the rest of the project. It embeds RobloxShadeHost.exe and downloads ReShade with full add-on support, every effect package from ReShade's official list, the presets from `presets/`, and optionally the DLSS5 or depth estimation add-on. Its window is drawn with [Dear ImGui](https://github.com/ocornut/imgui), and [miniz](https://github.com/richgel999/miniz) unpacks the effect packages. CMake downloads both at configure time and checks their hashes.

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --target installer
```

Output: `build/installer/RobloxShadeHost-Setup-<version>.exe`.

## What it does

Setup downloads everything into a temporary folder first. A failed or cancelled download stops before the installation folder is touched. An add-on that cannot be downloaded is skipped instead, and the finish page says so.

ReShade comes from reshade.me at installation time. Setup shows the license from that version's source tag before installing. It runs ReShade's own installer in headless mode against a temporary copy of the host and keeps only `dxgi.dll` and `ReShade.ini`.

Effect packages come from `EffectPackages.ini` in the `crosire/reshade-shaders` repository, the same list the ReShade installer uses. Each package is extracted into `reshade-shaders/Shaders` and `reshade-shaders/Textures`, skipping the files the list denies. ReShade's installer writes search paths ending in `\**\**`, which find nothing, so Setup shortens them to `\**`.

The presets component reads `presets/downloads.ini` from the `main` branch, downloads each listed preset, verifies its SHA-256, and checks that every effect the preset references was installed. To add a preset, commit it to `presets/` and add its filename and hash to `presets/downloads.ini`.

The DLSS5 component reads `downloads.ini` from the `dlss5-assets` release, and depth estimation reads the one from the `depth-assets` release. Every file must pass its SHA-256 check. A missing or invalid manifest, `enabled=0`, or a failed download skips the add-on. Manifest URLs must point at this repository's releases or at huggingface.co.

DLSS5 and depth estimation do not work together, so the add-ons page allows only one. Installing removes the files of the add-on that is not selected.

Reinstalling keeps `ReShade.ini`, presets and `RobloxShadeHost.ini`. After installing, Setup shows the shortcuts page, which writes `RobloxShadeHost.ini`.

Setup copies itself into the installation folder as `RobloxShadeHost-Setup.exe`, adds **RobloxShadeHost** and **RobloxShadeHost Setup** to the Start menu, and registers in Windows' app list under the key earlier Inno Setup versions used, so an update replaces their entry. Running it again from the Start menu offers updating, changing shortcuts and uninstalling. `RobloxShadeHost-Setup.files` lists the installed files for uninstalling. Uninstalling keeps `ReShade.ini`, presets and `RobloxShadeHost.ini` unless the user asks to delete them.

Credits open from the sidebar and are installed as `CREDITS.txt`. Removal requests go to **tiago@mouta.me**.

## Maintaining the downloads

The manifest sources and asset release notes are in `vendor/dlss5/` and `vendor/depth/`. The binaries belong in release assets, not Git. Upload updated `downloads.ini` to the same release when changing download URLs or checksums. Setup reads that release asset, so an application release is not required to update it.

To withdraw an add-on, remove its binary assets or upload a manifest with `enabled=0`. Older installers will skip it on their next run. Existing installations are not changed.

## Command line

Setup writes its log to `%TEMP%\RobloxShadeHost-Setup.log`, or to the file given with `--log`.

Unattended installation of the host only:

```powershell
.\RobloxShadeHost-Setup-0.3.6.exe --silent --components host
```

To install ReShade unattended, first read its license and pass `--accept-reshade-license`. `--components` takes a comma-separated list of `reshade`, `presets`, `depth` and `dlss5`; without it, Setup installs `reshade,presets`. Setup stops if both add-ons are selected. The exit code is 0 on success and 1 on failure.

| Option | Effect |
| --- | --- |
| `--silent` | Installs or uninstalls without a window. |
| `--dir <folder>` | Installation folder. Defaults to the registered installation or `%LOCALAPPDATA%\Programs\RobloxShadeHost`. |
| `--components <list>` | What to install with `--silent`. |
| `--accept-reshade-license` | Required with `--silent` when installing ReShade. |
| `--portable` | Leaves out the Start menu shortcuts, the app list entry and the copy of Setup. |
| `--uninstall` | Opens the uninstall page, or uninstalls with `--silent`. |
| `--delete-user-files` | With `--uninstall --silent`, also deletes settings and presets. |
| `--log <file>` | Where to write the setup log. |

Tests use `--effects-url`, `--presets-url`, `--dlss5-manifest` and `--depth-manifest` to replace the download locations.

## Verification

Run `./tests/installer_tests.ps1` after building the installer. The tests install into fresh folders under `build/installer-tests` with `--portable`, so they do not touch the Start menu or Windows' app list. They check component selection, license acceptance, effect and preset installation, configuration preservation, unavailable DLSS5 and depth downloads, and uninstalling. Internet access is required for ReShade, and ReShade's own installer runs several times in the background.

Add `-DownloadDLSS` or `-DownloadDepth` to also download the published add-on files and verify their hashes against the repository manifests.
