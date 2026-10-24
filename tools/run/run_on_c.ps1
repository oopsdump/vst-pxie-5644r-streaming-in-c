# Run an exe from the C: volume.
#
# Why: when the exe itself resides on D:, NI-PAL's DllMain fails and every NI session
# returns 0xBFFA0000 ("Unrecoverable Failure"). See
# docs\analysis\ni-pal-executable-volume.md. Working directory and data files are not
# affected, so data can stay on D:.
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses BOM-less .ps1 as ANSI.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$ExeArgs = @()
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }
$Source = (Resolve-Path $Exe).Path

$StageDir = Join-Path $env:LOCALAPPDATA 'vst_rf\run'
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

$Staged = Join-Path $StageDir (Split-Path $Source -Leaf)
Copy-Item $Source $Staged -Force

Write-Host "[run] staged to $Staged"

# Native stderr must not become a terminating error, and $LASTEXITCODE must still report
# the tool's real code. Funnelling both streams through ForEach-Object keeps warnings
# visible as plain text instead of NativeCommandError records.
$savedEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
Push-Location $StageDir
try {
    & $Staged @ExeArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
} finally {
    Pop-Location
    $ErrorActionPreference = $savedEap
}
exit $code
