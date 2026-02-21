param(
  [ValidateSet("debug", "release", "msvc-debug", "msvc-release")]
  [string]$Preset = "msvc-debug",
  [ValidateSet("cfr+", "mccfr")]
  [string]$Algorithm = "mccfr",
  [ValidateSet("kuhn", "leduc")]
  [string]$Game = "leduc",
  [int]$Iterations = 20000,
  [int]$LogInterval = 2000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$presetExeMap = @{
  "debug"       = "build\debug\plarbius_train.exe"
  "release"     = "build\release\plarbius_train.exe"
  "msvc-debug"  = "build\msvc-debug\Debug\plarbius_train.exe"
  "msvc-release"= "build\msvc-release\Release\plarbius_train.exe"
}
$exePath = Join-Path $projectRoot $presetExeMap[$Preset]
if (-not (Test-Path $exePath)) {
  throw "Missing executable: $exePath. Build first (scripts/bootstrap.ps1 -Preset $Preset)."
}

$cliArgs = @(
  $Iterations.ToString(),
  "--algo", $Algorithm,
  "--game", $Game,
  "--log-interval", $LogInterval.ToString(),
  "--no-strategy-print"
)
if ($Algorithm -eq "mccfr") {
  $cliArgs += @("--sampling-epsilon", "0.2")
}

Write-Host "Profiling: $exePath $($cliArgs -join ' ')"
$timing = Measure-Command {
  & $exePath @cliArgs | Out-Host
  if ($LASTEXITCODE -ne 0) {
    throw "Training run failed."
  }
}
Write-Host ("Wall time: {0:n3}s" -f $timing.TotalSeconds)
