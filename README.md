# PlarbiusV2

Standalone C++20 project for a scalable heads-up poker research stack focused on CFR-family training.

## Goals

- Start with a clean architecture for 1v1 extensive-form games.
- Provide a reliable CFR+ baseline on Kuhn poker.
- Provide external-sampling MCCFR with deterministic seeding.
- Support checkpoint/resume for long offline runs.
- Export/load average policies for offline selfplay evaluation.
- Support Kuhn and Leduc under the same game interface.
- Keep modules easy to swap when moving to MCCFR, abstraction, and larger games.

## Structure

- `include/plarbius/`: public interfaces and domain models
- `src/`: implementations
- `apps/`: runnable binaries (`plarbius_train`, `plarbius_selfplay`)
- `tests/`: unit and smoke tests
- `configs/`: training presets
- `docs/`: architecture and roadmap
- `scripts/`: helper scripts for local runs

## Quick Start

```powershell
cd c:\out\PlarbiusV2
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
.\build\debug\plarbius_train.exe 50000
```

If `Ninja` is not installed, use the MSVC presets instead:

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
.\build\msvc-debug\Debug\plarbius_train.exe 50000 --algo cfr+ --game kuhn
```

Select algorithm and deterministic run options:

```powershell
.\build\msvc-debug\Debug\plarbius_train.exe 200000 --algo mccfr --seed 42 `
  --game kuhn --sampling-epsilon 0.3 `
  --lcfr-discount --lcfr-start 2000 --lcfr-interval 1 `
  --prune-actions --prune-start 20000 --prune-threshold 0.000001 `
  --prune-min-actions 2 --prune-full-interval 5000 `
  --checkpoint data\kuhn_mccfr.ckpt.tsv --checkpoint-every 10000 `
  --metrics-out data\kuhn_mccfr_metrics.csv --metrics-interval 10000 `
  --policy-out data\kuhn_mccfr.policy.tsv --no-strategy-print
```

Resume from a checkpoint:

```powershell
.\build\msvc-debug\Debug\plarbius_train.exe 200000 --algo cfr+ --resume data\kuhn_mccfr.ckpt.tsv `
  --game kuhn --checkpoint data\kuhn_cfr_plus.ckpt.tsv --checkpoint-every 25000 `
  --policy-out data\kuhn_cfr_plus.policy.tsv
```

Train on Leduc:

```powershell
.\build\msvc-debug\Debug\plarbius_train.exe 50000 --algo mccfr --game leduc `
  --seed 7 --sampling-epsilon 0.2 --checkpoint data\leduc_mccfr.ckpt.tsv `
  --checkpoint-every 5000 --metrics-out data\leduc_mccfr.metrics.csv --metrics-interval 5000 `
  --policy-out data\leduc_mccfr.policy.tsv --no-strategy-print
```

Train on HUNL scaffold abstraction (separate entrypoint):

```powershell
.\build\msvc-debug\Debug\plarbius_train_hunl.exe 200000 --algo mccfr --seed 7 `
  --bucket-config configs\hunl\bucket_default.cfg `
  --action-config configs\hunl\action_default.cfg `
  --sampling-epsilon 0.2 --lcfr-discount --lcfr-start 5000 `
  --checkpoint data\hunl_mccfr.ckpt.tsv --checkpoint-every 10000 `
  --metrics-out data\hunl_mccfr.metrics.csv --metrics-interval 10000 `
  --policy-out data\hunl_mccfr.policy.tsv --no-strategy-print
```

This HUNL module is an abstraction scaffold for architecture and pipeline development.
Exploitability reporting for HUNL is not implemented yet.

Run multi-seed HUNL scaffold experiments:

```powershell
.\scripts\run_hunl_experiments.ps1 -Preset msvc-debug -Algorithms mccfr `
  -Seeds 1,2,3 -Iterations 300000 -CheckpointEvery 10000 `
  -BucketConfig configs\hunl\bucket_default.cfg -ActionConfig configs\hunl\action_default.cfg
```

Run controlled Leduc experiments with pairwise scoring + champion policy:

```powershell
.\scripts\run_leduc_experiments.ps1 -Preset msvc-debug -Algorithms mccfr `
  -Seeds 1,2,3 -Iterations 300000 -CheckpointEvery 5000 -PairwiseHands 100000
```

Evaluate saved policies with expected-value selfplay (parallel at root chance):

```powershell
.\build\msvc-debug\Debug\plarbius_selfplay.exe --game kuhn `
  --policy-a data\kuhn_cfr_plus.policy.tsv --policy-b data\kuhn_mccfr.policy.tsv --threads 4
```

Run controlled Kuhn experiments over multiple seeds:

```powershell
.\scripts\run_kuhn_experiments.ps1 -Preset msvc-debug -Algorithms cfr+,mccfr `
  -Seeds 1,2,3 -Iterations 100000 -CheckpointEvery 5000
```

This script now also generates per-seed sampled hand-history CSV files (`hand_history_seed*_cfr_vs_mccfr.csv`)
for dashboard `mbb/game` + AIVAT-style plots. Control it with:

- `-HandHistoryHands 20000` (default)
- `-HandHistoryBaseSeed 1`
- `-SkipHandHistory` to disable
- `-MccfrLcfrDiscount -MccfrLcfrStart 2000 -MccfrLcfrInterval 1`
- `-MccfrPruneActions -MccfrPruneStart 20000 -MccfrPruneThreshold 0.000001`
- `-MccfrPruneMinActions 2 -MccfrPruneFullTraversalInterval 5000`

Quick profiling run:

```powershell
.\scripts\profile_training.ps1 -Preset msvc-debug -Algorithm mccfr -Game leduc -Iterations 20000
```

## Railway Dashboard

Deployable dashboard folder:

- `railway_dashboard/`

It includes:

- Frontend charts (`index.html`) for learning curves and final exploitability
- Backend API (`server.js`) that reads `experiments/*/summary.csv` and `all_metrics.csv`
- Leduc pairwise matrix + champion policy metadata
- Kuhn policy-vs-policy running match curve with confidence interval
- Hand-history `mbb/game` charting with AIVAT-style variance reduction

Run locally:

```powershell
cd c:\out\PlarbiusV2\railway_dashboard
npm install
$env:EXPERIMENTS_DIR="c:\out\PlarbiusV2\experiments"
npm start
```

## Immediate Next Build Targets

1. Add configurable action/card abstraction for larger game trees.
2. Add reproducible benchmark sweeps (seed matrix + auto summary comparisons).
3. Add distributed trainer workers and checkpoint sharding.
4. Add online depth-limited resolving stage.
