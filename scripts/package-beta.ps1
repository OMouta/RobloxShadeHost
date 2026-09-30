# Builds Unishade and packs a portable copy for testers into build/beta. The zip has ReShade, every official effect
# and the presets, without the depth estimation and DLSS5 add-ons. Testers unzip it and run Unishade.exe.
#
#   ./scripts/package-beta.ps1

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build'
$out = Join-Path $build 'beta'

$version = [regex]::Match((Get-Content "$repo/CMakeLists.txt" -Raw), 'project\(Unishade VERSION ([\d.]+)').Groups[1].Value
$commit = git -C $repo rev-parse --short HEAD

cmake -S $repo -B $build -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
cmake --build $build --config Release --target installer --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }

# Setup does the actual install, the same way it does for users. --portable keeps it out of the Start menu and
# Windows' app list.
$folder = Join-Path $out 'Unishade'
$log = Join-Path $out 'setup.log'
if (Test-Path $folder) { Remove-Item $folder -Recurse -Force }
New-Item -ItemType Directory -Path $out -Force | Out-Null
# Presets come from main on the GitHub repository origin points at, which may still have its old name.
$repository = [regex]::Match((git -C $repo remote get-url origin), 'github\.com[:/](.+?)(\.git)?$').Groups[1].Value
$arguments = @('--silent', '--portable', '--components', 'reshade,presets', '--accept-reshade-license',
    '--presets-url', "https://raw.githubusercontent.com/$repository/main/presets",
    '--dir', "`"$folder`"", '--log', "`"$log`"")
$setup = Join-Path $build "installer/Unishade-Setup-$version.exe"
$exitCode = (Start-Process $setup -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru).ExitCode
if ($exitCode -ne 0) { throw "Setup failed with exit code $exitCode. See $log" }

# Setup's uninstall list means nothing outside an installation.
Remove-Item (Join-Path $folder 'RobloxShadeHost-Setup.files') -ErrorAction SilentlyContinue

$zip = Join-Path $out "Unishade-$version-beta-$commit.zip"
Compress-Archive -Path $folder -DestinationPath $zip -Force
Write-Host "Packed $zip"
