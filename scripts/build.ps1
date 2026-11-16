param(
    [ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Debug',
    [switch]$HostFmv,
    # Ship/Release installers pass -NoDebugKit so XRDBG, FPS HUD, and forced /DEBUG stay off.
    # RelWithDebInfo/Debug keep the kit on unless -NoDebugKit is set.
    [switch]$NoDebugKit
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakeExe) {
    $vswhereExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhereExe) {
        $vsRoot = & $vswhereExe -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsRoot) {
            $bundledCMake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $bundledCMake) { $cmakeExe = $bundledCMake }
        }
    }
}
if (-not $cmakeExe) {
    throw 'Install CMake 3.20+ and Visual Studio 2022 Desktop development with C++, including a Windows SDK; then use a Developer PowerShell.'
}
if (-not (Test-Path (Join-Path $projectRoot 'src/recomp/gen/recomp_types.h'))) {
    throw 'Generate the C sources first: python scripts/pipeline.py --lift'
}
& (Join-Path $PSScriptRoot 'apply_runtime_patch.ps1')
$buildDir = Join-Path $projectRoot 'build'
$platformArgs = @('-A', 'x64')
$cachePath = Join-Path $buildDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath) {
    $platformLine = Select-String -Path $cachePath -Pattern '^CMAKE_GENERATOR_PLATFORM:INTERNAL=' |
        Select-Object -First 1
    if ($platformLine) {
        $cachedPlatform = $platformLine.Line.Substring(
            $platformLine.Line.IndexOf('=') + 1
        )
        if ([string]::IsNullOrWhiteSpace($cachedPlatform)) {
            $platformArgs = @()
        } else {
            $platformArgs = @('-A', $cachedPlatform)
        }
    }
}
$cmakeArgs = @(
    '-S', $projectRoot,
    '-B', $buildDir,
    '-G', 'Visual Studio 17 2022'
)
$cmakeArgs += $platformArgs
$debugKit = if ($NoDebugKit -or $Configuration -eq 'Release') { 'OFF' } else { 'ON' }
$cmakeArgs += @(
    '-DXBOXRECOMP_ENABLE_HOST_FMV=OFF',
    '-DVCPKG_MANIFEST_FEATURES=',
    "-DREDCARD_DEBUGKIT=$debugKit"
)
if ($HostFmv) {
    $vcpkgRoot = $env:VCPKG_ROOT
    if (-not $vcpkgRoot) {
        throw 'Set VCPKG_ROOT to a space-free vcpkg installation before building with -HostFmv.'
    }
    if ($vcpkgRoot.Contains(' ')) {
        throw 'VCPKG_ROOT must not contain spaces because the FFmpeg port rejects spaced source paths.'
    }
    $vcpkgToolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $vcpkgToolchain)) {
        throw 'VCPKG_ROOT does not contain scripts\buildsystems\vcpkg.cmake.'
    }
    $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
    $cmakeArgs += "-DVCPKG_INSTALLED_DIR=$(Join-Path $vcpkgRoot 'installed')"
    $cmakeArgs += '-DVCPKG_TARGET_TRIPLET=x64-windows'
    $cmakeArgs += '-DVCPKG_MANIFEST_FEATURES=host-fmv'
    $cmakeArgs += '-DXBOXRECOMP_ENABLE_HOST_FMV=ON'
} else {
    $cmakeArgs += '-DCMAKE_TOOLCHAIN_FILE='
}
& $cmakeExe @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmakeExe --build $buildDir --config $Configuration --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed; inspect the compiler diagnostics.' }
Write-Host 'Run the executable from the project root so game_files paths resolve.'
