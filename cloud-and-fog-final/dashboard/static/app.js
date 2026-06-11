"use strict";

// ─── Config ──────────────────────────────────────────────────────────────────
const OVERVIEW_MS = 100;    // live data poll interval (map, KPIs, cards, health)
const HISTORY_MS  = 1000;   // trails + trend poll interval (heavier queries)
const TRAIL_SECONDS = 5;    // max age (s) of points kept in node movement trails

const PALETTE = ["#4f46e5", "#16a34a", "#d97706", "#9333ea", "#dc2626",
                 "#0891b2", "#db2777", "#ca8a04", "#2563eb", "#059669"];
const colorCache = {};
function colorFor(id) {
  if (!(id in colorCache)) colorCache[id] = PALETTE[Object.keys(colorCache).length % PALETTE.length];
  return colorCache[id];
}

// ─── Friendly labels ─────────────────────────────────────────────────────────
// Map a board's node_id (its MAC last byte, e.g. "0xAB") to a human-readable
// name. Edit this to taste — IDs not listed here just show their raw hex.
// Each board prints its own ID on boot: "Board ID (MAC last byte): 0x..".
// NB: keys are UPPERCASE hex to match how the IDs are stored (e.g. "0xAB").
const LABELS = {
  // "0x84": "Anchor 1",
  // "0xAB": "Anchor 2",
  // "0x01": "Node A",
};
function labelFor(id) {
  if (id == null) return id;
  return LABELS[id] || LABELS[String(id).toUpperCase()] || id;
}

const $ = (s) => document.querySelector(s);
const cssVar = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

// ─── State ───────────────────────────────────────────────────────────────────
let lastOverview = null;
let lastTracks   = {};
let trendChart   = null;
let sparkData    = { altitude: {}, pressure: {} };   // { field: { node_id: [values] } }

// ─── Theme ───────────────────────────────────────────────────────────────────
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme;
  localStorage.setItem("mesh-theme", theme);
  if (trendChart) { styleChart(); trendChart.update("none"); }
  drawMap();
  drawAllSparklines();
}
function initTheme() {
  const saved = localStorage.getItem("mesh-theme");
  const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
  applyTheme(saved || (prefersDark ? "dark" : "light"));
}
$("#theme-toggle").addEventListener("click", () =>
  applyTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark"));

// ─── Polling ─────────────────────────────────────────────────────────────────
async function fetchJSON(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

async function pollOverview() {
  try {
    lastOverview = await fetchJSON("/api/overview");
    setConn(true);
    renderKPIs(lastOverview);
    renderHealth(lastOverview);
    renderCards(lastOverview);
    drawMap();
  } catch (e) {
    setConn(false, "disconnected");
  }
}

async function pollHistory() {
  try {
    const data = await fetchJSON(`/api/history/positions?seconds=${TRAIL_SECONDS}`);
    lastTracks = $("#trails-toggle").checked ? trimTracks(data.tracks, TRAIL_SECONDS) : {};
    drawMap();

    const field = $("#trend-field").value, minutes = $("#trend-window").value;
    const trend = await fetchJSON(`/api/history/sensor?field=${field}&minutes=${minutes}`);
    updateTrend(trend);

    // Sparklines on the sensor cards — fixed short window, reuses the same API.
    const [altH, prsH] = await Promise.all([
      fetchJSON("/api/history/sensor?field=altitude&minutes=15"),
      fetchJSON("/api/history/sensor?field=pressure&minutes=15"),
    ]);
    sparkData.altitude = seriesToValues(altH.series);
    sparkData.pressure = seriesToValues(prsH.series);
    drawAllSparklines();
  } catch (e) { /* overview surfaces connection state */ }
}

// Reduce a {node: [{ts,value}]} series to {node: [value]} downsampled for sparklines.
function seriesToValues(series) {
  const out = {};
  for (const [id, pts] of Object.entries(series || {})) {
    let vals = pts.map((p) => p.value).filter((v) => v != null);
    const MAX = 80;
    if (vals.length > MAX) {
      const stride = Math.ceil(vals.length / MAX);
      vals = vals.filter((_, i) => i % stride === 0);
    }
    out[id] = vals;
  }
  return out;
}

// Keep only the last `seconds` of each track, measured from that track's own
// latest point (robust to any client/DB clock offset).
function trimTracks(tracks, seconds) {
  const out = {};
  const cutoffMs = seconds * 1000;
  for (const [id, pts] of Object.entries(tracks || {})) {
    if (!pts || !pts.length) { out[id] = pts || []; continue; }
    const latest = new Date(pts[pts.length - 1].ts).getTime();
    out[id] = pts.filter((p) => latest - new Date(p.ts).getTime() <= cutoffMs);
  }
  return out;
}

// ─── Header ──────────────────────────────────────────────────────────────────
function setConn(ok, msg) {
  $("#conn-dot").className = "dot " + (ok ? "dot-green" : "dot-red");
  $("#conn-text").textContent = ok ? "connected" : (msg || "disconnected");
}
function tick() { $("#clock").textContent = new Date().toLocaleTimeString(); }

// ─── KPIs ────────────────────────────────────────────────────────────────────
function renderKPIs(d) {
  const total   = d.health.length;
  const online  = d.health.filter((h) => h.online).length;
  const anchors = d.positions.filter((p) => p.is_anchor).length;
  $("#kpi-total").textContent   = total;
  $("#kpi-online").textContent  = online;
  $("#kpi-anchors").textContent = anchors;
  $("#kpi-mobile").textContent  = Math.max(total - anchors, 0);
}

// ─── Health ──────────────────────────────────────────────────────────────────
function fmtAge(s) {
  if (s == null) return "—";
  if (s < 60)   return `${s.toFixed(0)}s ago`;
  if (s < 3600) return `${(s / 60).toFixed(0)}m ago`;
  return `${(s / 3600).toFixed(1)}h ago`;
}
function renderHealth(d) {
  const body = $("#health-body");
  const posById = Object.fromEntries(d.positions.map((p) => [p.node_id, p]));
  const senById = Object.fromEntries(d.sensors.map((s) => [s.node_id, s]));
  const rows = [...d.health].sort((a, b) => a.node_id.localeCompare(b.node_id));

  if (!rows.length) {
    body.innerHTML = `<tr><td colspan="5" class="empty">No data yet — start the simulator or hardware bridge.</td></tr>`;
    return;
  }
  body.innerHTML = rows.map((h) => {
    const ref = posById[h.node_id] || senById[h.node_id] || {};
    const isAnchor = !!ref.is_anchor;
    const sen = senById[h.node_id];
    const peers = sen && sen.peers ? sen.peers.length : 0;
    const role = isAnchor ? `<span class="badge badge-anchor">anchor</span>`
                          : `<span class="badge badge-node">mobile</span>`;
    return `<tr>
      <td><span class="dot ${h.online ? "dot-green" : "dot-red"}"></span></td>
      <td class="mono">${labelFor(h.node_id)}</td>
      <td>${role}</td>
      <td>${fmtAge(h.age_s)}</td>
      <td class="mono">${peers}</td>
    </tr>`;
  }).join("");
}

// ─── Sensor cards ────────────────────────────────────────────────────────────
function renderCards(d) {
  const wrap = $("#cards");
  const online = new Set(d.health.filter((h) => h.online).map((h) => h.node_id));
  const sensors = [...d.sensors].sort((a, b) => a.node_id.localeCompare(b.node_id));
  if (!sensors.length) { wrap.innerHTML = `<div class="empty">No sensor data yet.</div>`; return; }

  const num = (v, dp = 1) => (v == null ? "—" : Number(v).toFixed(dp));
  wrap.innerHTML = sensors.map((s) => {
    const isOn = online.has(s.node_id);
    const role = s.is_anchor ? `<span class="badge badge-anchor card-role">anchor</span>`
                             : `<span class="badge badge-node card-role">mobile</span>`;
    return `<div class="card ${isOn ? "" : "offline"}">
      <div class="card-id"><span class="dot ${isOn ? "dot-green" : "dot-red"}"></span><span class="mono">${labelFor(s.node_id)}</span>${role}</div>
      <div class="metrics">
        <span class="k">Temp</span><span class="v">${num(s.temp)} °C</span>
        <span class="k">Pressure</span><span class="v">${num(s.pressure, 0)} Pa</span>
        <span class="k">Altitude</span><span class="v">${num(s.altitude, 2)} m</span>
        <span class="k">Heading</span><span class="v">${num(s.azimuth, 0)}°</span>
        <span class="k">Peers</span><span class="v">${s.peers ? s.peers.length : 0}</span>
      </div>
      <div class="sparks">
        <div class="spark"><span class="spark-label">Altitude</span><canvas class="spark-cv" data-node="${s.node_id}" data-field="altitude"></canvas></div>
        <div class="spark"><span class="spark-label">Pressure</span><canvas class="spark-cv" data-node="${s.node_id}" data-field="pressure"></canvas></div>
      </div>
    </div>`;
  }).join("");

  drawAllSparklines();   // canvases were just (re)created — paint them
}

// ─── Sparklines ──────────────────────────────────────────────────────────────
function drawAllSparklines() {
  document.querySelectorAll(".spark-cv").forEach((cv) => {
    const vals = (sparkData[cv.dataset.field] || {})[cv.dataset.node] || [];
    drawSparkline(cv, vals, colorFor(cv.dataset.node));
  });
}

function drawSparkline(canvas, values, color) {
  const { ctx, W, H } = fitCanvas(canvas);
  ctx.clearRect(0, 0, W, H);

  if (!values || values.length < 2) {
    ctx.strokeStyle = cssVar("--border"); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, H - 2); ctx.lineTo(W, H - 2); ctx.stroke();
    return;
  }

  let min = Math.min(...values), max = Math.max(...values);
  if (max === min) { max += 1; min -= 1; }
  const pad = 3;
  const xs = (i) => (i / (values.length - 1)) * W;
  const ys = (v) => H - pad - ((v - min) / (max - min)) * (H - 2 * pad);

  // soft area fill
  ctx.beginPath();
  values.forEach((v, i) => (i ? ctx.lineTo(xs(i), ys(v)) : ctx.moveTo(xs(i), ys(v))));
  ctx.lineTo(W, H); ctx.lineTo(0, H); ctx.closePath();
  ctx.fillStyle = color + "1f"; ctx.fill();

  // line
  ctx.beginPath();
  values.forEach((v, i) => (i ? ctx.lineTo(xs(i), ys(v)) : ctx.moveTo(xs(i), ys(v))));
  ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.lineJoin = "round"; ctx.stroke();

  // dot on the latest value
  const lx = xs(values.length - 1), ly = ys(values[values.length - 1]);
  ctx.fillStyle = color; ctx.beginPath(); ctx.arc(lx, ly, 1.8, 0, 2 * Math.PI); ctx.fill();
}

// ─── Position map (high-DPI canvas) ──────────────────────────────────────────
function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, W: w, H: h };
}

function drawMap() {
  const { ctx, W, H } = fitCanvas($("#map"));
  ctx.clearRect(0, 0, W, H);

  const muted = cssVar("--muted"), gridC = cssVar("--map-grid"),
        labelC = cssVar("--map-label"), textC = cssVar("--text"),
        anchorC = cssVar("--anchor");

  if (!lastOverview || !lastOverview.positions.length) {
    ctx.fillStyle = muted; ctx.font = "14px Inter, sans-serif"; ctx.textAlign = "center";
    ctx.fillText("Waiting for position data…", W / 2, H / 2);
    return;
  }

  const pts = lastOverview.positions;

  // Set of anchor node_ids — anchors are fixed, so they don't get trails.
  const anchorIdSet = new Set(pts.filter((p) => p.is_anchor).map((p) => p.node_id));

  // Base the view extent on the ANCHORS only (they're fixed). That way the map
  // doesn't rescale — and the anchors don't appear to shift — as mobile nodes
  // move around. Falls back to all points until at least 2 anchors are known.
  const anchorPts = pts.filter((p) => p.is_anchor && p.x != null && p.y != null);
  const extentPts = anchorPts.length >= 2 ? anchorPts : pts;

  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  const consider = (x, y) => {
    if (x == null || y == null) return;
    minX = Math.min(minX, x); maxX = Math.max(maxX, x);
    minY = Math.min(minY, y); maxY = Math.max(maxY, y);
  };
  extentPts.forEach((p) => consider(p.x, p.y));
  if (!isFinite(minX)) { minX = -1; maxX = 1; minY = -1; maxY = 1; }

  // Generous padding so mobile nodes have room around the fixed anchor frame.
  const padW = Math.max((maxX - minX) * 0.35, 0.8), padH = Math.max((maxY - minY) * 0.35, 0.8);
  minX -= padW; maxX += padW; minY -= padH; maxY += padH;

  const margin = 30, spanX = maxX - minX || 1, spanY = maxY - minY || 1;
  const scale = Math.min((W - 2 * margin) / spanX, (H - 2 * margin) / spanY);
  const offX = (W - spanX * scale) / 2, offY = (H - spanY * scale) / 2;
  const sx = (x) => offX + (x - minX) * scale;
  const sy = (y) => H - (offY + (y - minY) * scale);   // +Y = north = up

  // Grid
  let step = 1;
  while (spanX / step > 12) step *= 2;
  ctx.lineWidth = 1; ctx.strokeStyle = gridC; ctx.fillStyle = labelC;
  ctx.font = "10px Inter, sans-serif"; ctx.textAlign = "left";
  for (let gx = Math.ceil(minX / step) * step; gx <= maxX; gx += step) {
    ctx.beginPath(); ctx.moveTo(sx(gx), 0); ctx.lineTo(sx(gx), H); ctx.stroke();
    ctx.fillText(gx.toFixed(0), sx(gx) + 3, H - 5);
  }
  for (let gy = Math.ceil(minY / step) * step; gy <= maxY; gy += step) {
    ctx.beginPath(); ctx.moveTo(0, sy(gy)); ctx.lineTo(W, sy(gy)); ctx.stroke();
    ctx.fillText(gy.toFixed(0), 3, sy(gy) - 4);
  }

  // North indicator
  ctx.strokeStyle = muted; ctx.fillStyle = muted; ctx.lineWidth = 1.5;
  ctx.beginPath(); ctx.moveTo(W - 26, 42); ctx.lineTo(W - 26, 18); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(W - 30, 24); ctx.lineTo(W - 26, 15); ctx.lineTo(W - 22, 24); ctx.closePath(); ctx.fill();
  ctx.textAlign = "center"; ctx.font = "600 11px Inter, sans-serif"; ctx.fillText("N", W - 26, 55);

  // Trails — mobile nodes only; anchors are fixed and don't get one.
  Object.entries(lastTracks).forEach(([id, track]) => {
    if (anchorIdSet.has(id) || track.length < 2) return;
    ctx.strokeStyle = colorFor(id) + "55"; ctx.lineWidth = 2.5; ctx.lineJoin = "round";
    ctx.beginPath();
    track.forEach((q, i) => (i ? ctx.lineTo(sx(q.x), sy(q.y)) : ctx.moveTo(sx(q.x), sy(q.y))));
    ctx.stroke();
  });

  // Nodes
  pts.forEach((p) => {
    if (p.x == null || p.y == null) return;
    const X = sx(p.x), Y = sy(p.y), col = p.is_anchor ? anchorC : colorFor(p.node_id);

    if (p.azimuth != null && !p.is_anchor) {
      const a = (p.azimuth * Math.PI) / 180, len = 20;
      ctx.strokeStyle = col; ctx.lineWidth = 2.5; ctx.lineCap = "round";
      ctx.beginPath(); ctx.moveTo(X, Y); ctx.lineTo(X + Math.sin(a) * len, Y - Math.cos(a) * len); ctx.stroke();
    }

    ctx.shadowColor = col + "55"; ctx.shadowBlur = 8;
    ctx.fillStyle = col;
    if (p.is_anchor) { ctx.fillRect(X - 7, Y - 7, 14, 14); }
    else { ctx.beginPath(); ctx.arc(X, Y, 7, 0, 2 * Math.PI); ctx.fill(); }
    ctx.shadowBlur = 0;
    ctx.lineWidth = 2; ctx.strokeStyle = cssVar("--map-bg");
    if (p.is_anchor) ctx.strokeRect(X - 7, Y - 7, 14, 14);
    else { ctx.beginPath(); ctx.arc(X, Y, 7, 0, 2 * Math.PI); ctx.stroke(); }

    ctx.fillStyle = textC; ctx.font = "600 11px Inter, sans-serif"; ctx.textAlign = "left";
    ctx.fillText(labelFor(p.node_id), X + 11, Y - 7);
    ctx.fillStyle = labelC; ctx.font = "10px Inter, sans-serif";
    ctx.fillText(`${p.x.toFixed(1)}, ${p.y.toFixed(1)}`, X + 11, Y + 6);
  });
}

// ─── Trend chart (Chart.js) ──────────────────────────────────────────────────
function styleChart() {
  if (!trendChart) return;
  const text = cssVar("--muted"), grid = cssVar("--border"), legend = cssVar("--text");
  const o = trendChart.options;
  o.scales.x.ticks.color = text; o.scales.x.grid.color = grid;
  o.scales.y.ticks.color = text; o.scales.y.grid.color = grid;
  o.plugins.legend.labels.color = legend;
}

function initTrend() {
  const ctx = $("#trend").getContext("2d");
  trendChart = new Chart(ctx, {
    type: "line",
    data: { datasets: [] },
    options: {
      responsive: true, maintainAspectRatio: false, parsing: false,
      interaction: { mode: "nearest", axis: "x", intersect: false },
      animation: { duration: 250 },
      elements: { point: { radius: 0, hoverRadius: 4 }, line: { tension: 0.15, borderWidth: 2 } },
      scales: {
        x: { type: "linear", grid: { display: true },
             ticks: { maxTicksLimit: 6,
                      callback: (v) => new Date(v).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) } },
        y: { grid: { display: true }, ticks: {} },
      },
      plugins: {
        legend: { position: "bottom", labels: { usePointStyle: true, pointStyle: "circle", boxWidth: 8, padding: 14 } },
        tooltip: { callbacks: { title: (items) => new Date(items[0].parsed.x).toLocaleTimeString() } },
      },
    },
  });
  styleChart();
}

// Centered moving average over a single contiguous segment (no gaps inside).
function smoothSegment(seg) {
  if (seg.length < 3) return seg;
  const win = Math.min(41, Math.max(5, Math.round(seg.length / 40)));
  const half = Math.floor(win / 2);
  return seg.map((p, i) => {
    let sum = 0, n = 0;
    for (let j = Math.max(0, i - half); j <= Math.min(seg.length - 1, i + half); j++) {
      sum += seg[j].y; n++;
    }
    return { x: p.x, y: sum / n };
  });
}

// Convert a series to {x,y} points: split into segments at large time gaps so we
// never draw across dead time, optionally smoothing each segment.
function buildTrendData(points, smooth) {
  const raw = (points || [])
    .filter((d) => d.value != null)
    .map((d) => ({ x: new Date(d.ts).getTime(), y: d.value }))
    .sort((a, b) => a.x - b.x);
  if (raw.length < 3) return raw;

  const deltas = [];
  for (let i = 1; i < raw.length; i++) deltas.push(raw[i].x - raw[i - 1].x);
  deltas.sort((a, b) => a - b);
  const median = deltas[Math.floor(deltas.length / 2)] || 1000;
  const thresh = Math.max(median * 5, 15000);   // break gaps > 5x normal (min 15s)

  // Split into contiguous segments
  const segments = [];
  let cur = [raw[0]];
  for (let i = 1; i < raw.length; i++) {
    if (raw[i].x - raw[i - 1].x > thresh) { segments.push(cur); cur = []; }
    cur.push(raw[i]);
  }
  segments.push(cur);

  // Smooth each (within segment only) and join with null breaks between them
  const out = [];
  let prevLastX = null;
  segments.forEach((seg) => {
    const s = smooth ? smoothSegment(seg) : seg;
    if (prevLastX !== null) out.push({ x: (prevLastX + s[0].x) / 2, y: null });
    out.push(...s);
    prevLastX = s[s.length - 1].x;
  });
  return out;
}

function updateTrend(data) {
  const series = data.series || {};
  const ids = Object.keys(series).sort();
  const smooth = $("#trend-smooth").checked;
  trendChart.data.datasets = ids.map((id) => ({
    label: labelFor(id),
    data: buildTrendData(series[id], smooth),
    borderColor: colorFor(id),
    backgroundColor: colorFor(id) + "22",
    spanGaps: false,
    fill: false,
  }));
  styleChart();
  trendChart.update("none");
}

// ─── Boot ────────────────────────────────────────────────────────────────────
$("#trails-toggle").addEventListener("change", pollHistory);
$("#trend-field").addEventListener("change", pollHistory);
$("#trend-window").addEventListener("change", pollHistory);
$("#trend-smooth").addEventListener("change", pollHistory);
window.addEventListener("resize", () => { drawMap(); drawAllSparklines(); });

initTheme();
initTrend();
tick(); setInterval(tick, 1000);
pollOverview(); pollHistory();
setInterval(pollOverview, OVERVIEW_MS);
setInterval(pollHistory, HISTORY_MS);
