param([switch]$SkipPython)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$dependencies = Get-Content (Join-Path $projectRoot 'dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force (Join-Path $projectRoot 'tools') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $projectRoot 'analysis') | Out-Null
foreach ($name in @('xboxrecomp', 'xbox-debug-kit')) {
    $dependency = $dependencies.$name
    $target = Join-Path $projectRoot "tools\$name"
    if (-not (Test-Path -LiteralPath $target)) {
        & git clone $dependency.url $target
        if ($LASTEXITCODE -ne 0) { throw "Could not clone $name." }
        & git -C $target checkout --detach $dependency.commit
        if ($LASTEXITCODE -ne 0) { throw "Could not check out pinned $name revision." }
    }
    $revision = & git -C $target rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -ne $dependency.commit) {
        throw "$name is not at its pinned revision; the existing checkout was left intact."
    }
}
& (Join-Path $PSScriptRoot 'apply_runtime_patch.ps1')
if (-not $SkipPython) {
    $pythonExe = Join-Path $projectRoot '.venv\Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $pythonExe)) {
        & python -m venv (Join-Path $projectRoot '.venv')
        if ($LASTEXITCODE -ne 0) { throw 'Install Python 3.12+ and make python available on PATH.' }
    }
    & $pythonExe -m pip install --target (Join-Path $projectRoot '.deps') "capstone==$($dependencies.capstone)"
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned Capstone dependency.' }
    & $pythonExe -m pip install "psutil==$($dependencies.psutil)"
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the pinned performance-probe dependency.' }
}
Write-Host 'Pinned runtime dependencies are ready. Extract your own disc and run scripts/pipeline.py --lift next.'
