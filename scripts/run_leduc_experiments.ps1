param(
  [ValidateSet("debug", "release", "msvc-debug", "msvc-release")]
  [string]$Preset = "msvc-debug",
  [string]$Algorithms = "mccfr",
  [string]$Seeds = "1,2,3",
  [int]$Iterations = 300000,
  [int]$CheckpointEvery = 10000,
  [double]$MccfrSamplingEpsilon = 0.2,
  [bool]$MccfrLcfrDiscount = $true,
  [int]$MccfrLcfrStart = 5000,
  [int]$MccfrLcfrInterval = 1,
  [bool]$MccfrPruneActions = $false,
  [int]$MccfrPruneStart = 30000,
  [double]$MccfrPruneThreshold = 0.000001,
  [int]$MccfrPruneMinActions = 2,
  [int]$MccfrPruneFullTraversalInterval = 5000,
  [int]$PairwiseHands = 100000,
  [int]$PairwiseBaseSeed = 101,
  [switch]$SkipPairwise,
  [string]$OutputRoot = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $projectRoot "experiments"
}

$presetTrainExeMap = @{
  "debug"        = "build\debug\plarbius_train.exe"
  "release"      = "build\release\plarbius_train.exe"
  "msvc-debug"   = "build\msvc-debug\Debug\plarbius_train.exe"
  "msvc-release" = "build\msvc-release\Release\plarbius_train.exe"
}
$trainExePath = Join-Path $projectRoot $presetTrainExeMap[$Preset]
if (-not (Test-Path $trainExePath)) {
  throw "Missing executable: $trainExePath. Build first (scripts/bootstrap.ps1 -Preset $Preset)."
}

$presetSelfplayExeMap = @{
  "debug"        = "build\debug\plarbius_selfplay.exe"
  "release"      = "build\release\plarbius_selfplay.exe"
  "msvc-debug"   = "build\msvc-debug\Debug\plarbius_selfplay.exe"
  "msvc-release" = "build\msvc-release\Release\plarbius_selfplay.exe"
}
$selfplayExePath = Join-Path $projectRoot $presetSelfplayExeMap[$Preset]
if (-not $SkipPairwise -and -not (Test-Path $selfplayExePath)) {
  throw "Missing executable: $selfplayExePath. Build first (scripts/bootstrap.ps1 -Preset $Preset)."
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $OutputRoot "leduc_$runId"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$summaryPath = Join-Path $runDir "summary.csv"
$allMetricsPath = Join-Path $runDir "all_metrics.csv"
$pairwisePath = Join-Path $runDir "pairwise_matrix.csv"
$pairwiseScoresPath = Join-Path $runDir "pairwise_policy_scores.csv"
$bestPolicyPath = Join-Path $runDir "leduc_blueprint_best.policy.tsv"

"algorithm,game,seed,iterations,checkpoint_every,final_exploitability,final_nash_conv,final_utility_p0,final_ci95,pairwise_score,is_champion,metrics_path,policy_path,checkpoint_path,stdout_log,pairwise_matrix_path,best_policy_path" | Set-Content -Path $summaryPath
"algorithm,game,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,best_response_p1,nash_conv,exploitability,metrics_path,metric_label" | Set-Content -Path $allMetricsPath
"policy_a,algorithm_a,seed_a,policy_b,algorithm_b,seed_b,sampled_hands,sample_seed,utility_p0,sampled_utility_mean_p0,sampled_ci95,hand_history_path,stdout_log" | Set-Content -Path $pairwisePath

$algoList = $Algorithms.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
$seedList = $Seeds.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }

$trainingRows = @()
$policyRows = @()

foreach ($algo in $algoList) {
  foreach ($seedText in $seedList) {
    $seed = [int]$seedText
    $safeAlgo = ($algo -replace "[^a-zA-Z0-9_-]", "_")
    $prefix = "${safeAlgo}_seed${seed}"
    $metricsPath = Join-Path $runDir "${prefix}.metrics.csv"
    $policyPath = Join-Path $runDir "${prefix}.policy.tsv"
    $checkpointPath = Join-Path $runDir "${prefix}.ckpt.tsv"
    $stdoutLog = Join-Path $runDir "${prefix}.stdout.log"

    "algorithm,game,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,best_response_p1,nash_conv,exploitability,metrics_path,metric_label" | Set-Content -Path $metricsPath

    $cliArgs = @(
      $Iterations.ToString(),
      "--algo", $algo,
      "--game", "leduc",
      "--seed", $seed.ToString(),
      "--checkpoint", $checkpointPath,
      "--checkpoint-every", $CheckpointEvery.ToString(),
      "--policy-out", $policyPath,
      "--log-interval", $CheckpointEvery.ToString(),
      "--no-strategy-print"
    )

    if ($algo -eq "mccfr") {
      $cliArgs += @("--sampling-epsilon", $MccfrSamplingEpsilon.ToString("0.###############"))
      if ($MccfrLcfrDiscount) {
        $cliArgs += @("--lcfr-discount", "--lcfr-start", $MccfrLcfrStart.ToString(), "--lcfr-interval", $MccfrLcfrInterval.ToString())
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

    Write-Host "Running Leduc: algo=$algo seed=$seed iterations=$Iterations"
    & $trainExePath @cliArgs | Tee-Object -FilePath $stdoutLog | Out-Host
    if ($LASTEXITCODE -ne 0) {
      throw "Leduc run failed for algo=$algo seed=$seed"
    }

    if (-not (Test-Path $policyPath)) {
      throw "Missing policy file after run: $policyPath"
    }

    $logLines = Get-Content -Path $stdoutLog
    foreach ($line in $logLines) {
      $match = [regex]::Match($line, "Iteration\s+(\d+),\s*infosets=(\d+)")
      if (-not $match.Success) {
        continue
      }
      $iter = $match.Groups[1].Value
      $infosets = $match.Groups[2].Value
      "$algo,leduc,$seed,$iter,$infosets,,,,,,,$metricsPath,infosets" | Add-Content -Path $metricsPath
      "$algo,leduc,$seed,$iter,$infosets,,,,,,,$metricsPath,infosets" | Add-Content -Path $allMetricsPath
    }

    $trainingRows += [PSCustomObject]@{
      algorithm = $algo
      seed = $seed
      iterations = $Iterations
      checkpoint_every = $CheckpointEvery
      metrics_path = $metricsPath
      policy_path = $policyPath
      checkpoint_path = $checkpointPath
      stdout_log = $stdoutLog
      final_utility_p0 = $null
      final_ci95 = $null
      pairwise_score = $null
      is_champion = $false
    }

    $policyRows += [PSCustomObject]@{
      algorithm = $algo
      seed = $seed
      policy_path = $policyPath
      policy_file = (Split-Path $policyPath -Leaf)
      score_sum = 0.0
      score_count = 0
      pairwise_score = 0.0
    }
  }
}

$pairCounter = 0
if (-not $SkipPairwise -and $policyRows.Count -ge 2) {
  for ($i = 0; $i -lt $policyRows.Count; $i += 1) {
    for ($j = $i + 1; $j -lt $policyRows.Count; $j += 1) {
      $rowA = $policyRows[$i]
      $rowB = $policyRows[$j]
      $pairSeed = $PairwiseBaseSeed + $pairCounter
      $pairTag = "a${i}_seed$($rowA.seed)_vs_b${j}_seed$($rowB.seed)"
      $historyPath = Join-Path $runDir ("hand_history_${pairTag}.csv")
      $stdoutPath = Join-Path $runDir ("pairwise_${pairTag}.stdout.log")

      $selfplayArgs = @(
        "--game", "leduc",
        "--policy-a", $rowA.policy_path,
        "--policy-b", $rowB.policy_path,
        "--sampled-hands", $PairwiseHands.ToString(),
        "--sample-seed", $pairSeed.ToString(),
        "--hand-history-out", $historyPath
      )

      Write-Host "Pairwise: $($rowA.policy_file) vs $($rowB.policy_file)"
      $selfplayOutput = & $selfplayExePath @selfplayArgs
      $selfplayOutput | Tee-Object -FilePath $stdoutPath | Out-Host
      if ($LASTEXITCODE -ne 0) {
        throw "Pairwise selfplay failed for $($rowA.policy_file) vs $($rowB.policy_file)"
      }

      $mean = $null
      $ci95 = $null
      $utility = $null
      foreach ($outLine in $selfplayOutput) {
        if ($outLine -match "^utility_p0=(.+)$") {
          $utility = [double]$Matches[1]
        }
        if ($outLine -match "^sampled_utility_mean_p0=(.+)$") {
          $mean = [double]$Matches[1]
        }
        if ($outLine -match "^sampled_ci95=(.+)$") {
          $ci95 = [double]$Matches[1]
        }
      }
      if ($null -eq $mean) {
        throw "Failed to parse sampled_utility_mean_p0 for pairwise run: $stdoutPath"
      }
      if ($null -eq $ci95) {
        $ci95 = 0.0
      }
      if ($null -eq $utility) {
        $utility = $mean
      }

      "$($rowA.policy_file),$($rowA.algorithm),$($rowA.seed),$($rowB.policy_file),$($rowB.algorithm),$($rowB.seed),$PairwiseHands,$pairSeed,$utility,$mean,$ci95,$historyPath,$stdoutPath" | Add-Content -Path $pairwisePath

      $policyRows[$i].score_sum = [double]$policyRows[$i].score_sum + $mean
      $policyRows[$i].score_count = [int]$policyRows[$i].score_count + 1
      $policyRows[$j].score_sum = [double]$policyRows[$j].score_sum - $mean
      $policyRows[$j].score_count = [int]$policyRows[$j].score_count + 1

      $pairCounter += 1
    }
  }
}

foreach ($entry in $policyRows) {
  if ($entry.score_count -gt 0) {
    $entry.pairwise_score = [double]$entry.score_sum / [double]$entry.score_count
  } else {
    $entry.pairwise_score = 0.0
  }
}

$champion = $null
if ($policyRows.Count -gt 0) {
  $champion = $policyRows | Sort-Object -Property pairwise_score -Descending | Select-Object -First 1
  Copy-Item -Path $champion.policy_path -Destination $bestPolicyPath -Force
}

"policy_file,algorithm,seed,pairwise_score,score_count,is_champion" | Set-Content -Path $pairwiseScoresPath
foreach ($entry in $policyRows) {
  $isChampion = $false
  if ($null -ne $champion -and $entry.policy_file -eq $champion.policy_file) {
    $isChampion = $true
  }
  "$($entry.policy_file),$($entry.algorithm),$($entry.seed),$($entry.pairwise_score),$($entry.score_count),$isChampion" | Add-Content -Path $pairwiseScoresPath
}

$policyScoreByFile = @{}
foreach ($entry in $policyRows) {
  $policyScoreByFile[$entry.policy_file] = $entry
}

foreach ($row in $trainingRows) {
  $policyFile = Split-Path $row.policy_path -Leaf
  $scoreEntry = $null
  if ($policyScoreByFile.ContainsKey($policyFile)) {
    $scoreEntry = $policyScoreByFile[$policyFile]
  }

  $pairwiseScore = ""
  $finalUtilityP0 = ""
  $isChampionText = "false"
  if ($null -ne $scoreEntry) {
    $pairwiseScore = $scoreEntry.pairwise_score
    $finalUtilityP0 = $scoreEntry.pairwise_score
    if ($null -ne $champion -and $scoreEntry.policy_file -eq $champion.policy_file) {
      $isChampionText = "true"
    }
  }

  "$($row.algorithm),leduc,$($row.seed),$($row.iterations),$($row.checkpoint_every),,,$finalUtilityP0,,$pairwiseScore,$isChampionText,$($row.metrics_path),$($row.policy_path),$($row.checkpoint_path),$($row.stdout_log),$pairwisePath,$bestPolicyPath" | Add-Content -Path $summaryPath
}

Write-Host ""
Write-Host "Leduc experiment run complete."
Write-Host "Summary: $summaryPath"
Write-Host "All metrics: $allMetricsPath"
Write-Host "Pairwise matrix: $pairwisePath"
Write-Host "Pairwise scores: $pairwiseScoresPath"
if ($null -ne $champion) {
  Write-Host "Champion policy: $($champion.policy_file)"
  Write-Host "Best policy copy: $bestPolicyPath"
}
