param(
  [ValidateSet("debug", "release", "msvc-debug", "msvc-release")]
  [string]$Preset = "msvc-debug",
  [string]$Algorithms = "cfr+,mccfr",
  [string]$Seeds = "1,2,3",
  [int]$Iterations = 50000,
  [int]$CheckpointEvery = 5000,
  [double]$MccfrSamplingEpsilon = 0.3,
  [string]$OutputRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $projectRoot "experiments"
}

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

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $OutputRoot "kuhn_$runId"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$summaryPath = Join-Path $runDir "summary.csv"
$allMetricsPath = Join-Path $runDir "all_metrics.csv"
"algorithm,seed,iterations,checkpoint_every,final_exploitability,final_nash_conv,metrics_path,policy_path,checkpoint_path,stdout_log" | Set-Content -Path $summaryPath
"algorithm,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,best_response_p1,nash_conv,exploitability,metrics_path" | Set-Content -Path $allMetricsPath

$algoList = $Algorithms.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
$seedList = $Seeds.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }

foreach ($algo in $algoList) {
  foreach ($seedText in $seedList) {
    $seed = [int]$seedText
    $safeAlgo = ($algo -replace "[^a-zA-Z0-9_-]", "_")
    $prefix = "${safeAlgo}_seed${seed}"
    $metricsPath = Join-Path $runDir "${prefix}.metrics.csv"
    $policyPath = Join-Path $runDir "${prefix}.policy.tsv"
    $checkpointPath = Join-Path $runDir "${prefix}.ckpt.tsv"
    $stdoutLog = Join-Path $runDir "${prefix}.stdout.log"

    $cliArgs = @(
      $Iterations.ToString(),
      "--algo", $algo,
      "--game", "kuhn",
      "--seed", $seed.ToString(),
      "--checkpoint", $checkpointPath,
      "--checkpoint-every", $CheckpointEvery.ToString(),
      "--metrics-out", $metricsPath,
      "--metrics-interval", $CheckpointEvery.ToString(),
      "--policy-out", $policyPath,
      "--log-interval", $CheckpointEvery.ToString(),
      "--no-strategy-print"
    )

    if ($algo -eq "mccfr") {
      $cliArgs += @("--sampling-epsilon", $MccfrSamplingEpsilon.ToString("0.###############"))
    }

    Write-Host "Running: algo=$algo seed=$seed iterations=$Iterations"
    & $exePath @cliArgs | Tee-Object -FilePath $stdoutLog | Out-Host
    if ($LASTEXITCODE -ne 0) {
      throw "Run failed for algo=$algo seed=$seed"
    }

    $metricsRows = Import-Csv -Path $metricsPath
    if ($metricsRows.Count -eq 0) {
      throw "No metrics rows produced: $metricsPath"
    }
    $final = $metricsRows[-1]
    "$algo,$seed,$Iterations,$CheckpointEvery,$($final.exploitability),$($final.nash_conv),$metricsPath,$policyPath,$checkpointPath,$stdoutLog" | Add-Content -Path $summaryPath

    foreach ($row in $metricsRows) {
      "$algo,$seed,$($row.iteration),$($row.infosets),$($row.utility_p0),$($row.utility_p1),$($row.best_response_p0),$($row.best_response_p1),$($row.nash_conv),$($row.exploitability),$metricsPath" | Add-Content -Path $allMetricsPath
    }
  }
}

Write-Host ""
Write-Host "Experiment run complete."
Write-Host "Summary: $summaryPath"
Write-Host "All metrics: $allMetricsPath"
