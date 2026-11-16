param([string]$OutputDirectory = [Environment]::GetFolderPath('Desktop'))
$kitCollector = Join-Path (Split-Path $PSScriptRoot -Parent) 'tools\xbox-debug-kit\installer\collect_debug.ps1'
& $kitCollector -DataFolder RedCardRecomp -GameName 'RedCard 20-03' -OutputDirectory $OutputDirectory
