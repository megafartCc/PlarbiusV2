const fs = require("fs");
const path = require("path");
const express = require("express");

const app = express();

const PROJECT_ROOT = path.resolve(__dirname, "..");
const DEFAULT_EXPERIMENTS_DIR = path.join(PROJECT_ROOT, "experiments");
const EXPERIMENTS_DIR = path.resolve(process.env.EXPERIMENTS_DIR || DEFAULT_EXPERIMENTS_DIR);
const PORT = Number(process.env.PORT || 3000);

function assertSafeRunId(runId) {
  if (!/^[a-zA-Z0-9._-]+$/.test(runId)) {
    throw new Error("Invalid run id.");
  }
}

function getRunDir(runId) {
  assertSafeRunId(runId);
  const runDir = path.join(EXPERIMENTS_DIR, runId);
  if (!fs.existsSync(runDir) || !fs.statSync(runDir).isDirectory()) {
    throw new Error(`Run not found: ${runId}`);
  }
  return runDir;
}

function parseCsvFile(filePath) {
  if (!fs.existsSync(filePath)) {
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
  const header = lines[0].split(",");
  const rows = [];
  for (let i = 1; i < lines.length; i += 1) {
    if (!lines[i]) {
      continue;
    }
    const cells = lines[i].split(",");
    const row = {};
    for (let c = 0; c < header.length; c += 1) {
      row[header[c]] = cells[c] !== undefined ? cells[c] : "";
    }
    rows.push(row);
  }
  return rows;
}

function countCsvRows(filePath) {
  if (!fs.existsSync(filePath)) {
    return 0;
  }
  const rows = parseCsvFile(filePath);
  return rows.length;
}

function toNumber(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

function listRunIds() {
  if (!fs.existsSync(EXPERIMENTS_DIR)) {
    return [];
  }
  return fs
    .readdirSync(EXPERIMENTS_DIR, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => d.name)
    .sort((a, b) => b.localeCompare(a));
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

function parseSummary(runId) {
  const runDir = getRunDir(runId);
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
  return rows;
}

function parseAllMetrics(runId) {
  const runDir = getRunDir(runId);
  const metricsPath = path.join(runDir, "all_metrics.csv");
  return parseCsvFile(metricsPath).map((row) => ({
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
        const mean = values.reduce((acc, v) => acc + v, 0) / count;
        const variance =
          count > 1
            ? values.reduce((acc, v) => acc + (v - mean) * (v - mean), 0) / (count - 1)
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
      const action = sampleIndex(dist, rng); // 0 check, 1 bet
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
      const action = sampleIndex(dist, rng); // 0 call, 1 fold
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

function listPolicyFiles(runId) {
  const runDir = getRunDir(runId);
  const fromDir = fs
    .readdirSync(runDir)
    .filter((name) => name.endsWith(".policy.tsv"))
    .sort();

  const fromSummary = parseSummary(runId)
    .map((row) => row.policy_file)
    .filter((x) => Boolean(x));

  return Array.from(new Set([...fromDir, ...fromSummary])).sort();
}

app.use(express.static(path.join(__dirname, "public")));

app.get("/api/health", (_req, res) => {
  res.json({
    ok: true,
    project_root: PROJECT_ROOT,
    experiments_dir: EXPERIMENTS_DIR
  });
});

app.get("/api/runs", (_req, res) => {
  const runIds = listRunIds();
  const runs = runIds.map((id) => {
    const runDir = path.join(EXPERIMENTS_DIR, id);
    const summaryPath = path.join(runDir, "summary.csv");
    const metricsPath = path.join(runDir, "all_metrics.csv");
    return {
      id,
      summary_rows: countCsvRows(summaryPath),
      metrics_rows: countCsvRows(metricsPath),
      updated_at: fs.statSync(runDir).mtime.toISOString()
    };
  });
  res.json({ runs });
});

app.get("/api/runs/:runId/summary", (req, res) => {
  try {
    const rows = parseSummary(req.params.runId);
    res.json({ run_id: req.params.runId, rows });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/metrics", (req, res) => {
  try {
    const rows = parseAllMetrics(req.params.runId);
    res.json({ run_id: req.params.runId, rows });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/learning", (req, res) => {
  try {
    const rows = parseAllMetrics(req.params.runId);
    const learning = aggregateLearning(rows);
    res.json({
      run_id: req.params.runId,
      by_algorithm: learning.byAlgorithm,
      by_seed: learning.bySeed
    });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/policies", (req, res) => {
  try {
    const policyFiles = listPolicyFiles(req.params.runId);
    res.json({ run_id: req.params.runId, policies: policyFiles });
  } catch (error) {
    res.status(400).json({ error: error.message });
  }
});

app.get("/api/runs/:runId/match", (req, res) => {
  try {
    const runDir = getRunDir(req.params.runId);
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

    const policyAPath = resolveArtifactPath(runDir, policyAName);
    const policyBPath = resolveArtifactPath(runDir, policyBName);
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
      run_id: req.params.runId,
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

app.listen(PORT, () => {
  console.log(`Plarbius dashboard listening on port ${PORT}`);
  console.log(`Experiments dir: ${EXPERIMENTS_DIR}`);
});

