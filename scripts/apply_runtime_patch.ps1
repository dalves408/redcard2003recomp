$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$toolkitRoot = Join-Path $projectRoot 'tools\xboxrecomp'
$patch = Join-Path $PSScriptRoot 'xboxrecomp-runtime.patch'
# git writes "error:" to stderr even for expected check failures; don't let that
# trip Stop. Exit codes are what matter.
$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& git -C $toolkitRoot apply --reverse --check $patch 2>$null | Out-Null
$rev = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($rev -eq 0) { return }
$ErrorActionPreference = 'Continue'
& git -C $toolkitRoot apply --check $patch 2>&1 | Out-Null
$chk = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($chk -ne 0) {
    throw 'Toolkit changes conflict with the pinned RedCard runtime patch; preserve local work before resolving.'
}
$ErrorActionPreference = 'Continue'
& git -C $toolkitRoot apply $patch 2>&1 | Out-Null
$app = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($app -ne 0) { throw 'Could not apply the RedCard runtime patch.' }
