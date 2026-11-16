param([ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$cmakeCommand=Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe=if($cmakeCommand){$cmakeCommand.Source}else{$null}
if(-not $cmakeExe){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakeExe=Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if(-not (Test-Path -LiteralPath $cmakeExe)){throw 'CMake and Visual Studio C++ build tools are required.'}
$buildDir=Join-Path $projectRoot 'build'
& $cmakeExe -S $projectRoot -B $buildDir
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed.'}
& $cmakeExe --build $buildDir --config $Configuration --parallel 2 --target redcard_checks
if($LASTEXITCODE -ne 0){throw 'Regression build failed.'}
$ctestExe=Join-Path (Split-Path $cmakeExe -Parent) 'ctest.exe'
& $ctestExe --test-dir $buildDir -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Regression checks failed.'}
