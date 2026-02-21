param(
  [int]$Iterations = 50000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\\build\\debug\\plarbius_train.exe"
if (-not (Test-Path $exe)) {
  throw "Build missing. Run scripts/bootstrap.ps1 first."
}

& $exe $Iterations

