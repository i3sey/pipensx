/* pipensx web companion SPA. Vanilla JS, no build step.
   Talks to the REST/SSE API served by src/app/web_server.cpp. */
"use strict";

const $ = (id) => document.getElementById(id);

/* ---------- PIN ---------- */
// A QR scan lands with ?pin=…; remember it and strip it from the URL.
{
  const params = new URLSearchParams(location.search);
  const pin = params.get("pin");
  if (pin) {
    localStorage.setItem("pipensxPin", pin);
    history.replaceState(null, "", location.pathname + location.hash);
  }
}
const getPin = () => localStorage.getItem("pipensxPin") || "";

async function api(path, options = {}) {
  options.headers = Object.assign({}, options.headers);
  if (getPin()) options.headers["X-Pipensx-Pin"] = getPin();
  const resp = await fetch(path, options);
  if (resp.status === 401) {
    const pin = prompt("PIN (set on the console in Settings → Web companion):");
    if (pin) {
      localStorage.setItem("pipensxPin", pin);
      return api(path, options);
    }
  }
  return resp;
}

async function postJson(path, body) {
  return api(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

/* ---------- helpers ---------- */
function fmtBytes(n) {
  if (!n && n !== 0) return "?";
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  let v = Number(n);
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return (v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)) + " " + units[i];
}
function fmtSpeed(bps) { return bps > 0 ? fmtBytes(bps) + "/s" : "—"; }
function fmtEta(task) {
  if (!task.speedBps || task.totalBytes <= task.completedBytes) return "";
  const s = Math.round((task.totalBytes - task.completedBytes) / task.speedBps);
  if (s < 90) return s + "s";
  if (s < 5400) return Math.round(s / 60) + "m";
  return Math.round(s / 3600) + "h";
}
function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

let toastTimer = null;
function toast(message, isError = false) {
  const el = $("toast");
  el.textContent = message;
  el.classList.toggle("err", isError);
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

function closeModal() { $("modal").close(); }
function openModal(html) {
  const modal = $("modal");
  modal.innerHTML = html;
  modal.showModal();
  modal.addEventListener("click", (e) => { if (e.target === modal) closeModal(); },
                         { once: true });
}

/* ---------- tabs ---------- */
const tabs = ["downloads", "catalog", "add"];
function currentTab() {
  const h = location.hash.replace("#", "");
  return tabs.includes(h) ? h : "downloads";
}
function showTab() {
  const active = currentTab();
  for (const t of tabs) {
    $("view-" + t).hidden = t !== active;
    document.querySelector(`.tab[data-tab="${t}"]`)
      .classList.toggle("active", t === active);
  }
  if (active === "catalog") loadCatalog();
}
window.addEventListener("hashchange", showTab);

/* ---------- live state (SSE) ---------- */
let state = { tasks: [], jobs: [], storage: null };
let lastStatuses = new Map();
let events = null;

function connectEvents() {
  if (events) events.close();
  events = new EventSource("/api/events");
  events.addEventListener("state", (e) => {
    $("conn").classList.add("on");
    state = JSON.parse(e.data);
    notifyTransitions();
    renderDownloads();
  });
  events.onerror = () => { $("conn").classList.remove("on"); };
}
connectEvents();

// Free one of the two SSE slots while the tab naps in the background.
let hiddenSince = null;
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    hiddenSince = Date.now();
    setTimeout(() => {
      if (document.hidden && hiddenSince && Date.now() - hiddenSince > 290000) {
        events?.close();
        events = null;
      }
    }, 300000);
  } else {
    hiddenSince = null;
    if (!events) connectEvents();
  }
});

/* ---------- notifications ---------- */
function notifySupported() { return "Notification" in window; }
function refreshNotifyRow() {
  $("notify-row").hidden =
    !notifySupported() || Notification.permission !== "default";
}
$("notify-btn").addEventListener("click", async () => {
  await Notification.requestPermission();
  refreshNotifyRow();
});
function notifyTransitions() {
  const interesting = { Installed: "installed", Completed: "downloaded", Error: "failed" };
  for (const t of state.tasks) {
    const prev = lastStatuses.get(t.id);
    if (prev && prev !== t.status && interesting[t.status] &&
        notifySupported() && Notification.permission === "granted") {
      new Notification("pipensx: " + t.name, {
        body: t.status === "Error" ? (t.error || "failed") : interesting[t.status],
        icon: "/icon-192.png",
      });
    }
    lastStatuses.set(t.id, t.status);
  }
}

/* ---------- downloads view ---------- */
const activeStatuses = ["Downloading", "Installing", "Committing", "Checking", "Verifying"];

function taskCard(t) {
  const pct = t.totalBytes ? Math.min(100, 100 * t.completedBytes / t.totalBytes) : 0;
  const installPct = t.installTotalBytes
    ? Math.min(100, 100 * t.installedBytes / t.installTotalBytes) : null;
  const active = activeStatuses.includes(t.status);
  const barClass = t.status === "Error" ? "err"
    : (t.status === "Installed" || t.status === "Completed") ? "ok" : "";

  const meta = [];
  if (active) meta.push(fmtSpeed(t.speedBps));
  if (t.totalBytes) meta.push(`${fmtBytes(t.completedBytes)} / ${fmtBytes(t.totalBytes)}`);
  const eta = fmtEta(t);
  if (eta && t.status === "Downloading") meta.push("ETA " + eta);
  if (active) meta.push(`${t.peers} peers`);
  if (t.mode === "install" && t.packageCount)
    meta.push(`pkg ${t.packagesInstalled}/${t.packageCount}`);
  if (t.status === "Installing" && installPct !== null)
    meta.push(`install ${installPct.toFixed(0)}%`);

  const btn = (label, cmd, cls = "") =>
    `<button class="btn ${cls}" data-task="${t.id}" data-cmd="${cmd}">${label}</button>`;
  const actions = [];
  if (["Downloading", "Checking", "Queued"].includes(t.status)) actions.push(btn("Pause", "pause"));
  if (t.status === "Paused") actions.push(btn("Resume", "resume", "btn-primary"));
  if (t.status === "Error") actions.push(btn("Retry", "retry", "btn-primary"));
  if (t.status === "Queued") actions.push(btn("Move up", "move-front"));
  if (["Paused", "Completed", "Error"].includes(t.status)) actions.push(btn("Verify", "verify"));
  actions.push(btn("Remove", "remove", "btn-danger"));

  return `<div class="card task" data-id="${t.id}">
    <div class="task-head">
      <div class="task-name">${esc(t.name || t.id)}</div>
      <span class="badge ${t.status.toLowerCase()}">${t.status}</span>
    </div>
    <div class="bar"><div class="bar-fill ${barClass}" style="width:${pct}%"></div></div>
    <div class="task-meta">${meta.map(esc).join("<span>·</span>")}</div>
    ${t.error ? `<div class="task-error">${esc(t.error)}</div>` : ""}
    <div class="task-actions">${actions.join("")}</div>
  </div>`;
}

function jobCard(j) {
  const stageText = { findingPeers: "Finding peers", connecting: "Connecting",
    fetchingMetadata: "Fetching metadata", validating: "Validating" }[j.stage] || j.stage;
  const detail = j.state === "resolving"
    ? stageText + (j.peerCount ? ` (peer ${j.peerIndex}/${j.peerCount})` : "")
    : j.state === "error" ? (j.error || "failed") : j.state;
  const cancellable = ["queued", "resolving"].includes(j.state);
  return `<div class="card task">
    <div class="task-head">
      <div class="task-name">${esc(j.title)}</div>
      <span class="badge ${j.state}">${j.state === "resolving" ? "Resolving" : j.state}</span>
    </div>
    <div class="task-meta">${esc(detail)}</div>
    ${cancellable
      ? `<div class="task-actions"><button class="btn" data-job="${j.jobId}">Cancel</button></div>`
      : ""}
  </div>`;
}

function renderDownloads() {
  // Terminal "done" jobs whose task is already in the list are noise.
  const taskIds = new Set(state.tasks.map((t) => t.id));
  const jobs = state.jobs.filter((j) => !(j.state === "done" && taskIds.has(j.taskId)));
  $("jobs").innerHTML = jobs.map(jobCard).join("");
  $("tasks").innerHTML = state.tasks.map(taskCard).join("");
  $("tasks-empty").hidden = state.tasks.length > 0 || jobs.length > 0;

  if (state.storage && state.storage.available) {
    $("storage").hidden = false;
    const used = state.storage.totalBytes - state.storage.freeBytes;
    $("storage-fill").style.width =
      (100 * used / state.storage.totalBytes).toFixed(1) + "%";
    $("storage-text").textContent =
      `SD: ${fmtBytes(state.storage.freeBytes)} free of ${fmtBytes(state.storage.totalBytes)}`;
  }
  refreshNotifyRow();
}

document.addEventListener("click", async (e) => {
  const cmdBtn = e.target.closest("[data-cmd]");
  if (cmdBtn) {
    const id = cmdBtn.dataset.task;
    const cmd = cmdBtn.dataset.cmd;
    if (cmd === "remove") return confirmRemove(id);
    const resp = await api(`/api/tasks/${id}/${cmd}`, { method: "POST" });
    if (!resp.ok && resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || `${cmd} failed`, true);
    }
    return;
  }
  const jobBtn = e.target.closest("[data-job]");
  if (jobBtn) await api(`/api/jobs/${jobBtn.dataset.job}/cancel`, { method: "POST" });
});

function confirmRemove(id) {
  const t = state.tasks.find((x) => x.id === id);
  openModal(`<div class="modal-body">
      <h3>Remove download?</h3>
      <div class="modal-sub">${esc(t ? t.name : id)}</div>
      <label class="modal-check">
        <input type="checkbox" id="rm-data"> Also delete downloaded data
      </label>
    </div>
    <div class="modal-actions">
      <button class="btn" id="rm-cancel">Cancel</button>
      <button class="btn btn-danger" id="rm-ok">Remove</button>
    </div>`);
  $("rm-cancel").onclick = closeModal;
  $("rm-ok").onclick = async () => {
    const resp = await postJson(`/api/tasks/${id}/remove`,
                                { deleteData: $("rm-data").checked });
    closeModal();
    if (!resp.ok) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "remove failed", true);
    }
  };
}

/* ---------- catalog ---------- */
let catalog = null;
let catalogFiltered = [];
let catalogShown = 0;
const CATALOG_CHUNK = 60;

async function loadCatalog() {
  if (catalog) return;
  const status = $("catalog-status");
  status.hidden = false;
  status.textContent = "Loading catalog…";
  try {
    const resp = await api("/api/catalog");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    catalog = await resp.json();
    status.hidden = true;
    applyCatalogFilter();
  } catch (err) {
    catalog = null;
    status.textContent = "Failed to load the catalog: " + err.message;
  }
}

function applyCatalogFilter() {
  const q = $("search").value.trim().toLowerCase();
  catalogFiltered = !q
    ? catalog
    : catalog.filter((e) => e.title.toLowerCase().includes(q));
  catalogShown = 0;
  $("catalog-grid").innerHTML = "";
  appendCatalogChunk();
}

function appendCatalogChunk() {
  if (!catalogFiltered) return;
  const slice = catalogFiltered.slice(catalogShown, catalogShown + CATALOG_CHUNK);
  catalogShown += slice.length;
  const html = slice.map((e, i) => {
    const idx = catalogShown - slice.length + i;
    const sub = [e.year, e.size ? fmtBytes(e.size) : ""].filter(Boolean).join(" · ");
    return `<div class="game" data-idx="${idx}">
      ${e.posterUrl
        ? `<img class="game-cover" loading="lazy" src="${esc(e.posterUrl)}" alt="" onerror="this.style.visibility='hidden'">`
        : `<div class="game-cover"></div>`}
      <div class="game-body">
        <div class="game-title">${esc(e.title)}</div>
        <div class="game-sub">${esc(sub)}</div>
      </div>
    </div>`;
  }).join("");
  $("catalog-grid").insertAdjacentHTML("beforeend", html);
}

new IntersectionObserver((entries) => {
  if (entries[0].isIntersecting) appendCatalogChunk();
}).observe($("catalog-sentinel"));

let searchTimer = null;
$("search").addEventListener("input", () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => { if (catalog) applyCatalogFilter(); }, 150);
});

$("catalog-grid").addEventListener("click", (e) => {
  const card = e.target.closest(".game");
  if (!card) return;
  const entry = catalogFiltered[Number(card.dataset.idx)];
  if (entry) openGameModal(entry);
});

function openGameModal(entry) {
  const sub = [entry.year, entry.genre, entry.publisher,
               entry.size ? fmtBytes(entry.size) : ""].filter(Boolean).join(" · ");
  openModal(`<div class="modal-body">
      ${entry.posterUrl ? `<img class="modal-cover" src="${esc(entry.posterUrl)}" alt="">` : ""}
      <h3>${esc(entry.title)}</h3>
      <div class="modal-sub">${esc(sub)}</div>
      ${entry.description ? `<div class="modal-desc">${esc(entry.description)}</div>` : ""}
    </div>
    <div class="modal-actions">
      <button class="btn" id="game-cancel">Close</button>
      <button class="btn" id="game-download">Download</button>
      <button class="btn btn-primary" id="game-install">Install</button>
    </div>`);
  $("game-cancel").onclick = closeModal;
  const start = (mode) => async () => {
    closeModal();
    const resp = await postJson("/api/add/catalog",
                                { infoHash: entry.infoHash, mode });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      toast("Added to the Switch queue");
      location.hash = "#downloads";
    } else {
      toast(body.error || "add failed", true);
    }
  };
  $("game-install").onclick = start("install");
  $("game-download").onclick = start("download");
}

/* ---------- add tab ---------- */
function checkedMode(groupId) {
  return document.querySelector(`#${groupId} input:checked`).value;
}

$("magnet-btn").addEventListener("click", async () => {
  const magnet = $("magnet-input").value.trim();
  if (!magnet) return toast("Paste a magnet link first", true);
  $("magnet-btn").disabled = true;
  try {
    const resp = await postJson("/api/add/magnet",
                                { magnet, mode: checkedMode("magnet-mode") });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("magnet-input").value = "";
      toast("Resolving on the Switch…");
      location.hash = "#downloads";
    } else {
      toast(body.error || "add failed", true);
    }
  } finally {
    $("magnet-btn").disabled = false;
  }
});

$("torrent-btn").addEventListener("click", async () => {
  const file = $("torrent-file").files[0];
  if (!file) return toast("Choose a .torrent file first", true);
  $("torrent-btn").disabled = true;
  try {
    const resp = await api(
      "/api/add/torrent?mode=" + checkedMode("torrent-mode"),
      { method: "POST",
        headers: { "Content-Type": "application/x-bittorrent" },
        body: file });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("torrent-file").value = "";
      toast("Added to the Switch queue");
      location.hash = "#downloads";
    } else {
      toast(body.error || "upload failed", true);
    }
  } finally {
    $("torrent-btn").disabled = false;
  }
});

/* ---------- boot ---------- */
showTab();
refreshNotifyRow();
api("/api/tasks").then((r) => r.json()).then((s) => {
  state = s;
  renderDownloads();
}).catch(() => {});
