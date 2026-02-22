param(
  [ValidateSet("debug", "release", "msvc-debug", "msvc-release")]
  [string]$Preset = "msvc-debug",
  [string]$Algorithms = "mccfr",
  [string]$Seeds = "1,2,3",
  [int]$Iterations = 200000,
  [int]$CheckpointEvery = 10000,
  [string]$BucketConfig = "",
  [string]$ActionConfig = "",
  [double]$StackBb = 100.0,
  [double]$SmallBlindBb = 0.5,
  [double]$BigBlindBb = 1.0,
  [double]$MccfrSamplingEpsilon = 0.2,
  [bool]$MccfrLcfrDiscount = $true,
  [int]$MccfrLcfrStart = 5000,
  [int]$MccfrLcfrInterval = 1,
  [bool]$MccfrPruneActions = $false,
  [int]$MccfrPruneStart = 30000,
  [double]$MccfrPruneThreshold = 0.000001,
  [int]$MccfrPruneMinActions = 2,
  [int]$MccfrPruneFullTraversalInterval = 10000,
  [string]$OutputRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $projectRoot "experiments"
}
if ([string]::IsNullOrWhiteSpace($BucketConfig)) {
  $BucketConfig = Join-Path $projectRoot "configs\hunl\bucket_default.cfg"
}
if ([string]::IsNullOrWhiteSpace($ActionConfig)) {
  $ActionConfig = Join-Path $projectRoot "configs\hunl\action_default.cfg"
}
if (-not (Test-Path $BucketConfig)) {
  throw "Bucket config not found: $BucketConfig"
}
if (-not (Test-Path $ActionConfig)) {
  throw "Action config not found: $ActionConfig"
}

$presetExeMap = @{
  "debug"        = "build\debug\plarbius_train_hunl.exe"
  "release"      = "build\release\plarbius_train_hunl.exe"
  "msvc-debug"   = "build\msvc-debug\Debug\plarbius_train_hunl.exe"
  "msvc-release" = "build\msvc-release\Release\plarbius_train_hunl.exe"
}
$exePath = Join-Path $projectRoot $presetExeMap[$Preset]
if (-not (Test-Path $exePath)) {
  throw "Missing executable: $exePath. Build first (cmake --build build\$Preset --target plarbius_train_hunl)."
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $OutputRoot "hunl_$runId"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$summaryPath = Join-Path $runDir "summary.csv"
$allMetricsPath = Join-Path $runDir "all_metrics.csv"
"algorithm,game,seed,iterations,checkpoint_every,final_infosets,final_exploitability,final_nash_conv,metrics_path,policy_path,checkpoint_path,stdout_log,bucket_config,action_config" | Set-Content -Path $summaryPath
"algorithm,game,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,best_response_p1,nash_conv,exploitability,metrics_path,metric_label" | Set-Content -Path $allMetricsPath

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
      "--seed", $seed.ToString(),
      "--bucket-config", $BucketConfig,
      "--action-config", $ActionConfig,
      "--stack-bb", $StackBb.ToString("0.###############"),
      "--small-blind-bb", $SmallBlindBb.ToString("0.###############"),
      "--big-blind-bb", $BigBlindBb.ToString("0.###############"),
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
      if ($MccfrLcfrDiscount) {
        $cliArgs += @(
          "--lcfr-discount",
          "--lcfr-start", $MccfrLcfrStart.ToString(),
          "--lcfr-interval", $MccfrLcfrInterval.ToString()
        )
      }
      if ($MccfrPruneActions) {
        $cliArgs += @(
          "--prune-actions",
          "--prune-start", $MccfrPruneStart.ToString(),
          "--prune-threshold", $MccfrPruneThreshold.ToString("0.###############"),
          "--prune-min-actions", $MccfrPruneMinActions.ToString(),
          "--prune-full-interval", $MccfrPruneFullTraversalInterval.ToString()
        )
      }
    }

    Write-Host "Running HUNL scaffold: algo=$algo seed=$seed iterations=$Iterations"
    & $exePath @cliArgs | Tee-Object -FilePath $stdoutLog | Out-Host
    if ($LASTEXITCODE -ne 0) {
      throw "Run failed for algo=$algo seed=$seed"
    }

    if (-not (Test-Path $metricsPath)) {
      throw "Missing metrics file after run: $metricsPath"
    }
    $metricsRows = Import-Csv -Path $metricsPath
    if ($metricsRows.Count -eq 0) {
      throw "No metrics rows produced: $metricsPath"
    }
    $final = $metricsRows[-1]

    "$algo,hunl,$seed,$Iterations,$CheckpointEvery,$($final.infosets),,,$metricsPath,$policyPath,$checkpointPath,$stdoutLog,$BucketConfig,$ActionConfig" | Add-Content -Path $summaryPath

    foreach ($row in $metricsRows) {
      "$algo,hunl,$seed,$($row.iteration),$($row.infosets),$($row.utility_p0),$($row.utility_p1),$($row.best_response_p0),$($row.best_response_p1),$($row.nash_conv),$($row.exploitability),$metricsPath,infosets" | Add-Content -Path $allMetricsPath
    }
  }
}

Write-Host ""
Write-Host "HUNL scaffold experiment run complete."
Write-Host "Summary: $summaryPath"
Write-Host "All metrics: $allMetricsPath"
