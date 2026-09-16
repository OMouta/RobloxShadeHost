param(
    [string]$Compiler = "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe",
    [switch]$DownloadDLSS,
    [string]$PresetsBaseUrl
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repo ('build/installer-tests/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

function Build-TestInstaller([string]$Name, [string]$ManifestUrl = '') {
    $arguments = @('/Q', '/DTestMode', "/O$testRoot", "/F$Name")
    if ($ManifestUrl) { $arguments += "/DDownloadManifestUrl=$ManifestUrl" }
    if ($PresetsBaseUrl) { $arguments += "/DPresetsBaseUrl=$PresetsBaseUrl" }
    & $Compiler @arguments "$repo/installer/RobloxShadeHost.iss"
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
    return Join-Path $testRoot "$Name.exe"
}

function Invoke-TestInstaller(
    [string]$Setup, [string]$Name, [string]$Components, [bool]$AcceptLicense = $true
) {
    $destination = Join-Path $testRoot $Name
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/NOICONS',
        "/COMPONENTS=$Components", "/DIR=`"$destination`"", "/LOG=`"$testRoot/$Name.log`"")
    if ($AcceptLicense) { $arguments += '/ACCEPTRESHADELICENSE=1' }
    $process = Start-Process $Setup -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($AcceptLicense -and $process.ExitCode -ne 0) {
        throw "$Name failed with exit code $($process.ExitCode). See $testRoot/$Name.log"
    }
    if (-not $AcceptLicense -and ($process.ExitCode -eq 0 -or (Test-Path $destination))) {
        throw 'ReShade installation proceeded without license acceptance.'
    }
    return $destination
}

function Assert-File([string]$Directory, [string]$Name, [bool]$Expected = $true) {
    if ((Test-Path (Join-Path $Directory $Name)) -ne $Expected) {
        throw "Unexpected file state: $Directory/$Name; expected present=$Expected"
    }
}

$setup = Build-TestInstaller 'Setup'
$hostOnly = Invoke-TestInstaller $setup 'host-only' 'host'
Assert-File $hostOnly 'RobloxShadeHost.exe'
Assert-File $hostOnly 'CREDITS.txt'
Assert-File $hostOnly 'dxgi.dll' $false
Assert-File $hostOnly 'nvngx_dlssnr.dll' $false
Assert-File $hostOnly 'unins000.exe' $false

$null = Invoke-TestInstaller $setup 'no-license' 'host,reshade' $false
$reshade = Invoke-TestInstaller $setup 'reshade' 'host,reshade,reshade\presets'
Assert-File $reshade 'dxgi.dll'
Assert-File $reshade 'ReShade-LICENSE.txt'
Assert-File $reshade 'renodx-dlss.addon64' $false
Assert-File $reshade 'onnxruntime.dll' $false
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
# Left behind by an earlier installer that offered the depth estimation add-on.
Set-Content "$reshade/depth-anything-v2-small.onnx" 'stale'
$null = Invoke-TestInstaller $setup 'reshade' 'host,reshade,reshade\presets'
if ((Get-FileHash "$reshade/ReShade.ini").Hash -ne $originalHash) {
    throw 'Reinstall changed the existing ReShade configuration.'
}
Assert-File $reshade 'depth-anything-v2-small.onnx' $false

# An install from the earlier installer kept ReShade's doubled search paths. Reinstalling repairs
# those lines and nothing else.
$brokenIni = $reshadeIni -replace '(?m)^((?:Effect|Texture)SearchPaths=.*\\\*\*)(?=\r?$)', '$1\**'
$brokenIni += "`n[InstallerTest]`nPreserve=1`n"
[IO.File]::WriteAllText("$reshade/ReShade.ini", $brokenIni.TrimStart([char]0xFEFF), [Text.UTF8Encoding]::new($true))
$null = Invoke-TestInstaller $setup 'reshade' 'host,reshade,reshade\presets'
$repairedIni = Get-Content "$reshade/ReShade.ini" -Raw
Assert-SearchPaths $repairedIni
if ($repairedIni -notmatch '(?m)^Preserve=1' -or ([regex]::Matches($repairedIni, '(?m)^\[GENERAL\]')).Count -ne 1) {
    throw 'Repairing the search paths did not preserve the rest of ReShade.ini.'
}
if ((Get-Content "$reshade/presets/GenericPreset1.ini" -Raw) -notmatch 'Edited') {
    throw 'Reinstall overwrote an edited preset.'
}

$missingSetup = Build-TestInstaller 'Setup-Missing' `
    'https://github.com/OMouta/RobloxShadeHost/releases/download/dlss5-assets/not-present.ini'
$missing = Invoke-TestInstaller $missingSetup 'missing-addons' 'host,reshade,reshade\dlss5'
Assert-File $missing 'RobloxShadeHost.exe'
Assert-File $missing 'dxgi.dll'
Assert-File $missing 'nvngx_dlssnr.dll' $false
Assert-File $missing 'renodx-dlss.addon64' $false
$missingLog = Get-Content "$testRoot/missing-addons.log" -Raw
if ($missingLog -notmatch 'DLSS5 skipped:') {
    throw 'Missing optional downloads were not reported.'
}

if ($DownloadDLSS) {
    $full = Invoke-TestInstaller $setup 'full' 'host,reshade,reshade\dlss5'
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

Write-Output "Installer checks passed. Test files and logs: $testRoot"
