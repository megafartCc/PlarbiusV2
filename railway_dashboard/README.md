# PlarbiusV2 Railway Dashboard

Deployable web dashboard for:

- Learning curves (`exploitability` vs `iteration`) from `all_metrics.csv`
- Run summary table from `summary.csv`
- Final exploitability comparisons
- Pluribus-style running match curve (sampled Kuhn hands, mean + 95% CI)

## Folder Structure

- `server.js`: Express API + static hosting
- `public/index.html`: dashboard UI
- `public/app.js`: charts + API wiring
- `public/styles.css`: styling
- `package.json`: Railway start scripts

## API Endpoints

- `GET /api/health`
- `GET /api/runs`
- `GET /api/runs/:runId/summary`
- `GET /api/runs/:runId/learning`
- `GET /api/runs/:runId/policies`
- `GET /api/runs/:runId/match?policyA=...&policyB=...&hands=10000&seed=1&game=kuhn`

## Local Run

```powershell
cd c:\out\PlarbiusV2\railway_dashboard
npm install
$env:EXPERIMENTS_DIR="c:\out\PlarbiusV2\experiments"
npm start
```

Open `http://localhost:3000`.

## Railway Deploy

1. Set service root to `PlarbiusV2/railway_dashboard`
2. Railway build command: `npm install`
3. Railway start command: `npm start`
4. Set env var:
   - `EXPERIMENTS_DIR=/app/experiments` (or the path where your CSV runs are available)
5. Ensure your experiment folders are present in that path at runtime.

## Notes

- Match simulation endpoint currently supports `game=kuhn` only.
- For full Pluribus-style evaluation (mbb/game with AIVAT), next step is adding a hand-history evaluator and variance reduction module.

