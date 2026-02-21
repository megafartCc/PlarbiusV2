const runSelect = document.getElementById("runSelect");
const refreshRunsBtn = document.getElementById("refreshRunsBtn");
const loadRunBtn = document.getElementById("loadRunBtn");
const statusLine = document.getElementById("statusLine");
const refreshMetaLine = document.getElementById("refreshMetaLine");

const summaryStats = document.getElementById("summaryStats");
const summaryTbody = document.querySelector("#summaryTable tbody");

const policyASelect = document.getElementById("policyASelect");
const policyBSelect = document.getElementById("policyBSelect");
const handsInput = document.getElementById("handsInput");
const seedInput = document.getElementById("seedInput");
const runMatchBtn = document.getElementById("runMatchBtn");
const matchStats = document.getElementById("matchStats");

const handHistorySelect = document.getElementById("handHistorySelect");
const handMethodSelect = document.getElementById("handMethodSelect");
const handMaxPointsInput = document.getElementById("handMaxPointsInput");
const loadHandChartBtn = document.getElementById("loadHandChartBtn");
const handChartStats = document.getElementById("handChartStats");

const state = {
  runId: null,
  runs: [],
  summaryRows: [],
  learning: { by_algorithm: [], by_seed: [] },
  policies: [],
  handHistories: [],
  refresh: null,
  refreshStream: null,
  loadingRunsPromise: null,
  loadingRunPromise: null,
  lastRefreshVersionHandled: 0
};

const charts = {
  seedLearning: null,
  meanLearning: null,
  finalExploitability: null,
  match: null,
  handHistory: null
};

function setStatus(message, isError = false) {
  statusLine.textContent = message;
  statusLine.classList.toggle("error", Boolean(isError));
}

async function fetchJson(url) {
  const response = await fetch(url);
  const data = await response.json();
  if (!response.ok) {
    throw new Error(data.error || `Request failed: ${url}`);
  }
  return data;
}

function algoColor(algo, alpha = 1) {
  const key = String(algo || "unknown").toLowerCase();
  if (key.includes("mccfr")) {
    return `rgba(15, 118, 110, ${alpha})`;
  }
  if (key.includes("cfr")) {
    return `rgba(29, 78, 216, ${alpha})`;
  }
  return `rgba(156, 163, 175, ${alpha})`;
}

function fmt(value, digits = 6) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return "-";
  }
  return Number(value).toFixed(digits);
}

function fmtInt(value) {
  if (!Number.isFinite(Number(value))) {
    return "-";
  }
  return Math.round(Number(value)).toLocaleString();
}

function fmtTimestamp(value) {
  if (!value) {
    return "-";
  }
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) {
    return String(value);
  }
  return date.toLocaleString();
}

function fmtDurationMs(ms) {
  const value = Number(ms);
  if (!Number.isFinite(value) || value <= 0) {
    return "0s";
  }
  const totalSeconds = Math.floor(value / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  if (minutes <= 0) {
    return `${seconds}s`;
  }
  return `${minutes}m ${seconds}s`;
}

function destroyChart(chartKey) {
  if (charts[chartKey]) {
    charts[chartKey].destroy();
    charts[chartKey] = null;
  }
}

function clearCharts() {
  destroyChart("seedLearning");
  destroyChart("meanLearning");
  destroyChart("finalExploitability");
  destroyChart("match");
  destroyChart("handHistory");
}

function setRefreshState(refresh) {
  if (!refresh) {
    return;
  }
  state.refresh = refresh;
  if (Number.isFinite(Number(refresh.version))) {
    state.lastRefreshVersionHandled = Math.max(state.lastRefreshVersionHandled, Number(refresh.version));
  }
  renderRefreshMeta();
}

function renderRefreshMeta() {
  if (!state.refresh) {
    refreshMetaLine.textContent = "Refresh status: unavailable";
    return;
  }
  const refresh = state.refresh;
  const nextIn = refresh.next_refresh_at
    ? Math.max(0, new Date(refresh.next_refresh_at).getTime() - Date.now())
    : refresh.next_refresh_in_ms;
  refreshMetaLine.textContent =
    `Refresh v${refresh.version} | last ${fmtTimestamp(refresh.refreshed_at)} | ` +
    `next in ${fmtDurationMs(nextIn)} | interval ${fmtDurationMs(refresh.refresh_interval_ms)} | runs ${fmtInt(refresh.runs_count)}`;
}

function renderRunOptions() {
  runSelect.innerHTML = "";
  state.runs.forEach((run) => {
    const option = document.createElement("option");
    option.value = run.id;
    const errorSuffix = run.error ? " [error]" : "";
    option.textContent = `${run.id}${errorSuffix} (summary:${run.summary_rows}, metrics:${run.metrics_rows}, hh:${run.hand_history_count})`;
    runSelect.appendChild(option);
  });
}

function renderSummary() {
  summaryTbody.innerHTML = "";
  if (state.summaryRows.length === 0) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 6;
    td.textContent = "No summary rows found in this run.";
    tr.appendChild(td);
    summaryTbody.appendChild(tr);
  } else {
    state.summaryRows.forEach((row) => {
      const tr = document.createElement("tr");
      tr.innerHTML = `
        <td>${row.algorithm || "-"}</td>
        <td>${row.seed ?? "-"}</td>
        <td>${row.iterations ?? "-"}</td>
        <td>${fmt(row.final_exploitability)}</td>
        <td>${fmt(row.final_nash_conv)}</td>
        <td>${row.policy_file || "-"}</td>
      `;
      summaryTbody.appendChild(tr);
    });
  }

  const algoGroups = new Map();
  state.summaryRows.forEach((row) => {
    const key = row.algorithm || "unknown";
    if (!algoGroups.has(key)) {
      algoGroups.set(key, []);
    }
    const value = Number(row.final_exploitability);
    if (Number.isFinite(value)) {
      algoGroups.get(key).push(value);
    }
  });

  const totalRows = state.summaryRows.length;
  const bestRow = [...state.summaryRows]
    .filter((r) => Number.isFinite(Number(r.final_exploitability)))
    .sort((a, b) => Number(a.final_exploitability) - Number(b.final_exploitability))[0];

  const algoCards = [];
  for (const [algo, values] of algoGroups.entries()) {
    if (values.length === 0) {
      continue;
    }
    const mean = values.reduce((a, b) => a + b, 0) / values.length;
    algoCards.push(`
      <div class="summary-pill">
        <div class="k">${algo} mean final exploitability</div>
        <div class="v">${fmt(mean)}</div>
      </div>
    `);
  }

  summaryStats.innerHTML = `
    <div class="summary-pill">
      <div class="k">Run</div>
      <div class="v">${state.runId || "-"}</div>
    </div>
    <div class="summary-pill">
      <div class="k">Summary rows</div>
      <div class="v">${totalRows}</div>
    </div>
    <div class="summary-pill">
      <div class="k">Best row (min exploitability)</div>
      <div class="v">${bestRow ? `${bestRow.algorithm} seed ${bestRow.seed}: ${fmt(bestRow.final_exploitability)}` : "-"}</div>
    </div>
    ${algoCards.join("")}
  `;
}

function renderLearningCharts() {
  const seedCtx = document.getElementById("seedLearningChart");
  const meanCtx = document.getElementById("meanLearningChart");
  const finalCtx = document.getElementById("finalExploitabilityChart");

  destroyChart("seedLearning");
  destroyChart("meanLearning");
  destroyChart("finalExploitability");

  const seedDatasets = state.learning.by_seed.map((series) => ({
    label: `${series.algorithm} seed ${series.seed}`,
    data: series.points.map((p) => ({ x: p.iteration, y: p.exploitability })),
    borderColor: algoColor(series.algorithm, 0.4),
    backgroundColor: algoColor(series.algorithm, 0.4),
    pointRadius: 2,
    borderWidth: 1,
    tension: 0.1
  }));

  charts.seedLearning = new Chart(seedCtx, {
    type: "line",
    data: { datasets: seedDatasets },
    options: {
      responsive: true,
      plugins: {
        legend: { display: true, position: "bottom" }
      },
      scales: {
        x: { type: "linear", title: { display: true, text: "Iteration" } },
        y: { title: { display: true, text: "Exploitability" } }
      }
    }
  });

  const meanDatasets = [];
  state.learning.by_algorithm.forEach((series) => {
    const upper = series.points.map((p) => ({ x: p.iteration, y: p.mean + p.ci95 }));
    const lower = series.points.map((p) => ({ x: p.iteration, y: p.mean - p.ci95 }));
    const mean = series.points.map((p) => ({ x: p.iteration, y: p.mean }));

    meanDatasets.push({
      label: `${series.algorithm} CI upper`,
      data: upper,
      borderColor: algoColor(series.algorithm, 0),
      pointRadius: 0,
      borderWidth: 0
    });
    meanDatasets.push({
      label: `${series.algorithm} CI band`,
      data: lower,
      borderColor: algoColor(series.algorithm, 0),
      backgroundColor: algoColor(series.algorithm, 0.16),
      pointRadius: 0,
      borderWidth: 0,
      fill: "-1"
    });
    meanDatasets.push({
      label: `${series.algorithm} mean`,
      data: mean,
      borderColor: algoColor(series.algorithm, 1),
      backgroundColor: algoColor(series.algorithm, 1),
      pointRadius: 2,
      borderWidth: 2,
      tension: 0.1
    });
  });

  charts.meanLearning = new Chart(meanCtx, {
    type: "line",
    data: { datasets: meanDatasets },
    options: {
      responsive: true,
      plugins: {
        legend: {
          position: "bottom",
          labels: {
            filter: (item) => !item.text.includes("CI upper") && !item.text.includes("CI band")
          }
        }
      },
      scales: {
        x: { type: "linear", title: { display: true, text: "Iteration" } },
        y: { title: { display: true, text: "Mean Exploitability" } }
      }
    }
  });

  const labels = state.summaryRows.map((row) => `${row.algorithm} seed ${row.seed}`);
  const values = state.summaryRows.map((row) => Number(row.final_exploitability));
  const colors = state.summaryRows.map((row) => algoColor(row.algorithm, 0.8));

  charts.finalExploitability = new Chart(finalCtx, {
    type: "bar",
    data: {
      labels,
      datasets: [
        {
          label: "Final exploitability",
          data: values,
          backgroundColor: colors
        }
      ]
    },
    options: {
      responsive: true,
      plugins: { legend: { display: false } },
      scales: {
        y: { title: { display: true, text: "Exploitability" } }
      }
    }
  });
}

function renderPolicyOptions() {
  const optionsHtml = state.policies
    .map((name) => `<option value="${name}">${name}</option>`)
    .join("");
  policyASelect.innerHTML = optionsHtml;
  policyBSelect.innerHTML = optionsHtml;
  if (state.policies.length >= 2) {
    policyASelect.selectedIndex = 0;
    policyBSelect.selectedIndex = 1;
  } else if (state.policies.length === 1) {
    policyASelect.selectedIndex = 0;
    policyBSelect.selectedIndex = 0;
  }
}

function renderHandHistoryOptions(previousSelection = null) {
  handHistorySelect.innerHTML = "";
  if (state.handHistories.length === 0) {
    const option = document.createElement("option");
    option.value = "";
    option.textContent = "No hand-history files found";
    handHistorySelect.appendChild(option);
    handHistorySelect.disabled = true;
    loadHandChartBtn.disabled = true;
    handChartStats.textContent = "No hand-history files found for this run.";
    destroyChart("handHistory");
    return;
  }

  for (const file of state.handHistories) {
    const option = document.createElement("option");
    option.value = file.relative_path;
    option.textContent = `${file.relative_path} (${fmtInt(file.size_bytes)} bytes)`;
    handHistorySelect.appendChild(option);
  }

  handHistorySelect.disabled = false;
  loadHandChartBtn.disabled = false;

  if (previousSelection && state.handHistories.some((f) => f.relative_path === previousSelection)) {
    handHistorySelect.value = previousSelection;
  } else {
    handHistorySelect.selectedIndex = 0;
  }
}

function buildMbbDatasets(curve, labelPrefix, colorLine, colorFill) {
  const upper = curve.points.map((p) => ({ x: p.hand, y: p.ci95_upper_mbb_per_game }));
  const lower = curve.points.map((p) => ({ x: p.hand, y: p.ci95_lower_mbb_per_game }));
  const mean = curve.points.map((p) => ({ x: p.hand, y: p.mean_mbb_per_game }));

  return [
    {
      label: `${labelPrefix} CI upper`,
      data: upper,
      borderColor: "rgba(0,0,0,0)",
      pointRadius: 0,
      borderWidth: 0
    },
    {
      label: `${labelPrefix} CI band`,
      data: lower,
      borderColor: "rgba(0,0,0,0)",
      backgroundColor: colorFill,
      pointRadius: 0,
      borderWidth: 0,
      fill: "-1"
    },
    {
      label: `${labelPrefix} mean`,
      data: mean,
      borderColor: colorLine,
      backgroundColor: colorLine,
      borderWidth: 2,
      tension: 0.1,
      pointRadius: 1
    }
  ];
}

function renderHandHistoryChart(result) {
  const datasets = [];

  if (result.raw && Array.isArray(result.raw.points) && result.raw.points.length > 0) {
    datasets.push(...buildMbbDatasets(result.raw, "Raw", "rgba(234, 88, 12, 1)", "rgba(234, 88, 12, 0.14)"));
  }
  if (result.aivat_like && Array.isArray(result.aivat_like.points) && result.aivat_like.points.length > 0) {
    datasets.push(...buildMbbDatasets(result.aivat_like, "AIVAT-like", "rgba(21, 128, 61, 1)", "rgba(21, 128, 61, 0.15)"));
  }

  destroyChart("handHistory");

  charts.handHistory = new Chart(document.getElementById("handHistoryChart"), {
    type: "line",
    data: { datasets },
    options: {
      responsive: true,
      plugins: {
        legend: {
          position: "bottom",
          labels: {
            filter: (item) => !item.text.includes("CI upper") && !item.text.includes("CI band")
          }
        }
      },
      scales: {
        x: { type: "linear", title: { display: true, text: "Hands Played" } },
        y: { title: { display: true, text: "Running mean (mbb/game, P0)" } }
      }
    }
  });

  const parts = [
    `${result.hand_history_relative_path}`,
    `hands=${fmtInt(result.hands_count)}`,
    `contexts=${fmtInt(result.context_keys_count)}`
  ];

  if (result.raw) {
    parts.push(`raw=${fmt(result.raw.final_mean_mbb_per_game, 3)} +/- ${fmt(result.raw.final_ci95_mbb_per_game, 3)} mbb/game`);
  }
  if (result.aivat_like) {
    parts.push(`aivat-like=${fmt(result.aivat_like.final_mean_mbb_per_game, 3)} +/- ${fmt(result.aivat_like.final_ci95_mbb_per_game, 3)} mbb/game`);
  }

  handChartStats.textContent = parts.join(" | ");
}

async function runMatch() {
  if (!state.runId) {
    return;
  }
  const policyA = policyASelect.value;
  const policyB = policyBSelect.value;
  const hands = Math.max(10, Number(handsInput.value || 10000));
  const seed = Math.max(1, Number(seedInput.value || 1));

  setStatus(`Running match simulation: ${policyA} vs ${policyB} ...`);
  const url = `/api/runs/${encodeURIComponent(state.runId)}/match?policyA=${encodeURIComponent(policyA)}&policyB=${encodeURIComponent(policyB)}&hands=${hands}&seed=${seed}`;
  const result = await fetchJson(url);

  const points = result.report.points || [];
  const upper = points.map((p) => ({ x: p.hand, y: p.ci_upper }));
  const lower = points.map((p) => ({ x: p.hand, y: p.ci_lower }));
  const mean = points.map((p) => ({ x: p.hand, y: p.mean }));

  destroyChart("match");

  charts.match = new Chart(document.getElementById("matchChart"), {
    type: "line",
    data: {
      datasets: [
        {
          label: "95% CI upper",
          data: upper,
          borderColor: "rgba(59, 130, 246, 0)",
          borderWidth: 0,
          pointRadius: 0
        },
        {
          label: "95% CI band",
          data: lower,
          borderColor: "rgba(59, 130, 246, 0)",
          backgroundColor: "rgba(59, 130, 246, 0.2)",
          fill: "-1",
          borderWidth: 0,
          pointRadius: 0
        },
        {
          label: "Running mean utility (P0)",
          data: mean,
          borderColor: "rgba(37, 99, 235, 1)",
          backgroundColor: "rgba(37, 99, 235, 1)",
          borderWidth: 2,
          tension: 0.1,
          pointRadius: 1
        }
      ]
    },
    options: {
      responsive: true,
      plugins: {
        legend: {
          labels: {
            filter: (item) => !item.text.includes("upper") && !item.text.includes("band")
          }
        }
      },
      scales: {
        x: { type: "linear", title: { display: true, text: "Hands Played" } },
        y: { title: { display: true, text: "Utility / hand (P0)" } }
      }
    }
  });

  matchStats.textContent =
    `Final mean utility P0=${fmt(result.report.utility_mean_p0)} ` +
    `(95% CI [${fmt(result.report.ci_lower)}, ${fmt(result.report.ci_upper)}]) over ${result.report.hands} hands`;
  setStatus(`Loaded run ${state.runId}.`);
}

async function loadHandHistoryChart() {
  if (!state.runId) {
    return;
  }
  if (state.handHistories.length === 0 || !handHistorySelect.value) {
    handChartStats.textContent = "No hand-history files available for this run.";
    destroyChart("handHistory");
    return;
  }

  const handHistory = handHistorySelect.value;
  const method = handMethodSelect.value;
  const maxPoints = Math.max(20, Math.min(2000, Number(handMaxPointsInput.value || 500)));

  setStatus(`Loading hand-history chart (${method})...`);
  const url =
    `/api/runs/${encodeURIComponent(state.runId)}/mbb?handHistory=${encodeURIComponent(handHistory)}` +
    `&method=${encodeURIComponent(method)}&maxPoints=${maxPoints}`;
  const result = await fetchJson(url);

  renderHandHistoryChart(result);
  setStatus(`Loaded run ${state.runId}.`);
}

async function loadRun(runId, options = {}) {
  const { silent = false, autoLoadHandChart = true } = options;
  if (state.loadingRunPromise) {
    return state.loadingRunPromise;
  }

  state.loadingRunPromise = (async () => {
    state.runId = runId;
    if (!silent) {
      setStatus(`Loading run ${runId}...`);
    }

    const previousHistorySelection = handHistorySelect.value;

    const [summaryData, learningData, policyData, handData] = await Promise.all([
      fetchJson(`/api/runs/${encodeURIComponent(runId)}/summary`),
      fetchJson(`/api/runs/${encodeURIComponent(runId)}/learning`),
      fetchJson(`/api/runs/${encodeURIComponent(runId)}/policies`),
      fetchJson(`/api/runs/${encodeURIComponent(runId)}/hand-histories`)
    ]);

    state.summaryRows = summaryData.rows || [];
    state.learning = {
      by_algorithm: learningData.by_algorithm || [],
      by_seed: learningData.by_seed || []
    };
    state.policies = policyData.policies || [];
    state.handHistories = handData.files || [];

    renderSummary();
    renderLearningCharts();
    renderPolicyOptions();
    renderHandHistoryOptions(previousHistorySelection);

    matchStats.textContent = "";
    destroyChart("match");

    if (autoLoadHandChart && state.handHistories.length > 0) {
      await loadHandHistoryChart();
    } else {
      destroyChart("handHistory");
      handChartStats.textContent = state.handHistories.length
        ? "Select a hand-history file and click Load Hand Chart."
        : "No hand-history files found for this run.";
    }

    if (!silent) {
      setStatus(`Loaded run ${runId}.`);
    }
  })();

  try {
    await state.loadingRunPromise;
  } finally {
    state.loadingRunPromise = null;
  }
}

function chooseRunId(preserveSelection = true) {
  if (preserveSelection && state.runId && state.runs.some((r) => r.id === state.runId && !r.error)) {
    return state.runId;
  }
  const healthy = state.runs.find((r) => !r.error);
  if (healthy) {
    return healthy.id;
  }
  return state.runs.length > 0 ? state.runs[0].id : null;
}

async function loadRunsAndMaybeSelectLatest(options = {}) {
  const { preserveSelection = true, silent = false, forceBackendRefresh = false } = options;

  if (state.loadingRunsPromise) {
    return state.loadingRunsPromise;
  }

  state.loadingRunsPromise = (async () => {
    if (!silent) {
      setStatus("Loading run list...");
    }

    if (forceBackendRefresh) {
      const refreshNow = await fetchJson("/api/refresh-now");
      if (refreshNow.status) {
        setRefreshState(refreshNow.status);
      }
    }

    const data = await fetchJson("/api/runs");
    state.runs = data.runs || [];
    if (data.refresh) {
      setRefreshState(data.refresh);
    }
    renderRunOptions();

    if (state.runs.length === 0) {
      setStatus("No runs found. Set EXPERIMENTS_DIR or run experiments first.", true);
      clearCharts();
      summaryTbody.innerHTML = "";
      summaryStats.innerHTML = "";
      policyASelect.innerHTML = "";
      policyBSelect.innerHTML = "";
      handHistorySelect.innerHTML = "";
      handChartStats.textContent = "";
      return;
    }

    const selected = chooseRunId(preserveSelection);
    if (!selected) {
      setStatus("No runnable runs found.", true);
      return;
    }

    runSelect.value = selected;
    await loadRun(selected, { silent: true, autoLoadHandChart: true });

    if (!silent) {
      setStatus(`Loaded run ${selected}.`);
    }
  })();

  try {
    await state.loadingRunsPromise;
  } finally {
    state.loadingRunsPromise = null;
  }
}

async function handleRefreshEvent(payload) {
  if (!payload || !Number.isFinite(Number(payload.version))) {
    return;
  }
  const version = Number(payload.version);

  if (state.refresh) {
    state.refresh = {
      ...state.refresh,
      version,
      refreshed_at: payload.refreshed_at || state.refresh.refreshed_at,
      runs_count: payload.runs_count ?? state.refresh.runs_count
    };
    renderRefreshMeta();
  }

  if (version <= state.lastRefreshVersionHandled) {
    return;
  }

  state.lastRefreshVersionHandled = version;
  try {
    setStatus(`Detected new artifacts (refresh v${version}). Syncing dashboard...`);
    await loadRunsAndMaybeSelectLatest({ preserveSelection: true, silent: true, forceBackendRefresh: false });
    setStatus(`Dashboard synced to refresh v${version}.`);
  } catch (error) {
    setStatus(error.message, true);
  }
}

function connectRefreshStream() {
  if (typeof EventSource === "undefined") {
    refreshMetaLine.textContent = `${refreshMetaLine.textContent} | live stream unavailable in this browser`;
    return;
  }

  if (state.refreshStream) {
    state.refreshStream.close();
    state.refreshStream = null;
  }

  const stream = new EventSource("/api/refresh-stream");
  state.refreshStream = stream;

  stream.addEventListener("hello", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (payload.status) {
        setRefreshState(payload.status);
      }
    } catch (_error) {
    }
  });

  stream.addEventListener("refresh", (event) => {
    try {
      const payload = JSON.parse(event.data);
      handleRefreshEvent(payload);
    } catch (_error) {
    }
  });

  stream.addEventListener("ping", (event) => {
    try {
      const payload = JSON.parse(event.data);
      if (state.refresh && Number.isFinite(Number(payload.version))) {
        state.refresh.version = Number(payload.version);
      }
      renderRefreshMeta();
    } catch (_error) {
    }
  });

  stream.onerror = () => {
    if (refreshMetaLine.textContent && !refreshMetaLine.textContent.includes("stream reconnecting")) {
      refreshMetaLine.textContent = `${refreshMetaLine.textContent} | stream reconnecting...`;
    }
  };
}

refreshRunsBtn.addEventListener("click", async () => {
  try {
    await loadRunsAndMaybeSelectLatest({ preserveSelection: true, silent: false, forceBackendRefresh: true });
  } catch (error) {
    setStatus(error.message, true);
  }
});

loadRunBtn.addEventListener("click", async () => {
  try {
    const runId = runSelect.value;
    await loadRun(runId, { silent: false, autoLoadHandChart: true });
  } catch (error) {
    setStatus(error.message, true);
  }
});

runMatchBtn.addEventListener("click", async () => {
  try {
    await runMatch();
  } catch (error) {
    setStatus(error.message, true);
  }
});

loadHandChartBtn.addEventListener("click", async () => {
  try {
    await loadHandHistoryChart();
  } catch (error) {
    setStatus(error.message, true);
  }
});

handHistorySelect.addEventListener("change", async () => {
  if (state.handHistories.length > 0) {
    try {
      await loadHandHistoryChart();
    } catch (error) {
      setStatus(error.message, true);
    }
  }
});

setInterval(() => {
  renderRefreshMeta();
}, 1000);

(async () => {
  try {
    const status = await fetchJson("/api/refresh-status");
    setRefreshState(status);
  } catch (_error) {
  }

  connectRefreshStream();

  try {
    await loadRunsAndMaybeSelectLatest({ preserveSelection: true, silent: false, forceBackendRefresh: false });
  } catch (error) {
    setStatus(error.message, true);
  }
})();
