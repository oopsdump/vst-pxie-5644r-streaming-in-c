# Copy build\bin to C:\vst_rf\bin so the GUI and recorders run from the C: volume.
# NI-PAL requires the executable on C:; the GUI refuses to start elsewhere.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Src  = Join-Path $Root 'build\bin'
$Dst  = 'C:\vst_rf\bin'
if (-not (Test-Path $Src)) { throw "missing $Src (run build_all.ps1 first)" }
New-Item -ItemType Directory -Force -Path $Dst | Out-Null
Copy-Item (Join-Path $Src '*.exe') $Dst -Force
Write-Host "[stage] copied to $Dst"
Write-Host "[stage] run: $Dst\vst_gui.exe"