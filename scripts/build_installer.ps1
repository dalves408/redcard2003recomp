# Build Release game + redcard_import, then compile the Inno Setup installer.
# Usage:
#   .\scripts\build_installer.ps1
#   .\scripts\build_installer.ps1 -AppVersion 1.0.0 -OutSuffix -test5
# Defaults to Release with REDCARD_DEBUGKIT=OFF (no FPS HUD / xrdbg / forced PDB).
# Pass -Configuration RelWithDebInfo and/or -DebugBuild only for diagnostic packages.
param(
    [ValidateSet('Release','RelWithDebInfo','Debug')][string]$Configuration = 'Release',
    [string]$AppVersion = '1.0.0',
    [string]$OutSuffix = '',
    [switch]$SkipGameBuild,
    [switch]$DebugBuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root

$Iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $Iscc)) {
    throw "Inno Setup 6 ISCC.exe not found at $Iscc. Install from https://jrsoftware.org/isinfo.php"
}

if (-not $SkipGameBuild) {
    # build.ps1 turns REDCARD_DEBUGKIT off for Release automatically.
    & "$PSScriptRoot\build.ps1" -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'Game build failed' }
}

$BuildDir = Join-Path $Root "build\$Configuration"
$ImportSrc = Join-Path $Root 'installer\redcard_import.c'
$ImportExe = Join-Path $BuildDir 'redcard_import.exe'
$Manifest = Join-Path $Root 'installer\manifest.h'
if (-not (Test-Path $Manifest)) {
    & "$Root\.venv\Scripts\python.exe" (Join-Path $Root 'installer\gen_manifest.py') (Join-Path $Root 'game_files')
}

# Compile import tool with the same VS env as build.ps1 uses via vswhere / cl
$Vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { throw 'Visual Studio C++ tools not found' }
$Aux = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$Cmd = "`"$Aux`" && cl /nologo /O2 /W3 /DUNICODE /D_UNICODE `"$ImportSrc`" /Fe:`"$ImportExe`" /I`"$Root\installer`" bcrypt.lib ole32.lib shell32.lib advapi32.lib"
cmd /c $Cmd
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ImportExe)) { throw 'redcard_import.exe build failed' }

# Hi-res texture download helper (Setup extracts it to {tmp} when the texture task is ticked)
$DlSrc = Join-Path $Root 'installer\texture_download.c'
$DlExe = Join-Path $BuildDir 'redcard_texture_download.exe'
$DlObj = Join-Path $BuildDir 'texture_download.obj'
$Cmd = "`"$Aux`" && cl /nologo /O2 /W3 /MT /DUNICODE /D_UNICODE `"$DlSrc`" /Fo:`"$DlObj`" /Fe:`"$DlExe`" winhttp.lib bcrypt.lib shell32.lib"
cmd /c $Cmd
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $DlExe)) { throw 'redcard_texture_download.exe build failed' }

# recomplauncher.json (installed with the game) and the RecompLauncher release ZIP
& "$Root\.venv\Scripts\python.exe" (Join-Path $Root 'scripts\recomplauncher_manifest.py') --version $AppVersion "--suffix=$OutSuffix" --build $BuildDir --out (Join-Path $Root 'installer\Output')
if ($LASTEXITCODE -ne 0) { throw 'recomplauncher_manifest.py failed' }

$Iss = Join-Path $Root 'installer\redcard_2003.iss'
$Defs = @(
    "/DAppVersion=$AppVersion",
    "/DBuildDir=$BuildDir",
    "/DOutSuffix=$OutSuffix"
)
if ($DebugBuild) { $Defs += '/DDebugBuild=1' }

& $Iscc @Defs $Iss
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }

$Out = Join-Path $Root "installer\Output\RedCard2003-Setup-$AppVersion$OutSuffix.exe"
Write-Host "Installer: $Out"
Get-Item $Out | Format-List FullName, Length, LastWriteTime

# Standalone hi-res texture add-on (uses texture.bin beside it, or downloads it)
& $Iscc "/DBuildDir=$BuildDir" "/DOutSuffix=$OutSuffix" (Join-Path $Root 'installer\redcard_textures_addon.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed (texture add-on)' }
Get-ChildItem (Join-Path $Root 'installer\Output') -Filter "RedCard2003-HiRes-Textures-*-Setup$OutSuffix.exe" |
    Format-List FullName, Length, LastWriteTime
