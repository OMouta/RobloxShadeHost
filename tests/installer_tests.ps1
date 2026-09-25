param(
    [string]$Setup = (Get-ChildItem "$PSScriptRoot/../build/installer/RobloxShadeHost-Setup-*.exe" |
        Sort-Object LastWriteTime | Select-Object -Last 1).FullName,
    [switch]$DownloadDLSS,
    [switch]$DownloadDepth,
    [string]$PresetsBaseUrl
)

$ErrorActionPreference = 'Stop'
if (-not $Setup -or -not (Test-Path $Setup)) { throw 'Build the installer first: cmake --build build --config Release --target installer' }
$repo = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repo ('build/installer-tests/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

# --portable keeps the tests out of the Start menu and Windows' app list.
function Invoke-Setup([string]$Name, [string[]]$Arguments) {
    $arguments = @('--silent', '--log', "`"$testRoot/$Name.log`"") + $Arguments
    if ($PresetsBaseUrl) { $arguments += @('--presets-url', $PresetsBaseUrl) }
    return (Start-Process $Setup -ArgumentList $arguments -Wait -PassThru).ExitCode
}

function Invoke-TestInstaller(
    [string]$Name, [string]$Components, [bool]$AcceptLicense = $true,
    [bool]$ExpectSuccess = $AcceptLicense, [string[]]$Extra = @()
) {
    $destination = Join-Path $testRoot $Name
    $arguments = @('--portable', '--components', $Components, '--dir', "`"$destination`"") + $Extra
    if ($AcceptLicense) { $arguments += '--accept-reshade-license' }
    $exitCode = Invoke-Setup $Name $arguments
    if ($ExpectSuccess -and $exitCode -ne 0) {
        throw "$Name failed with exit code $exitCode. See $testRoot/$Name.log"
    }
    if (-not $ExpectSuccess -and ($exitCode -eq 0 -or (Test-Path $destination))) {
        throw "$Name installed although Setup should have stopped."
    }
    return $destination
}

function Assert-File([string]$Directory, [string]$Name, [bool]$Expected = $true) {
    if ((Test-Path (Join-Path $Directory $Name)) -ne $Expected) {
        throw "Unexpected file state: $Directory/$Name; expected present=$Expected"
    }
}

$hostOnly = Invoke-TestInstaller 'host-only' 'host'
Assert-File $hostOnly 'RobloxShadeHost.exe'
Assert-File $hostOnly 'CREDITS.txt'
Assert-File $hostOnly 'dxgi.dll' $false
Assert-File $hostOnly 'nvngx_dlssnr.dll' $false
Assert-File $hostOnly 'depth-anything-v2-small.onnx' $false
Assert-File $hostOnly 'RobloxShadeHost-Setup.exe' $false

$null = Invoke-TestInstaller 'no-license' 'reshade' $false
$null = Invoke-TestInstaller 'dlss5-and-depth' 'reshade,dlss5,depth' -ExpectSuccess $false
if ((Get-Content "$testRoot/dlss5-and-depth.log" -Raw) -notmatch 'do not work together') {
    throw 'Setup did not refuse DLSS5 and depth estimation together.'
}
$reshade = Invoke-TestInstaller 'reshade' 'reshade,presets'
Assert-File $reshade 'dxgi.dll'
Assert-File $reshade 'ReShade-LICENSE.txt'
Assert-File $reshade 'renodx-dlss.addon64' $false
Assert-File $reshade 'onnxruntime.dll' $false
# ReShade's installer leaves these next to the exe; only its DLL and settings are installed.
Assert-File $reshade 'ReShadePreset.ini' $false
Assert-File $reshade 'ReShade.log' $false
Assert-File $reshade 'reshade-shaders/Shaders/ReShade.fxh'
Assert-File $reshade 'reshade-shaders/Shaders/FXShaders/AdaptiveTonemapper.fx'
Assert-File $reshade 'reshade-shaders/Shaders/qUINT/qUINT_common.fxh'
if (-not (Get-ChildItem "$reshade/reshade-shaders/Textures" -Filter *.png -Recurse | Select-Object -First 1)) {
    throw 'No textures were installed.'
}
function Assert-SearchPaths([string]$Ini) {
    if ($Ini -notmatch '(?m)^EffectSearchPaths=\.\\reshade-shaders\\Shaders\\\*\*\r?$' -or
        $Ini -notmatch '(?m)^TextureSearchPaths=\.\\reshade-shaders\\Textures\\\*\*\r?$') {
        throw 'ReShade.ini does not have the expected effect search paths.'
    }
}

$reshadeIni = Get-Content "$reshade/ReShade.ini" -Raw
Assert-SearchPaths $reshadeIni
if ($reshadeIni -notmatch '(?m)^PresetPath=\.\\presets\\ReShadePreset\.ini\r?$') {
    throw 'The initial preset browser path is not in the presets folder.'
}
if (-not (Compare-Object ([IO.File]::ReadAllBytes("$reshade/ReShade.ini")[0..2]) @(0xEF, 0xBB, 0xBF)) -eq $null) {
    throw 'ReShade.ini lost its byte order mark.'
}
if (([regex]::Matches($reshadeIni, '(?m)^\[GENERAL\]')).Count -ne 1) {
    throw 'ReShade.ini has a duplicated GENERAL section.'
}
foreach ($preset in Get-ChildItem "$repo/presets/*.ini" -Exclude downloads.ini) {
    if ((Get-FileHash "$reshade/presets/$($preset.Name)").Hash -ne (Get-FileHash $preset.FullName).Hash) {
        throw "Installed preset $($preset.Name) does not match the repository."
    }
}
if ((Get-Content "$reshade/CREDITS.txt" -Raw) -notmatch 'tiago@mouta.me') {
    throw 'Removal contact is missing from installed credits.'
}

Add-Content "$reshade/ReShade.ini" "`n[InstallerTest]`nPreserve=1"
$originalHash = (Get-FileHash "$reshade/ReShade.ini").Hash
Set-Content "$reshade/presets/GenericPreset1.ini" 'Techniques=Edited@Edited.fx'
Set-Content "$reshade/ReShadePreset.ini" 'Techniques=Mine@Mine.fx'
$null = Invoke-TestInstaller 'reshade' 'reshade,presets'
if ((Get-FileHash "$reshade/ReShade.ini").Hash -ne $originalHash) {
    throw 'Reinstall changed the existing ReShade configuration.'
}
if ((Get-Content "$reshade/ReShadePreset.ini" -Raw) -notmatch 'Mine') {
    throw "Reinstall overwrote the user's ReShadePreset.ini."
}

# An install from an earlier installer kept ReShade's doubled search paths. Reinstalling repairs
# those lines and nothing else.
$brokenIni = $reshadeIni -replace '(?m)^((?:Effect|Texture)SearchPaths=.*\\\*\*)(?=\r?$)', '$1\**'
$brokenIni += "`n[InstallerTest]`nPreserve=1`n"
[IO.File]::WriteAllText("$reshade/ReShade.ini", $brokenIni.TrimStart([char]0xFEFF), [Text.UTF8Encoding]::new($true))
$null = Invoke-TestInstaller 'reshade' 'reshade,presets'
$repairedIni = Get-Content "$reshade/ReShade.ini" -Raw
Assert-SearchPaths $repairedIni
if ($repairedIni -notmatch '(?m)^Preserve=1' -or ([regex]::Matches($repairedIni, '(?m)^\[GENERAL\]')).Count -ne 1) {
    throw 'Repairing the search paths did not preserve the rest of ReShade.ini.'
}
if ((Get-Content "$reshade/presets/GenericPreset1.ini" -Raw) -notmatch 'Edited') {
    throw 'Reinstall overwrote an edited preset.'
}

$missingManifests = @(
    '--dlss5-manifest', 'https://github.com/OMouta/RobloxShadeHost/releases/download/dlss5-assets/not-present.ini',
    '--depth-manifest', 'https://github.com/OMouta/RobloxShadeHost/releases/download/depth-assets/not-present.ini')
$missing = Invoke-TestInstaller 'missing-dlss5' 'reshade,dlss5' -Extra $missingManifests
Assert-File $missing 'RobloxShadeHost.exe'
Assert-File $missing 'dxgi.dll'
Assert-File $missing 'nvngx_dlssnr.dll' $false
Assert-File $missing 'renodx-dlss.addon64' $false
if ((Get-Content "$testRoot/missing-dlss5.log" -Raw) -notmatch 'DLSS5 skipped:') {
    throw 'Missing DLSS5 downloads were not reported.'
}
$missing = Invoke-TestInstaller 'missing-depth' 'reshade,depth' -Extra $missingManifests
Assert-File $missing 'dxgi.dll'
Assert-File $missing 'onnxruntime.dll' $false
Assert-File $missing 'DirectML.dll' $false
Assert-File $missing 'depth-anything-v2-small.onnx' $false
if ((Get-Content "$testRoot/missing-depth.log" -Raw) -notmatch 'Depth estimation skipped:') {
    throw 'Missing depth estimation downloads were not reported.'
}

if ($DownloadDLSS) {
    # Installing DLSS5 over depth estimation removes the depth files.
    New-Item -ItemType Directory -Path "$testRoot/full" -Force | Out-Null
    Set-Content "$testRoot/full/depth-anything-v2-small.onnx" 'stale'
    $full = Invoke-TestInstaller 'full' 'reshade,dlss5'
    Assert-File $full 'depth-anything-v2-small.onnx' $false
    Assert-File $full 'nvngx_dlssnr.dll'
    Assert-File $full 'renodx-dlss.addon64'
    $manifest = Get-Content "$repo/vendor/dlss5/downloads.ini" -Raw
    foreach ($file in @('nvngx_dlssnr.dll', 'renodx-dlss.addon64')) {
        $pattern = '(?ms)^\[' + [regex]::Escape($file) + '\]\r?\n.*?^sha256=([a-f0-9]{64})'
        $expectedHash = [regex]::Match($manifest, $pattern).Groups[1].Value
        if ((Get-FileHash "$full/$file").Hash -ne $expectedHash) {
            throw "$file does not match the repository manifest."
        }
    }
}

if ($DownloadDepth) {
    # Installing depth estimation over DLSS5 removes the DLSS5 files.
    New-Item -ItemType Directory -Path "$testRoot/depth" -Force | Out-Null
    Set-Content "$testRoot/depth/renodx-dlss.addon64" 'stale'
    $depth = Invoke-TestInstaller 'depth' 'reshade,depth'
    Assert-File $depth 'renodx-dlss.addon64' $false
    $manifest = Get-Content "$repo/vendor/depth/downloads.ini" -Raw
    foreach ($file in @('onnxruntime.dll', 'DirectML.dll', 'depth-anything-v2-small.onnx')) {
        Assert-File $depth $file
        $pattern = '(?ms)^\[' + [regex]::Escape($file) + '\]\r?\n.*?^sha256=([a-f0-9]{64})'
        $expectedHash = [regex]::Match($manifest, $pattern).Groups[1].Value
        if ((Get-FileHash "$depth/$file").Hash -ne $expectedHash) {
            throw "$file does not match the repository manifest."
        }
    }
}

# Uninstalling keeps ReShade settings and presets unless asked to delete them too.
if ((Invoke-Setup 'uninstall' @('--uninstall', '--dir', "`"$reshade`"")) -ne 0) {
    throw "Uninstall failed. See $testRoot/uninstall.log"
}
foreach ($file in @('RobloxShadeHost.exe', 'dxgi.dll', 'CREDITS.txt', 'reshade-shaders')) { Assert-File $reshade $file $false }
Assert-File $reshade 'ReShade.ini'
Assert-File $reshade 'presets/GenericPreset1.ini'
if ((Invoke-Setup 'uninstall-all' @('--uninstall', '--delete-user-files', '--dir', "`"$reshade`"")) -ne 0 -or (Test-Path $reshade)) {
    throw "Uninstall did not delete the folder. See $testRoot/uninstall-all.log"
}

Write-Output "Installer checks passed. Test files and logs: $testRoot"
