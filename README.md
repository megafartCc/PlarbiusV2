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
  --checkpoint-every 5000 --policy-out data\leduc_mccfr.policy.tsv --no-strategy-print
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
- Kuhn policy-vs-policy running match curve with confidence interval

Run locally:

```powershell
cd c:\out\PlarbiusV2\railway_dashboard
npm install
$env:EXPERIMENTS_DIR="c:\out\PlarbiusV2\experiments"
npm start
```

## Immediate Next Build Targets

1. Add Leduc exploitability / best-response evaluator.
2. Add richer abstraction layers for larger game trees.
3. Add distributed trainer workers and checkpoint sharding.
4. Add online depth-limited resolving stage.
