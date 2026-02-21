# PlarbiusV2 Railway Dashboard

Deployable web dashboard for:

- Learning curves (`exploitability` vs `iteration`) from `all_metrics.csv`
- Run summary table from `summary.csv`
- Final exploitability comparisons
- Pluribus-style running match curve (sampled Kuhn hands, mean + 95% CI)
- Hand-history `mbb/game` charting (raw and AIVAT-style variance-reduced)
- Auto-refresh when new run artifacts appear (scheduled backend refresh + SSE updates)

## Folder Structure

- `server.js`: Express API + static hosting
- `public/index.html`: dashboard UI
- `public/app.js`: charts + API wiring
- `public/styles.css`: styling
- `package.json`: Railway start scripts

## API Endpoints

- `GET /api/health`
- `GET /api/refresh-status`
- `GET /api/refresh-now`
- `GET /api/refresh-stream` (SSE)
- `GET /api/runs`
- `GET /api/runs/:runId/summary`
- `GET /api/runs/:runId/metrics`
- `GET /api/runs/:runId/learning`
- `GET /api/runs/:runId/policies`
- `GET /api/runs/:runId/hand-histories`
- `GET /api/runs/:runId/mbb?handHistory=<relative_path>&method=both|raw|aivat_like&maxPoints=500`
- `GET /api/runs/:runId/match?policyA=...&policyB=...&hands=10000&seed=1&game=kuhn`

## Hand-History Schema

Supported file types:

- `.jsonl` (one JSON object per line)
- `.csv` (header row + data rows)

The ingestion is tolerant to multiple field names. Minimum requirement is a usable P0 delta in bb, directly or indirectly:

- Preferred: `delta_p0_bb`
- Alternatives supported:
  - `p0_delta_bb`, `player0_delta_bb`, `utility_p0_bb`, `result_p0_bb`, `profit_p0_bb`
  - P1 equivalents (`delta_p1_bb`, etc.) are converted with sign flip
  - Chip deltas (`delta_p0_chips`, etc.) + `big_blind`/`bb` are converted to bb

Optional context fields improve AIVAT-style variance reduction by grouping similar states:

- `street`, `round`, `position`, `seat`, `pot_bucket`, `board_bucket`, `action_bucket`, `state_key`, `public_state`

Example JSONL row:

```json
{"hand_id": 1287, "delta_p0_bb": -1.5, "street": "turn", "position": "sb", "state_key": "r1:c-b-c"}
```

## Local Run

```powershell
cd c:\out\PlarbiusV2\railway_dashboard
npm install
$env:EXPERIMENTS_DIR="c:\out\PlarbiusV2\experiments"
$env:REFRESH_INTERVAL_MS="30000"
$env:ALLOWED_ORIGINS="*"
npm start
```

Open `http://localhost:3000`.

## Railway Deploy

1. Set service root to `PlarbiusV2/railway_dashboard`
2. Railway build command: `npm install`
3. Railway start command: `npm start`
4. Set env var:
   - `EXPERIMENTS_DIR=/app/experiments` (or the path where your CSV runs are available)
   - `REFRESH_INTERVAL_MS=30000` (optional, minimum 3000 ms)
   - `ALLOWED_ORIGINS=https://<your-frontend-domain>` (comma-separated list or `*`)
5. Ensure your experiment folders are present in that path at runtime.

## Split Deploy (Railway Frontend + ngrok Backend)

If backend runs on your PC via ngrok, frontend must call that absolute API host.

Frontend API base resolution order:

1. Query param `?apiBaseUrl=https://<backend-host>` (also saved to `localStorage`)
2. `localStorage["plarbius_api_base_url"]`
3. `<meta name="plarbius-api-base-url" content="...">`
4. `window.PLARBIUS_API_BASE_URL`
5. Same-origin fallback (default)

Example:

- Open your Railway frontend with:
  - `https://<railway-frontend-domain>/?apiBaseUrl=https://<your-ngrok-domain>`
- Run backend with:
  - `ALLOWED_ORIGINS=https://<railway-frontend-domain>`

## Notes

- Match simulation endpoint currently supports `game=kuhn` only.
- AIVAT-style here is a practical control-variate approximation using leave-one-out context means from logged hand-history fields.
