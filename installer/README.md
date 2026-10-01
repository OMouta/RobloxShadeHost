# Installer

Unishade Setup is a single native EXE built with the rest of the project. It embeds Unishade.exe and downloads ReShade with full add-on support, every effect package from ReShade's official list, the presets from `presets/`, and optionally the DLSS5 or depth estimation add-on. Its window is drawn with [Dear ImGui](https://github.com/ocornut/imgui), and [miniz](https://github.com/richgel999/miniz) unpacks the effect packages. CMake downloads both at configure time and checks their hashes.

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --target installer
```

Output: `build/installer/Unishade-Setup-<version>.exe`.

## What it does

Setup downloads everything into a new temporary folder with a random name first. A failed or cancelled download stops before the installation folder is touched. Cancelling, or closing the window, also stops a download that is still connecting and ends ReShade's installer if it is running.

ReShade is the version in `vendor/reshade/reshade.ini`, which is also the version whose headers the host is built against, so the menu matches the Dear ImGui that ReShade exports. Setup downloads `ReShade_Setup_<version>_Addon.exe` from reshade.me and checks it against the SHA-256 built into Setup. It keeps the file open so that nothing can replace it between that check and starting it. Setup shows the license from that version's source tag before installing. It runs ReShade's own installer in headless mode against a temporary copy of the host and keeps only `dxgi.dll` and `ReShade.ini`.

Effect packages come from `EffectPackages.ini` in the `crosire/reshade-shaders` repository, the same list the ReShade installer uses. Setup installs every package on the list, not only the ones the list enables by default, because the presets use effects from packages that are off by default there. Each package is extracted into `reshade-shaders/Shaders` and `reshade-shaders/Textures`, skipping the files the list denies. Only these file types are installed: effect sources (`.fx`, `.fxh`), textures ReShade can load (`.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.dds`, `.hdr`, `.cube`), and plain-text licenses and readmes (`.txt`, `.md`, and `LICENSE`, `LICENCE`, `COPYING`, `NOTICE` or `README` without an extension). Add-ons, DLLs, programs and anything else in a package stay out. A package whose zip names a file outside its own folder, or a file name with a colon, which on Windows names a drive or a stream of another file, is left out. The macOS and Linux host unpacks packages with the same code, in `src/package_files.h`. The packages come from the moving branch heads the list names, so they are not checksummed; the file types are what limits them. ReShade's installer writes search paths ending in `\**\**`, which find nothing, so Setup shortens them to `\**`.

An effect package that cannot be downloaded or unpacked is left out, and so is a preset that cannot be downloaded or verified or needs an effect that was not installed. The finish page lists what was left out. Setup stops only when nothing usable can be installed: when ReShade fails, when the list of effect packages cannot be downloaded, when no package can be installed, or when a package the list marks as required fails. The required package holds `ReShade.fxh`, which most other effects include.

The presets component reads `presets/downloads.ini` from the `main` branch, downloads each listed preset, verifies its SHA-256, and checks that every effect the preset references was installed. To add a preset, commit it to `presets/` and add its filename and hash to `presets/downloads.ini`.

The DLSS5 component reads `downloads.ini` from the `dlss5-assets` release, and depth estimation reads the one from the `depth-assets` release. These manifests only say where to download from and whether the add-on is available. The file names and SHA-256 hashes come from `vendor/dlss5/downloads.ini` and `vendor/depth/downloads.ini` and are built into Setup. A downloaded manifest that names other files or other hashes, `enabled=0`, a missing manifest or a failed download skips the add-on, and the finish page says so. Download addresses must be HTTPS URLs without a user name, query or `..` into the GitHub release or the Hugging Face repository the files in `vendor/` use. That only limits where Setup connects; the built-in hashes are what decide whether a file is installed.

DLSS5 and depth estimation do not work together, so the add-ons page allows only one. Once the selected add-on is installed, Setup removes the files of the other one. When the selected add-on is skipped, the add-on that was installed before stays. Picking neither removes both.

Downloads use HTTPS and check certificates for revocation. When the revocation check cannot be completed, for example because the network blocks the revocation servers, Setup repeats the download without it, as browsers do, rather than failing every installation on such networks; a certificate Windows reports as revoked still fails. A proxy that asks to sign in with Windows authentication gets the user's Windows account.

Downloads have size limits, so a broken or hostile server cannot fill the memory or the disk: 16 MB for download lists, presets and the license, 256 MB for an effect package or ReShade's installer, and 1 GB for an add-on file. Before unpacking, an effect package may hold at most 20,000 files, 128 MB per file and 1 GB in total.

Reinstalling keeps `ReShade.ini`, presets, `RobloxShadeHost.ini` and the saved game list in `games.ini`. The configuration and `RobloxShadeHost-Setup.files` manifest retain their original filenames to preserve existing install state without conversion. The host writes `RobloxShadeHost.ini` on its first start and when shortcuts change in its menu.

When Setup creates the installation folder outside the user's profile, such as `C:\Games\Unishade`, the first folder it creates gets its own permissions: full control for the user, SYSTEM and Administrators, and read and run for other users. Otherwise it would inherit the permissions of a drive's root, where every signed-in user may change files, and another account could replace `dxgi.dll` or `Unishade.exe`. Folders that already exist keep their permissions.

Setup copies itself into the installation folder as `Unishade-Setup.exe`, adds **Unishade** and **Unishade Setup** to the Start menu, and registers in Windows' app list under the key earlier Inno Setup versions used, so an update replaces their entry. Running it again from the Start menu offers updating and uninstalling. `RobloxShadeHost-Setup.files` lists the installed files for uninstalling. Setup writes the list also when copying fails partway, so whatever was copied can still be uninstalled. Entries that are absolute, contain a drive, `:` or `..`, or point outside the folder are dropped when the list is read or written, and the setup log names them.

Setup uninstalls only from a Unishade or RobloxShadeHost installation: a folder with `Unishade.exe` or `RobloxShadeHost.exe` and `RobloxShadeHost-Setup.files`. Anywhere else it refuses and deletes nothing.

Uninstalling deletes the files in `RobloxShadeHost-Setup.files`, the logs of the host and ReShade (`Unishade.log`, `Unishade.old.log`, `RobloxShadeHost.log`, `RobloxShadeHost.old.log`, `ReShade.log` and its numbered copies), the Start menu shortcuts and the app list entry, then removes folders that are left empty. It also removes the **Start with Windows** entry (`Unishade` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` and `...\Explorer\StartupApproved\Run`) when it starts the `Unishade.exe` in that folder, so uninstalling one copy leaves another copy's entry alone. It keeps `ReShade.ini`, `ReShadePreset.ini`, `RobloxShadeHost.ini`, `games.ini`, `presets` and the effects the user added to `reshade-shaders`.

When the user also asks to delete their files, it deletes those too: `ReShade.ini`, `ReShadePreset.ini`, `RobloxShadeHost.ini`, `games.ini` and the `presets` and `reshade-shaders` folders with everything in them. Other files, such as screenshots, stay in the folder. Setup never follows links or junctions when it deletes.

Credits open from the sidebar and are installed as `CREDITS.txt`. Removal requests go to **tiago@mouta.me**.

The uninstall identity remains `{77125AF5-DF0A-485A-A633-E64FBD50E90C}_is1`. Upgrading RobloxShadeHost reuses its registered folder, replaces the old executables and Start menu shortcuts with Unishade entries, and keeps user data. New installations default to `%LOCALAPPDATA%\Programs\Unishade`. The host's original window classes and single-instance mutex remain stable for installer compatibility.

Setup only loads system DLLs from System32, since its copy in the installation folder sits next to ReShade's `dxgi.dll`.

## Maintaining the downloads

### ReShade

`vendor/reshade/reshade.ini` holds the ReShade version Setup installs and the SHA-256 of `ReShade_Setup_<version>_Addon.exe`. To move to another ReShade version:

1. Change `version=` in `vendor/reshade/reshade.ini` and empty `sha256=`.
2. In `CMakeLists.txt`, update the hashes of the ReShade headers, which CMake downloads from the same version's tag, and the Dear ImGui header to the version that ReShade release exports. `src/posix/posix.cmake` builds ReShade's effect compiler from the same version.
3. Configure on Windows. CMake downloads the installer from reshade.me to `build/reshade/` and prints a warning with the line to commit, such as `sha256=0123…`. Compare that file with one downloaded in a browser if in doubt, then commit the line.

With a hash in the file, configuring downloads the installer and fails if it does not match. Without one, configuring uses the hash of the file reshade.me sends at that moment and warns. If the download fails as well, Setup is not built until the line is filled in. Setup checks the hash again on every installation.

### Add-ons

The manifest sources and asset release notes are in `vendor/dlss5/` and `vendor/depth/`. The binaries belong in release assets, not Git. The file names and hashes in these manifests are built into Setup, so changing a file needs a new Setup release, and older Setups skip the add-on once the release assets change. Download addresses can change without a new Setup, within the same GitHub release or Hugging Face repository: upload the updated `downloads.ini` to the same release.

To withdraw an add-on, remove its binary assets or upload a manifest with `enabled=0`. Older installers will skip it on their next run. Existing installations are not changed.

## Command line

Setup writes its log to `%TEMP%\Unishade-Setup.log`, or to the file given with `--log`.

Unattended installation of the host only:

```powershell
.\Unishade-Setup-<version>.exe --silent --components host
```

To install ReShade unattended, first read its license and pass `--accept-reshade-license`. `--components` takes a comma-separated list of `reshade`, `presets`, `depth` and `dlss5`; without it, Setup installs `reshade,presets`. Setup stops if both add-ons are selected.

| Option | Effect |
| --- | --- |
| `--silent` | Installs or uninstalls without a window. |
| `--dir <folder>` | Installation folder. Defaults to the registered installation or `%LOCALAPPDATA%\Programs\Unishade`. |
| `--components <list>` | What to install with `--silent`. |
| `--accept-reshade-license` | Required with `--silent` when installing ReShade. |
| `--portable` | Leaves out the Start menu shortcuts, the app list entry and the copy of Setup. |
| `--uninstall` | Opens the uninstall page, or uninstalls with `--silent`. |
| `--delete-user-files` | With `--uninstall --silent`, also deletes `ReShade.ini`, `ReShadePreset.ini`, `RobloxShadeHost.ini`, `games.ini` and the `presets` and `reshade-shaders` folders. |
| `--log <file>` | Where to write the setup log. |

| Exit code | Meaning |
| --- | --- |
| 0 | Everything requested was installed or uninstalled. |
| 1 | Setup failed or was cancelled, or the command line is invalid. A failed installation leaves the folder unchanged. An uninstall refuses a folder that is not an installation, and reports files it could not delete. |
| 2 | Installed, but an effect package, a preset or the requested add-on was left out. The log says which. |

Tests use `--effects-url`, `--presets-url`, `--dlss5-manifest` and `--depth-manifest` to replace the download locations. `--effects-url` and the two manifest options also take the full path of a local file.

## Verification

Run `./tests/installer_tests.ps1` after building the installer. The tests install into fresh folders under `build/installer-tests` with `--portable`, so they do not touch the Start menu or Windows' app list. They check component selection, license acceptance, effect and preset installation, configuration preservation, unavailable and mismatched DLSS5 and depth downloads, uninstalling, that uninstalling refuses other folders and ignores file list entries outside the folder, and that it removes only its own **Start with Windows** entry. That check sets and restores the current user's `Unishade` startup entry. Internet access is required for ReShade, and ReShade's own installer runs several times in the background. On GitHub Actions, the presets come from the commit being tested; elsewhere, pass `-PresetsBaseUrl` to test presets that are not on `main` yet.

Add `-DownloadDLSS` or `-DownloadDepth` to also download the published add-on files and verify their hashes against the repository manifests.
