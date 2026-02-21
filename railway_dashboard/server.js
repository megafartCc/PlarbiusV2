const fs = require("fs");
const path = require("path");
const express = require("express");

const app = express();

const PROJECT_ROOT = path.resolve(__dirname, "..");
const DEFAULT_EXPERIMENTS_DIR = path.join(PROJECT_ROOT, "experiments");
const EXPERIMENTS_DIR = path.resolve(process.env.EXPERIMENTS_DIR || DEFAULT_EXPERIMENTS_DIR);
const PORT = Number(process.env.PORT || 3000);
const REFRESH_INTERVAL_MS = Math.max(3000, Number(process.env.REFRESH_INTERVAL_MS || 30000));
const SSE_PING_INTERVAL_MS = 25000;
const ALLOWED_ORIGINS = String(process.env.ALLOWED_ORIGINS || "*")
  .split(",")
  .map((x) => x.trim())
  .filter((x) => Boolean(x));

const runtimeCache = {
  version: 0,
  signature: "",
  refreshed_at_ms: 0,
  next_refresh_at_ms: 0,
  runs: [],
  runs_by_id: new Map()
};

const handHistoryCache = new Map();
const sseClients = new Set();
let refreshInFlight = false;

function isOriginAllowed(origin) {
  if (!origin) {
    return false;
  }
  if (ALLOWED_ORIGINS.includes("*")) {
    return true;
  }
  return ALLOWED_ORIGINS.includes(origin);
}

function assertSafeRunId(runId) {
  if (!/^[a-zA-Z0-9._-]+$/.test(runId)) {
    throw new Error("Invalid run id.");
  }
}

function ensureDirExists(dirPath) {
  return fs.existsSync(dirPath) && fs.statSync(dirPath).isDirectory();
}

function toNumber(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

function toIso(valueMs) {
  if (!Number.isFinite(valueMs) || valueMs <= 0) {
    return null;
  }
  return new Date(valueMs).toISOString();
}

function splitCsvLine(line) {
  const out = [];
  let current = "";
  let inQuotes = false;

  for (let i = 0; i < line.length; i += 1) {
    const ch = line[i];
    if (ch === '"') {
      if (inQuotes && line[i + 1] === '"') {
        current += '"';
        i += 1;
      } else {
        inQuotes = !inQuotes;
      }
      continue;
    }
    if (ch === "," && !inQuotes) {
      out.push(current);
      current = "";
      continue;
    }
    current += ch;
  }
  out.push(current);
  return out;
}

function parseCsvFile(filePath) {
  if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
    return [];
  }
  const raw = fs.readFileSync(filePath, "utf8").trim();
  if (!raw) {
    return [];
  }
  const lines = raw.split(/\r?\n/);
  if (lines.length < 2) {
    return [];
  }
  const header = splitCsvLine(lines[0]);
  const rows = [];

  for (let i = 1; i < lines.length; i += 1) {
    const line = lines[i];
    if (!line) {
      continue;
    }
    const cells = splitCsvLine(line);
    const row = {};
    for (let c = 0; c < header.length; c += 1) {
      row[header[c]] = cells[c] !== undefined ? cells[c] : "";
    }
    rows.push(row);
  }
  return rows;
}

function listRunIds() {
  if (!ensureDirExists(EXPERIMENTS_DIR)) {
    return [];
  }
  return fs
    .readdirSync(EXPERIMENTS_DIR, { withFileTypes: true })
    .filter((dirent) => dirent.isDirectory())
    .map((dirent) => dirent.name)
    .sort((a, b) => b.localeCompare(a));
}

function getRunDir(runId) {
  assertSafeRunId(runId);
  const runDir = path.join(EXPERIMENTS_DIR, runId);
  if (!ensureDirExists(runDir)) {
    throw new Error(`Run not found: ${runId}`);
  }
  return runDir;
}

function resolveArtifactPath(runDir, artifactPath) {
  if (!artifactPath) {
    return null;
  }
  const candidates = [];
  if (path.isAbsolute(artifactPath)) {
    candidates.push(artifactPath);
  } else {
    candidates.push(path.resolve(artifactPath));
    candidates.push(path.join(runDir, artifactPath));
    candidates.push(path.join(PROJECT_ROOT, artifactPath));
  }
  candidates.push(path.join(runDir, path.basename(artifactPath)));

  for (const candidate of candidates) {
    if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
      return path.resolve(candidate);
    }
  }
  return null;
}

function parseSummaryFromDir(runDir) {
  const summaryPath = path.join(runDir, "summary.csv");
  const rows = parseCsvFile(summaryPath).map((row) => {
    const metricsPath = resolveArtifactPath(runDir, row.metrics_path);
    const policyPath = resolveArtifactPath(runDir, row.policy_path);
    const checkpointPath = resolveArtifactPath(runDir, row.checkpoint_path);
    const stdoutPath = resolveArtifactPath(runDir, row.stdout_log);
    return {
      algorithm: row.algorithm || "",
      seed: toNumber(row.seed),
      iterations: toNumber(row.iterations),
      checkpoint_every: toNumber(row.checkpoint_every),
      final_exploitability: toNumber(row.final_exploitability),
      final_nash_conv: toNumber(row.final_nash_conv),
      metrics_path: metricsPath,
      metrics_file: metricsPath ? path.basename(metricsPath) : null,
      policy_path: policyPath,
      policy_file: policyPath ? path.basename(policyPath) : null,
      checkpoint_path: checkpointPath,
      checkpoint_file: checkpointPath ? path.basename(checkpointPath) : null,
      stdout_log: stdoutPath,
      stdout_file: stdoutPath ? path.basename(stdoutPath) : null
    };
  });
  return { summaryPath, rows };
}

function parseAllMetricsFromDir(runDir) {
  const metricsPath = path.join(runDir, "all_metrics.csv");
  const rows = parseCsvFile(metricsPath).map((row) => ({
    algorithm: row.algorithm || "",
    seed: toNumber(row.seed),
    iteration: toNumber(row.iteration),
    infosets: toNumber(row.infosets),
    utility_p0: toNumber(row.utility_p0),
    utility_p1: toNumber(row.utility_p1),
    best_response_p0: toNumber(row.best_response_p0),
    best_response_p1: toNumber(row.best_response_p1),
    nash_conv: toNumber(row.nash_conv),
    exploitability: toNumber(row.exploitability),
    metrics_path: resolveArtifactPath(runDir, row.metrics_path),
    metrics_file: row.metrics_path ? path.basename(row.metrics_path) : null
  }));
  return { metricsPath, rows };
}

function aggregateLearning(metricsRows) {
  const grouped = new Map();
  for (const row of metricsRows) {
    if (row.iteration === null || row.exploitability === null) {
      continue;
    }
    const algo = row.algorithm || "unknown";
    if (!grouped.has(algo)) {
      grouped.set(algo, new Map());
    }
    const byIter = grouped.get(algo);
    if (!byIter.has(row.iteration)) {
      byIter.set(row.iteration, []);
    }
    byIter.get(row.iteration).push(row.exploitability);
  }

  const byAlgorithm = [];
  for (const [algorithm, byIter] of grouped.entries()) {
    const points = Array.from(byIter.entries())
      .sort((a, b) => a[0] - b[0])
      .map(([iteration, values]) => {
        const count = values.length;
        const mean = values.reduce((acc, x) => acc + x, 0) / count;
        const variance =
          count > 1
            ? values.reduce((acc, x) => acc + (x - mean) * (x - mean), 0) / (count - 1)
            : 0;
        const stddev = Math.sqrt(variance);
        const stderr = count > 0 ? stddev / Math.sqrt(count) : 0;
        const ci95 = 1.96 * stderr;
        return {
          iteration,
          count,
          mean,
          stddev,
          stderr,
          ci95,
          min: Math.min(...values),
          max: Math.max(...values)
        };
      });
    byAlgorithm.push({ algorithm, points });
  }

  const bySeedKey = new Map();
  for (const row of metricsRows) {
    if (row.iteration === null || row.exploitability === null) {
      continue;
    }
    const key = `${row.algorithm}|${row.seed}`;
    if (!bySeedKey.has(key)) {
      bySeedKey.set(key, {
        algorithm: row.algorithm,
        seed: row.seed,
        points: []
      });
    }
    bySeedKey.get(key).points.push({
      iteration: row.iteration,
      exploitability: row.exploitability
    });
  }

  const bySeed = Array.from(bySeedKey.values()).map((series) => ({
    algorithm: series.algorithm,
    seed: series.seed,
    points: series.points.sort((a, b) => a.iteration - b.iteration)
  }));

  return { byAlgorithm, bySeed };
}

function walkDir(dirPath, maxDepth = 3, currentDepth = 0) {
  if (!ensureDirExists(dirPath) || currentDepth > maxDepth) {
    return [];
  }
  const out = [];
  const entries = fs.readdirSync(dirPath, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = path.join(dirPath, entry.name);
    if (entry.isDirectory()) {
      out.push(...walkDir(fullPath, maxDepth, currentDepth + 1));
    } else if (entry.isFile()) {
      out.push(fullPath);
    }
  }
  return out;
}

function listPolicyFilesFromDir(runDir, summaryRows) {
  const fromDisk = walkDir(runDir, 2)
    .filter((f) => f.toLowerCase().endsWith(".policy.tsv"))
    .map((f) => path.basename(f));
  const fromSummary = summaryRows.map((row) => row.policy_file).filter((x) => Boolean(x));
  return Array.from(new Set([...fromDisk, ...fromSummary])).sort();
}

function toRelativeFromRun(runDir, absolutePath) {
  const rel = path.relative(runDir, absolutePath);
  return rel.split(path.sep).join("/");
}

function listHandHistoryFilesFromDir(runDir) {
  const candidates = walkDir(runDir, 3);
  const out = [];
  for (const filePath of candidates) {
    const name = path.basename(filePath).toLowerCase();
    const ext = path.extname(filePath).toLowerCase();
    if (!(ext === ".jsonl" || ext === ".csv")) {
      continue;
    }
    if (name === "summary.csv" || name === "all_metrics.csv" || name.endsWith(".metrics.csv")) {
      continue;
    }
    const looksLikeHistory = name.includes("hand") || name.includes("history") || name.includes("match");
    if (!looksLikeHistory) {
      continue;
    }
    const stat = fs.statSync(filePath);
    out.push({
      file: path.basename(filePath),
      relative_path: toRelativeFromRun(runDir, filePath),
      absolute_path: path.resolve(filePath),
      size_bytes: stat.size,
      updated_at: stat.mtime.toISOString()
    });
  }
  out.sort((a, b) => a.relative_path.localeCompare(b.relative_path));
  return out;
}

function computeRunSignaturePart(runRecord) {
  return {
    id: runRecord.id,
    updated_at: runRecord.updated_at,
    summary_rows: runRecord.summary_rows,
    metrics_rows: runRecord.metrics_rows,
    policies: runRecord.policies,
    hand_histories: runRecord.hand_histories.map((h) => ({
      relative_path: h.relative_path,
      updated_at: h.updated_at,
      size_bytes: h.size_bytes
    }))
  };
}

function buildRunRecord(runId) {
  const runDir = getRunDir(runId);
  const runStat = fs.statSync(runDir);
  const { summaryPath, rows: summaryRows } = parseSummaryFromDir(runDir);
  const { metricsPath, rows: metricsRows } = parseAllMetricsFromDir(runDir);
  const policies = listPolicyFilesFromDir(runDir, summaryRows);
  const handHistories = listHandHistoryFilesFromDir(runDir);

  return {
    id: runId,
    run_dir: runDir,
    updated_at: runStat.mtime.toISOString(),
    summary_path: summaryPath,
    metrics_path: metricsPath,
    summary_rows: summaryRows.length,
    metrics_rows: metricsRows.length,
    summary_rows_data: summaryRows,
    metrics_rows_data: metricsRows,
    policies,
    hand_histories: handHistories
  };
}

function broadcastRefreshEvent(payload) {
  const data = `event: refresh\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const res of sseClients) {
    res.write(data);
  }
}

function refreshRunsCache(reason = "manual") {
  if (refreshInFlight) {
    return { changed: false, version: runtimeCache.version, reason: "in_flight" };
  }
  refreshInFlight = true;
  try {
    const runIds = listRunIds();
    const runs = [];
    const runsById = new Map();

    for (const runId of runIds) {
      try {
        const record = buildRunRecord(runId);
        runs.push(record);
        runsById.set(runId, record);
      } catch (error) {
        runs.push({
          id: runId,
          error: error.message,
          summary_rows: 0,
          metrics_rows: 0,
          policies: [],
          hand_histories: [],
          updated_at: null
        });
      }
    }

    const signature = JSON.stringify(runs.map(computeRunSignaturePart));
    const changed = signature !== runtimeCache.signature;
    if (changed) {
      runtimeCache.version += 1;
      runtimeCache.signature = signature;
      runtimeCache.runs = runs;
      runtimeCache.runs_by_id = runsById;
      runtimeCache.refreshed_at_ms = Date.now();
      runtimeCache.next_refresh_at_ms = runtimeCache.refreshed_at_ms + REFRESH_INTERVAL_MS;
      broadcastRefreshEvent({
        version: runtimeCache.version,
        reason,
        refreshed_at: toIso(runtimeCache.refreshed_at_ms),
        runs_count: runtimeCache.runs.length
      });
    } else {
      runtimeCache.refreshed_at_ms = Date.now();
      runtimeCache.next_refresh_at_ms = runtimeCache.refreshed_at_ms + REFRESH_INTERVAL_MS;
    }

    return { changed, version: runtimeCache.version, reason };
  } finally {
    refreshInFlight = false;
  }
}

function ensureCacheReady() {
  if (runtimeCache.refreshed_at_ms === 0) {
    refreshRunsCache("on_demand");
  }
}

function getRunRecord(runId) {
  ensureCacheReady();
  const record = runtimeCache.runs_by_id.get(runId);
  if (!record) {
    throw new Error(`Run not found: ${runId}`);
  }
  if (record.error) {
    throw new Error(record.error);
  }
  return record;
}

function getPathField(obj, keys) {
  for (const key of keys) {
    if (obj[key] !== undefined && obj[key] !== null && String(obj[key]).trim() !== "") {
      return obj[key];
    }
  }
  return null;
}

function normalizeHandRow(raw, fallbackHandNumber) {
  const handId =
    toNumber(getPathField(raw, ["hand_id", "hand", "id", "hand_number", "handIndex"])) ||
    fallbackHandNumber;

  const bbRaw = toNumber(getPathField(raw, ["big_blind", "bb", "blind", "bigBlind", "big_blind_amount"]));
  const bigBlind = bbRaw && bbRaw > 0 ? bbRaw : 1;

  let deltaBb = toNumber(
    getPathField(raw, [
      "delta_p0_bb",
      "p0_delta_bb",
      "player0_delta_bb",
      "utility_p0_bb",
      "result_p0_bb",
      "profit_p0_bb",
      "p0_bb"
    ])
  );

  if (deltaBb === null) {
    const p1Delta = toNumber(
      getPathField(raw, [
        "delta_p1_bb",
        "p1_delta_bb",
        "player1_delta_bb",
        "utility_p1_bb",
        "result_p1_bb",
        "profit_p1_bb",
        "p1_bb"
      ])
    );
    if (p1Delta !== null) {
      deltaBb = -p1Delta;
    }
  }

  if (deltaBb === null) {
    const chips = toNumber(
      getPathField(raw, [
        "delta_p0_chips",
        "player0_delta_chips",
        "utility_p0_chips",
        "result_p0_chips",
        "p0_chips"
      ])
    );
    if (chips !== null) {
      deltaBb = chips / bigBlind;
    }
  }

  if (deltaBb === null) {
    return null;
  }

  const contextParts = [];
  for (const key of [
    "street",
    "round",
    "position",
    "seat",
    "pot_bucket",
    "board_bucket",
    "action_bucket",
    "state_key",
    "public_state"
  ]) {
    if (raw[key] !== undefined && raw[key] !== null && String(raw[key]).trim() !== "") {
      contextParts.push(`${key}:${String(raw[key]).trim()}`);
    }
  }

  return {
    hand: handId,
    delta_p0_bb: deltaBb,
    context_key: contextParts.length > 0 ? contextParts.join("|") : "global",
    timestamp: getPathField(raw, ["timestamp", "ts", "time", "played_at"])
  };
}

function parseJsonlHandHistory(filePath) {
  const lines = fs.readFileSync(filePath, "utf8").split(/\r?\n/);
  const records = [];
  let handCounter = 1;
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) {
      continue;
    }
    let obj;
    try {
      obj = JSON.parse(trimmed);
    } catch (_error) {
      continue;
    }
    const record = normalizeHandRow(obj, handCounter);
    if (!record) {
      continue;
    }
    records.push(record);
    handCounter += 1;
  }
  return records;
}

function parseCsvHandHistory(filePath) {
  const rows = parseCsvFile(filePath);
  const records = [];
  let handCounter = 1;
  for (const row of rows) {
    const record = normalizeHandRow(row, handCounter);
    if (!record) {
      continue;
    }
    records.push(record);
    handCounter += 1;
  }
  return records;
}

function loadHandHistory(filePath) {
  const resolved = path.resolve(filePath);
  if (!fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) {
    throw new Error(`Hand history file not found: ${filePath}`);
  }
  const stat = fs.statSync(resolved);
  const cached = handHistoryCache.get(resolved);
  if (cached && cached.mtime_ms === stat.mtimeMs) {
    return cached.records;
  }

  let records = [];
  const ext = path.extname(resolved).toLowerCase();
  if (ext === ".jsonl") {
    records = parseJsonlHandHistory(resolved);
  } else if (ext === ".csv") {
    records = parseCsvHandHistory(resolved);
  } else {
    throw new Error(`Unsupported hand history format: ${ext}`);
  }

  if (records.length === 0) {
    throw new Error("No usable hand rows found in this hand history file.");
  }

  records.sort((a, b) => a.hand - b.hand);
  handHistoryCache.set(resolved, { mtime_ms: stat.mtimeMs, records });
  return records;
}

function computeAivatLikeSeries(values, contextKeys) {
  const n = values.length;
  if (contextKeys.length !== n) {
    throw new Error("AIVAT-like: values/context size mismatch.");
  }
  const globalSum = values.reduce((acc, x) => acc + x, 0);
  const groupStats = new Map();

  for (let i = 0; i < n; i += 1) {
    const key = contextKeys[i];
    if (!groupStats.has(key)) {
      groupStats.set(key, { sum: 0, count: 0 });
    }
    const stats = groupStats.get(key);
    stats.sum += values[i];
    stats.count += 1;
  }

  const adjusted = new Array(n);
  for (let i = 0; i < n; i += 1) {
    const key = contextKeys[i];
    const x = values[i];
    const stats = groupStats.get(key);
    const globalExcluding = n > 1 ? (globalSum - x) / (n - 1) : globalSum;
    let groupExcluding = globalExcluding;
    if (stats && stats.count > 1) {
      groupExcluding = (stats.sum - x) / (stats.count - 1);
    }
    const controlVariate = groupExcluding - globalExcluding;
    adjusted[i] = x - controlVariate;
  }
  return adjusted;
}

function computeRunningCurve(values, maxPoints = 400) {
  const n = values.length;
  if (n === 0) {
    return {
      hands: 0,
      final_mean_bb_per_hand: 0,
      final_mean_mbb_per_game: 0,
      final_ci95_mbb_per_game: 0,
      points: []
    };
  }

  const stride = Math.max(1, Math.floor(n / Math.max(20, maxPoints)));
  let sum = 0;
  let sumSq = 0;
  const points = [];

  for (let i = 0; i < n; i += 1) {
    const x = values[i];
    sum += x;
    sumSq += x * x;
    const hand = i + 1;

    if (hand === 1 || hand === n || hand % stride === 0) {
      const mean = sum / hand;
      const variance = hand > 1 ? (sumSq - (sum * sum) / hand) / (hand - 1) : 0;
      const stderr = Math.sqrt(Math.max(0, variance) / hand);
      const ci95 = 1.96 * stderr;
      points.push({
        hand,
        mean_bb_per_hand: mean,
        ci95_bb_per_hand: ci95,
        mean_mbb_per_game: mean * 1000,
        ci95_mbb_per_game: ci95 * 1000,
        ci95_lower_mbb_per_game: (mean - ci95) * 1000,
        ci95_upper_mbb_per_game: (mean + ci95) * 1000
      });
    }
  }

  const finalPoint = points[points.length - 1];
  return {
    hands: n,
    final_mean_bb_per_hand: finalPoint.mean_bb_per_hand,
    final_mean_mbb_per_game: finalPoint.mean_mbb_per_game,
    final_ci95_mbb_per_game: finalPoint.ci95_mbb_per_game,
    points
  };
}

function resolveHistoryFileForRun(runRecord, requestedFile) {
  const files = runRecord.hand_histories;
  if (!files || files.length === 0) {
    throw new Error("No hand-history files found for this run.");
  }
  if (!requestedFile) {
    return files[0];
  }
  const normalized = requestedFile.replaceAll("\\", "/");
  const byName = files.find((f) => f.file === requestedFile);
  if (byName) {
    return byName;
  }
  const byRelative = files.find((f) => f.relative_path === normalized);
  if (byRelative) {
    return byRelative;
  }
  throw new Error(`Hand-history file not found in run: ${requestedFile}`);
}

function readPolicyFile(policyPath) {
  const lines = fs.readFileSync(policyPath, "utf8").trim().split(/\r?\n/);
  if (lines.length === 0 || lines[0] !== "PLARBIUS_POLICY_V1") {
    throw new Error(`Unsupported policy format: ${policyPath}`);
  }
  const table = new Map();
  for (let i = 1; i < lines.length; i += 1) {
    if (!lines[i]) {
      continue;
    }
    const parts = lines[i].split("\t");
    if (parts.length !== 3) {
      continue;
    }
    const key = parts[0];
    const actionCount = Number(parts[1]);
    const probs = parts[2].split(",").map((x) => Number(x));
    if (actionCount !== probs.length) {
      continue;
    }
    const sum = probs.reduce((a, b) => a + b, 0);
    const normalized = sum > 0 ? probs.map((p) => p / sum) : probs.map(() => 1 / probs.length);
    table.set(key, normalized);
  }
  return table;
}

function getDist(policy, key, actionCount) {
  const probs = policy.get(key);
  if (!probs || probs.length !== actionCount) {
    const uniform = 1 / actionCount;
    return Array(actionCount).fill(uniform);
  }
  return probs;
}

function rngFromSeed(seed) {
  let state = seed >>> 0;
  return () => {
    state += 0x6d2b79f5;
    let t = Math.imul(state ^ (state >>> 15), 1 | state);
    t ^= t + Math.imul(t ^ (t >>> 7), 61 | t);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

function sampleIndex(probs, rng) {
  let r = rng();
  for (let i = 0; i < probs.length; i += 1) {
    r -= probs[i];
    if (r <= 0) {
      return i;
    }
  }
  return probs.length - 1;
}

function cardLabel(card) {
  if (card === 0) {
    return "J";
  }
  if (card === 1) {
    return "Q";
  }
  return "K";
}

function showdownWinner(card0, card1) {
  return card0 > card1 ? 0 : 1;
}

function simulateKuhnHand(policyA, policyB, rng) {
  const deck = [0, 1, 2];
  const first = Math.floor(rng() * deck.length);
  const card0 = deck[first];
  deck.splice(first, 1);
  const second = Math.floor(rng() * deck.length);
  const card1 = deck[second];

  const cards = [card0, card1];
  const contributions = [1, 1];
  let history = "";
  let currentPlayer = 0;
  let terminal = false;
  let winner = -1;

  while (!terminal) {
    const keyHistory = history.length === 0 ? "_" : history;
    const infosetKey = `${currentPlayer}|${cardLabel(cards[currentPlayer])}|${keyHistory}`;
    const policy = currentPlayer === 0 ? policyA : policyB;

    if (history === "" || history === "c") {
      const dist = getDist(policy, infosetKey, 2);
      const action = sampleIndex(dist, rng);
      if (action === 0) {
        if (history === "") {
          history = "c";
          currentPlayer = 1;
        } else {
          history = "cc";
          terminal = true;
          winner = showdownWinner(cards[0], cards[1]);
        }
      } else {
        if (history === "") {
          history = "b";
        } else {
          history = "cb";
        }
        contributions[currentPlayer] += 1;
        currentPlayer = 1 - currentPlayer;
      }
      continue;
    }

    if (history === "b" || history === "cb") {
      const dist = getDist(policy, infosetKey, 2);
      const action = sampleIndex(dist, rng);
      if (action === 0) {
        contributions[currentPlayer] += 1;
        history = history === "b" ? "bc" : "cbc";
        terminal = true;
        winner = showdownWinner(cards[0], cards[1]);
      } else {
        history = history === "b" ? "bf" : "cbf";
        terminal = true;
        winner = history === "bf" ? 0 : 1;
      }
      continue;
    }

    throw new Error(`Invalid Kuhn history state: ${history}`);
  }

  if (winner === 0) {
    return contributions[1];
  }
  return -contributions[0];
}

function simulateKuhnMatch(policyA, policyB, hands, seed) {
  const rng = rngFromSeed(seed);
  let sum = 0;
  let sumSq = 0;
  const stride = Math.max(1, Math.floor(hands / 400));
  const points = [];

  for (let hand = 1; hand <= hands; hand += 1) {
    const utility = simulateKuhnHand(policyA, policyB, rng);
    sum += utility;
    sumSq += utility * utility;

    if (hand % stride === 0 || hand === 1 || hand === hands) {
      const mean = sum / hand;
      const variance = hand > 1 ? (sumSq - (sum * sum) / hand) / (hand - 1) : 0;
      const stderr = Math.sqrt(Math.max(0, variance) / hand);
      const ci95 = 1.96 * stderr;
      points.push({
        hand,
        mean,
        ci95,
        ci_lower: mean - ci95,
        ci_upper: mean + ci95
      });
    }
  }

  const finalMean = sum / hands;
  const finalVariance = hands > 1 ? (sumSq - (sum * sum) / hands) / (hands - 1) : 0;
  const finalStderr = Math.sqrt(Math.max(0, finalVariance) / hands);
  const finalCi95 = 1.96 * finalStderr;

  return {
    hands,
    seed,
    utility_mean_p0: finalMean,
    utility_mean_p1: -finalMean,
    ci95: finalCi95,
    ci_lower: finalMean - finalCi95,
    ci_upper: finalMean + finalCi95,
    points
  };
}

function listPoliciesForRun(runId) {
  const record = getRunRecord(runId);
  return record.policies;
}

function getRefreshStatus() {
  ensureCacheReady();
  const now = Date.now();
  const nextInMs = runtimeCache.next_refresh_at_ms > now ? runtimeCache.next_refresh_at_ms - now : 0;
  return {
    version: runtimeCache.version,
    refreshed_at: toIso(runtimeCache.refreshed_at_ms),
    next_refresh_at: toIso(runtimeCache.next_refresh_at_ms),
    next_refresh_in_ms: nextInMs,
    refresh_interval_ms: REFRESH_INTERVAL_MS,
    runs_count: runtimeCache.runs.length
  };
}

function startScheduledRefresh() {
  refreshRunsCache("startup");
  const timer = setInterval(() => {
    refreshRunsCache("scheduled");
  }, REFRESH_INTERVAL_MS);
  timer.unref();

  const ssePing = setInterval(() => {
    const payload = `event: ping\ndata: ${JSON.stringify({
      at: new Date().toISOString(),
      version: runtimeCache.version
    })}\n\n`;
    for (const res of sseClients) {
      res.write(payload);
    }
  }, SSE_PING_INTERVAL_MS);
  ssePing.unref();
}

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && isOriginAllowed(origin)) {
    res.setHeader("Access-Control-Allow-Origin", origin);
    res.setHeader("Vary", "Origin");
  } else if (!origin && ALLOWED_ORIGINS.includes("*")) {
    res.setHeader("Access-Control-Allow-Origin", "*");
  }

  res.setHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type,Authorization");

  if (req.method === "OPTIONS") {
    res.status(204).end();
    return;
  }

  next();
});

app.use(express.static(path.join(__dirname, "public")));

app.get("/api/health", (_req, res) => {
  res.json({
    ok: true,
    project_root: PROJECT_ROOT,
    experiments_dir: EXPERIMENTS_DIR,
    refresh_interval_ms: REFRESH_INTERVAL_MS
  });
});

app.get("/api/refresh-status", (_req, res) => {
  try {
    res.json(getRefreshStatus());
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/refresh-now", (_req, res) => {
  try {
    const refresh = refreshRunsCache("manual");
    res.json({
      refresh,
      status: getRefreshStatus()
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/refresh-stream", (req, res) => {
  res.setHeader("Content-Type", "text/event-stream");
  res.setHeader("Cache-Control", "no-cache");
  res.setHeader("Connection", "keep-alive");
  if (typeof res.flushHeaders === "function") {
    res.flushHeaders();
  }

  const hello = {
    type: "hello",
    status: getRefreshStatus()
  };
  res.write(`event: hello\ndata: ${JSON.stringify(hello)}\n\n`);
  sseClients.add(res);

  req.on("close", () => {
    sseClients.delete(res);
  });
});

app.get("/api/runs", (_req, res) => {
  try {
    ensureCacheReady();
    const runs = runtimeCache.runs.map((run) => ({
      id: run.id,
      summary_rows: run.summary_rows || 0,
      metrics_rows: run.metrics_rows || 0,
      policy_count: run.policies ? run.policies.length : 0,
      hand_history_count: run.hand_histories ? run.hand_histories.length : 0,
      updated_at: run.updated_at,
      error: run.error || null
    }));
    res.json({ runs, refresh: getRefreshStatus() });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/summary", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    res.json({
      run_id: run.id,
      rows: run.summary_rows_data
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/metrics", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    res.json({
      run_id: run.id,
      rows: run.metrics_rows_data
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/learning", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    const learning = aggregateLearning(run.metrics_rows_data);
    res.json({
      run_id: run.id,
      by_algorithm: learning.byAlgorithm,
      by_seed: learning.bySeed
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/policies", (req, res) => {
  try {
    const policies = listPoliciesForRun(req.params.runId);
    res.json({
      run_id: req.params.runId,
      policies
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/hand-histories", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    res.json({
      run_id: run.id,
      files: run.hand_histories
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/mbb", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    const method = String(req.query.method || "both").toLowerCase();
    const requestedFile = req.query.handHistory || req.query.file || req.query.history;
    const maxPoints = Math.max(20, Math.min(2000, Number(req.query.maxPoints || 500)));
    const historyFile = resolveHistoryFileForRun(run, requestedFile);
    const records = loadHandHistory(historyFile.absolute_path);

    const rawValues = records.map((r) => r.delta_p0_bb);
    const contextKeys = records.map((r) => r.context_key || "global");
    const contextCount = new Set(contextKeys).size;

    const response = {
      run_id: run.id,
      hand_history_file: historyFile.file,
      hand_history_relative_path: historyFile.relative_path,
      method,
      hands_count: records.length,
      context_keys_count: contextCount,
      notes: "AIVAT-style uses leave-one-out context control variates from hand-history contexts."
    };

    if (method === "raw" || method === "both") {
      response.raw = computeRunningCurve(rawValues, maxPoints);
    }
    if (method === "aivat_like" || method === "both") {
      const adjusted = computeAivatLikeSeries(rawValues, contextKeys);
      response.aivat_like = computeRunningCurve(adjusted, maxPoints);
    }
    if (!response.raw && !response.aivat_like) {
      throw new Error("Invalid method. Use raw, aivat_like, or both.");
    }

    res.json(response);
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/match", (req, res) => {
  try {
    const run = getRunRecord(req.params.runId);
    const policyAName = req.query.policyA;
    const policyBName = req.query.policyB || policyAName;
    const hands = Math.max(10, Math.min(500000, Number(req.query.hands || 10000)));
    const seed = Math.max(1, Number(req.query.seed || 1));
    const game = req.query.game || "kuhn";

    if (!policyAName) {
      throw new Error("Missing required query param: policyA");
    }
    if (game !== "kuhn") {
      throw new Error("Match simulation currently supports only game=kuhn.");
    }

    const policyAPath = resolveArtifactPath(run.run_dir, policyAName);
    const policyBPath = resolveArtifactPath(run.run_dir, policyBName);
    if (!policyAPath) {
      throw new Error(`Policy file not found: ${policyAName}`);
    }
    if (!policyBPath) {
      throw new Error(`Policy file not found: ${policyBName}`);
    }

    const policyA = readPolicyFile(policyAPath);
    const policyB = readPolicyFile(policyBPath);
    const report = simulateKuhnMatch(policyA, policyB, hands, seed);
    res.json({
      run_id: run.id,
      game,
      policy_a: path.basename(policyAPath),
      policy_b: path.basename(policyBPath),
      report
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("*", (_req, res) => {
  res.sendFile(path.join(__dirname, "public", "index.html"));
});

startScheduledRefresh();

app.listen(PORT, () => {
  console.log(`Plarbius dashboard listening on port ${PORT}`);
  console.log(`Experiments dir: ${EXPERIMENTS_DIR}`);
  console.log(`Refresh interval: ${REFRESH_INTERVAL_MS} ms`);
  console.log(`Allowed origins: ${ALLOWED_ORIGINS.join(",")}`);
});
