const runSelect = document.getElementById("runSelect");
const refreshRunsBtn = document.getElementById("refreshRunsBtn");
const loadRunBtn = document.getElementById("loadRunBtn");
const statusLine = document.getElementById("statusLine");

const summaryStats = document.getElementById("summaryStats");
const summaryTbody = document.querySelector("#summaryTable tbody");

const policyASelect = document.getElementById("policyASelect");
const policyBSelect = document.getElementById("policyBSelect");
const handsInput = document.getElementById("handsInput");
const seedInput = document.getElementById("seedInput");
const runMatchBtn = document.getElementById("runMatchBtn");
const matchStats = document.getElementById("matchStats");

const state = {
  runId: null,
  runs: [],
  summaryRows: [],
  learning: { by_algorithm: [], by_seed: [] },
  policies: []
};

const charts = {
  seedLearning: null,
  meanLearning: null,
  finalExploitability: null,
  match: null
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

function clearCharts() {
  for (const chart of Object.values(charts)) {
    if (chart) {
      chart.destroy();
    }
  }
  charts.seedLearning = null;
  charts.meanLearning = null;
  charts.finalExploitability = null;
  charts.match = null;
}

function renderRunOptions() {
  runSelect.innerHTML = "";
  state.runs.forEach((run) => {
    const option = document.createElement("option");
    option.value = run.id;
    option.textContent = `${run.id} (summary:${run.summary_rows}, metrics:${run.metrics_rows})`;
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

  if (charts.seedLearning) {
    charts.seedLearning.destroy();
  }
  if (charts.meanLearning) {
    charts.meanLearning.destroy();
  }
  if (charts.finalExploitability) {
    charts.finalExploitability.destroy();
  }

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
        legend: { display: true, position: "bottom" },
        title: { display: false }
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
      label: `${series.algorithm} CI`,
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
            filter: (item) => !item.text.includes("CI upper") && !item.text.endsWith(" CI")
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
  }
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

  if (charts.match) {
    charts.match.destroy();
  }

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

async function loadRun(runId) {
  state.runId = runId;
  setStatus(`Loading run ${runId}...`);

  const [summaryData, learningData, policyData] = await Promise.all([
    fetchJson(`/api/runs/${encodeURIComponent(runId)}/summary`),
    fetchJson(`/api/runs/${encodeURIComponent(runId)}/learning`),
    fetchJson(`/api/runs/${encodeURIComponent(runId)}/policies`)
  ]);

  state.summaryRows = summaryData.rows || [];
  state.learning = {
    by_algorithm: learningData.by_algorithm || [],
    by_seed: learningData.by_seed || []
  };
  state.policies = policyData.policies || [];

  renderSummary();
  renderLearningCharts();
  renderPolicyOptions();
  matchStats.textContent = "";
  if (charts.match) {
    charts.match.destroy();
    charts.match = null;
  }
  setStatus(`Loaded run ${runId}.`);
}

async function loadRunsAndMaybeSelectLatest() {
  setStatus("Loading run list...");
  const data = await fetchJson("/api/runs");
  state.runs = data.runs || [];
  renderRunOptions();

  if (state.runs.length === 0) {
    setStatus("No runs found. Set EXPERIMENTS_DIR or run experiments first.", true);
    clearCharts();
    summaryTbody.innerHTML = "";
    summaryStats.innerHTML = "";
    policyASelect.innerHTML = "";
    policyBSelect.innerHTML = "";
    return;
  }

  const selected = state.runId && state.runs.some((r) => r.id === state.runId)
    ? state.runId
    : state.runs[0].id;
  runSelect.value = selected;
  await loadRun(selected);
}

refreshRunsBtn.addEventListener("click", async () => {
  try {
    await loadRunsAndMaybeSelectLatest();
  } catch (error) {
    setStatus(error.message, true);
  }
});

loadRunBtn.addEventListener("click", async () => {
  try {
    const runId = runSelect.value;
    await loadRun(runId);
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

loadRunsAndMaybeSelectLatest().catch((error) => {
  setStatus(error.message, true);
});

