/* pipensx web companion SPA — game-store edition. Vanilla JS, no build step.
   Talks to the REST/SSE API served by src/app/web_server.cpp. */
"use strict";

const $ = (id) => document.getElementById(id);

/* ================= i18n ================= */
const I18N = {
  en: {
    "tabs.downloads": "Downloads", "tabs.catalog": "Catalog", "tabs.add": "Add", "tabs.settings": "Settings",
    "dash.speed": "Speed", "dash.queue": "Queue", "dash.remaining": "Remaining", "dash.storage": "SD storage",
    "dash.traffic": "Traffic",
    "downloads.search": "Search downloads…", "downloads.all": "All", "downloads.active": "Active",
    "downloads.pauseAll": "Pause all", "downloads.resumeAll": "Resume all", "downloads.clearDone": "Clear done",
    "downloads.alerts": "Alerts", "downloads.enableNotify": "Enable notifications",
    "downloads.empty": "No downloads yet.", "downloads.emptySub": "Add a magnet link or pick something from the catalog.",
    "downloads.browse": "Browse catalog",
    "sort.recent": "Recent", "sort.name": "Name", "sort.progress": "Progress", "sort.speed": "Speed", "sort.eta": "ETA",
    "catalog.title": "Game catalog", "catalog.search": "Search 4000+ games…",
    "catalog.allGenres": "All genres", "catalog.allYears": "All years",
    "catalog.anySize": "Any size", "catalog.small": "< 4 GiB", "catalog.medium": "4–10 GiB", "catalog.large": "> 10 GiB",
    "catalog.newest": "Newest", "catalog.biggest": "Biggest", "catalog.smallest": "Smallest",
    "catalog.recentlyAdded": "Recently added", "catalog.reset": "Reset",
    "add.magnet": "Magnet link", "add.magnetSub": "Paste a magnet URI — the Switch resolves it.",
    "add.install": "Install", "add.download": "Download only", "add.addMagnet": "Add magnet",
    "add.torrent": ".torrent file", "add.torrentSub": "Upload a .torrent — or drop it anywhere on this tab.",
    "add.drop": "Drop .torrent here or click to choose", "add.upload": "Upload torrent",
    "settings.appearance": "Appearance", "settings.theme": "Theme",
    "settings.dark": "Dark", "settings.light": "Light", "settings.auto": "System",
    "settings.language": "Language", "settings.downloads": "Downloads",
    "settings.concurrent": "Concurrent downloads", "settings.stream": "Stream selection",
    "settings.allFiles": "All files", "settings.packagesOnly": "Packages only",
    "settings.location": "Install location", "settings.network": "Network",
    "settings.catalog": "Catalog", "settings.catalogUrl": "Catalog source URL",
    "settings.refresh": "Refresh catalog on launch", "settings.save": "Save settings",
    "game.install": "Install", "game.download": "Download", "game.close": "Close",
    "game.developer": "Developer", "game.publisher": "Publisher", "game.genre": "Genre",
    "game.year": "Year", "game.size": "Size", "game.titleId": "Title ID",
    "game.multiplayer": "Multiplayer", "game.performance": "Notes", "game.hash": "Info hash",
    "game.peers": "Peers", "game.added": "Added",
    "task.moveUp": "↑", "task.moveDown": "↓", "task.top": "Top",
    "task.pause": "Pause", "task.resume": "Resume", "task.retry": "Retry",
    "task.verify": "Verify", "task.remove": "Remove",
    "msg.addedQueue": "Added to the Switch queue", "msg.resolving": "Resolving on the Switch…",
    "msg.saved": "Settings saved", "msg.cleared": "Completed tasks cleared",
  },
  ru: {
    "tabs.downloads": "Загрузки", "tabs.catalog": "Каталог", "tabs.add": "Добавить", "tabs.settings": "Настройки",
    "dash.speed": "Скорость", "dash.queue": "Очередь", "dash.remaining": "Осталось", "dash.storage": "Память SD",
    "dash.traffic": "Трафик",
    "downloads.search": "Поиск загрузок…", "downloads.all": "Все", "downloads.active": "Активные",
    "downloads.pauseAll": "Пауза всем", "downloads.resumeAll": "Продолжить все", "downloads.clearDone": "Убрать готовые",
    "downloads.alerts": "Оповещения", "downloads.enableNotify": "Включить уведомления",
    "downloads.empty": "Пока нет загрузок.", "downloads.emptySub": "Добавьте magnet-ссылку или выберите игру из каталога.",
    "downloads.browse": "Открыть каталог",
    "sort.recent": "Новые", "sort.name": "Название", "sort.progress": "Прогресс", "sort.speed": "Скорость", "sort.eta": "Осталось",
    "catalog.title": "Каталог игр", "catalog.search": "Поиск по 4000+ играм…",
    "catalog.allGenres": "Все жанры", "catalog.allYears": "Все годы",
    "catalog.anySize": "Любой размер", "catalog.small": "< 4 ГиБ", "catalog.medium": "4–10 ГиБ", "catalog.large": "> 10 ГиБ",
    "catalog.newest": "Сначала новые", "catalog.biggest": "Сначала большие", "catalog.smallest": "Сначала маленькие",
    "catalog.recentlyAdded": "Недавно добавлены", "catalog.reset": "Сброс",
    "add.magnet": "Magnet-ссылка", "add.magnetSub": "Вставьте magnet-ссылку — Switch сам её разберёт.",
    "add.install": "Установить", "add.download": "Только скачать", "add.addMagnet": "Добавить magnet",
    "add.torrent": ".torrent файл", "add.torrentSub": "Загрузите .torrent — или перетащите его на эту вкладку.",
    "add.drop": "Перетащите .torrent сюда или нажмите для выбора", "add.upload": "Загрузить torrent",
    "settings.appearance": "Оформление", "settings.theme": "Тема",
    "settings.dark": "Тёмная", "settings.light": "Светлая", "settings.auto": "Системная",
    "settings.language": "Язык", "settings.downloads": "Загрузки",
    "settings.concurrent": "Одновременных загрузок", "settings.stream": "Выбор файлов",
    "settings.allFiles": "Все файлы", "settings.packagesOnly": "Только пакеты",
    "settings.location": "Куда устанавливать", "settings.network": "Сеть",
    "settings.catalog": "Каталог", "settings.catalogUrl": "URL источника каталога",
    "settings.refresh": "Обновлять каталог при запуске", "settings.save": "Сохранить",
    "game.install": "Установить", "game.download": "Скачать", "game.close": "Закрыть",
    "game.developer": "Разработчик", "game.publisher": "Издатель", "game.genre": "Жанр",
    "game.year": "Год", "game.size": "Размер", "game.titleId": "Title ID",
    "game.multiplayer": "Мультиплеер", "game.performance": "Заметки", "game.hash": "Инфо-хеш",
    "game.peers": "Пиры", "game.added": "Добавлена",
    "task.moveUp": "↑", "task.moveDown": "↓", "task.top": "Вверх",
    "task.pause": "Пауза", "task.resume": "Продолжить", "task.retry": "Повторить",
    "task.verify": "Проверить", "task.remove": "Удалить",
    "msg.addedQueue": "Добавлено в очередь Switch", "msg.resolving": "Распознаётся на Switch…",
    "msg.saved": "Настройки сохранены", "msg.cleared": "Готовые задачи убраны",
  },
};
let lang = localStorage.getItem("pipensxLang") || ((navigator.language || "en").toLowerCase().startsWith("ru") ? "ru" : "en");
const t = (k) => (I18N[lang] && I18N[lang][k]) || I18N.en[k] || k;
function applyI18n() {
  document.documentElement.lang = lang;
  document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll("[data-i18n-ph]").forEach((el) => { el.placeholder = t(el.dataset.i18nPh); });
  $("lang-btn").textContent = lang.toUpperCase();
  $("alerts-state").textContent = alertsOn() ? "on" : "off";
}

/* ================= theme ================= */
function applyTheme() {
  const pref = localStorage.getItem("pipensxTheme") || "dark";
  const theme = pref === "auto"
    ? (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark") : pref;
  document.documentElement.dataset.theme = theme;
  $("theme-btn").textContent = theme === "dark" ? "🌙" : "☀️";
  const sel = $("set-theme");
  if (sel) sel.value = pref;
}
$("theme-btn").addEventListener("click", () => {
  const cur = localStorage.getItem("pipensxTheme") || "dark";
  localStorage.setItem("pipensxTheme", cur === "dark" ? "light" : "dark");
  applyTheme();
});
$("lang-btn").addEventListener("click", () => {
  lang = lang === "en" ? "ru" : "en";
  localStorage.setItem("pipensxLang", lang);
  applyI18n();
  renderDownloads();
  if (catalog) { buildCatalogFilters(); applyCatalogFilter(); }
  const sel = $("set-lang");
  if (sel) sel.value = lang;
});
matchMedia("(prefers-color-scheme: light)").addEventListener?.("change", applyTheme);

/* ---------- PIN ---------- */
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
    const pin = prompt(lang === "ru"
      ? "PIN (задаётся на консоли в Настройки → Web companion):"
      : "PIN (set on the console in Settings → Web companion):");
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
  if (n === null || n === undefined) return "?";
  const units = lang === "ru" ? ["Б", "КиБ", "МиБ", "ГиБ", "ТиБ"] : ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0, v = Number(n);
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return (v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)) + " " + units[i];
}
function fmtSpeed(bps) { return bps > 0 ? fmtBytes(bps) + "/s" : "—"; }
function fmtEta(seconds) {
  if (!seconds) return "";
  const s = Number(seconds);
  const m = lang === "ru" ? "м " : "m ", h = lang === "ru" ? "ч " : "h ",
        d = lang === "ru" ? "д " : "d ", sec = lang === "ru" ? "с" : "s";
  if (s < 60) return s + sec;
  const minutes = Math.floor(s / 60);
  if (minutes < 60) return minutes + m + (s % 60) + sec;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return hours + h + (minutes % 60) + m;
  return Math.floor(hours / 24) + d + (hours % 24) + h;
}
function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g,
    (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function fmtDate(ts) {
  if (!ts) return "";
  try { return new Date(Number(ts) * 1000).toLocaleDateString(lang === "ru" ? "ru-RU" : "en-US"); }
  catch { return ""; }
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
function closeModal() { const m = $("modal"); if (m.open) m.close(); }
function openModal(html) {
  const modal = $("modal");
  modal.innerHTML = html;
  if (!modal.open) modal.showModal();
  modal.onclick = (e) => { if (e.target === modal) closeModal(); };
}

/* ---------- tabs ---------- */
const tabs = ["downloads", "catalog", "add", "settings"];
function currentTab() {
  const h = location.hash.replace("#", "");
  return tabs.includes(h) ? h : "downloads";
}
function showTab() {
  const active = currentTab();
  for (const tt of tabs) {
    $("view-" + tt).hidden = tt !== active;
    document.querySelector(`.tab[data-tab="${tt}"]`).classList.toggle("active", tt === active);
  }
  if (active === "catalog") loadCatalog();
  if (active === "settings") loadSettings();
}
window.addEventListener("hashchange", showTab);
$("empty-browse")?.addEventListener("click", () => { location.hash = "#catalog"; });

/* ---------- live state (SSE) ---------- */
let state = { tasks: [], jobs: [], storage: null, summary: null, version: "" };
let lastStatuses = new Map();
let events = null;
const speedHist = []; // last N aggregate download speeds for the chart
const SPEED_HIST_N = 60;

function connectEvents() {
  if (events) events.close();
  events = new EventSource("/api/events");
  events.addEventListener("state", (e) => {
    $("conn").classList.add("on");
    $("conn-text").textContent = "online";
    state = JSON.parse(e.data);
    if (state.version) {
      $("ver-badge").hidden = false;
      $("ver-badge").textContent = "v" + state.version;
      $("foot-ver").textContent = "pipensx v" + state.version;
    }
    notifyTransitions();
    renderDownloads();
  });
  events.onerror = () => {
    $("conn").classList.remove("on");
    $("conn-text").textContent = "offline";
  };
}
connectEvents();

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

/* ---------- alerts ---------- */
function notifySupported() {
  return "Notification" in window && Notification.permission !== "denied";
}
function alertsOn() { return localStorage.getItem("pipensxAlerts") !== "off"; }
function refreshNotifyRow() {
  $("alerts-state").textContent = alertsOn() ? "on" : "off";
  $("notify-btn").hidden = !notifySupported() || Notification.permission !== "default";
}
$("alerts-btn").addEventListener("click", () => {
  localStorage.setItem("pipensxAlerts", alertsOn() ? "off" : "on");
  if (alertsOn()) { unlockAudio(); beep(); }
  refreshNotifyRow();
  applyI18n();
});
$("notify-btn").addEventListener("click", async () => {
  await Notification.requestPermission();
  refreshNotifyRow();
});

let audioCtx = null;
function unlockAudio() {
  if (!audioCtx) {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    audioCtx = new Ctx();
  }
  if (audioCtx.state === "suspended") audioCtx.resume();
}
document.addEventListener("pointerdown", unlockAudio, { once: true });
function beep(isError = false) {
  if (!audioCtx || audioCtx.state !== "running") return;
  const seq = isError ? [[220, 0, 0.12], [180, 0.16, 0.14]]
                      : [[660, 0, 0.09], [880, 0.11, 0.12]];
  for (const [freq, at, dur] of seq) {
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = "sine";
    osc.frequency.value = freq;
    const t0 = audioCtx.currentTime + at;
    gain.gain.setValueAtTime(0.0001, t0);
    gain.gain.exponentialRampToValueAtTime(0.12, t0 + 0.015);
    gain.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    osc.connect(gain).connect(audioCtx.destination);
    osc.start(t0);
    osc.stop(t0 + dur + 0.02);
  }
}
let flashTimer = null, badgedIcon = null;
function setFavicon(href) {
  const link = document.querySelector('link[rel="icon"]');
  if (link) link.href = href;
}
function buildBadgedIcon(done) {
  const img = new Image();
  img.src = "/icon-192.png";
  img.onload = () => {
    const c = document.createElement("canvas");
    c.width = c.height = 64;
    const g = c.getContext("2d");
    g.drawImage(img, 0, 0, 64, 64);
    g.beginPath(); g.arc(48, 16, 14, 0, Math.PI * 2);
    g.fillStyle = "#ff5d5d"; g.fill();
    badgedIcon = c.toDataURL("image/png");
    done(badgedIcon);
  };
}
function startFlash(message) {
  stopFlash();
  const base = "pipensx";
  let tick = false;
  document.title = message;
  flashTimer = setInterval(() => { tick = !tick; document.title = tick ? base : message; }, 1200);
  if (badgedIcon) setFavicon(badgedIcon);
  else buildBadgedIcon(setFavicon);
}
function stopFlash() {
  if (flashTimer) clearInterval(flashTimer);
  flashTimer = null;
  document.title = "pipensx";
  setFavicon("/icon-192.png");
}
document.addEventListener("visibilitychange", () => { if (!document.hidden) stopFlash(); });
window.addEventListener("focus", stopFlash);
function fireAlert(title, body, isError) {
  if (notifySupported() && Notification.permission === "granted") {
    new Notification(title, { body, icon: "/icon-192.png" });
    return;
  }
  if (!alertsOn()) return;
  beep(isError);
  if (navigator.vibrate) navigator.vibrate(isError ? [180, 90, 180] : [120]);
  startFlash((isError ? "✕ " : "✓ ") + body);
}
function notifyTransitions() {
  const interesting = { Installed: "installed", Completed: "downloaded", Error: "failed" };
  for (const tt of state.tasks) {
    const prev = lastStatuses.get(tt.id);
    if (prev && prev !== tt.status && interesting[tt.status]) {
      const what = tt.status === "Error" ? (tt.error || "failed") : interesting[tt.status];
      fireAlert("pipensx: " + tt.name, tt.name + " — " + what, tt.status === "Error");
    }
    lastStatuses.set(tt.id, tt.status);
  }
}

/* ================= DOWNLOADS ================= */
const activeSet = new Set(["Downloading", "Installing", "Committing", "Checking", "Verifying", "Fetching"]);
function healthOf(tt) {
  if (tt.status !== "Downloading") return null;
  if ((tt.peers || 0) === 0 && !(tt.speedBps > 0)) return { cls: "poor", label: lang === "ru" ? "нет пиров" : "no peers" };
  if (!(tt.speedBps > 0)) return { cls: "slow", label: lang === "ru" ? "стоит" : "stalled" };
  if ((tt.speedBps || 0) < 512 * 1024) return { cls: "slow", label: lang === "ru" ? "медленно" : "slow" };
  return { cls: "excellent", label: "● " + fmtSpeed(tt.speedBps) };
}

function taskCard(tt, queueIdx) {
  const wantedTotal = tt.wantedTotalBytes || 0;
  const wantedDone = wantedTotal ? Math.min(tt.wantedCompletedBytes || 0, wantedTotal) : 0;
  const downloadPct = wantedTotal
    ? Math.min(100, 100 * wantedDone / wantedTotal)
    : (tt.totalBytes ? Math.min(100, 100 * tt.completedBytes / tt.totalBytes) : 0);
  const installPct = tt.installTotalBytes ? Math.min(100, 100 * tt.installedBytes / tt.installTotalBytes) : null;
  const fetchPct = tt.fetchProgress != null ? Math.min(100, 100 * tt.fetchProgress) : null;
  const pct = tt.status === "Fetching" && fetchPct !== null ? fetchPct
    : (tt.status === "Installing" || tt.status === "Committing") && installPct !== null ? installPct : downloadPct;
  const barClass = tt.status === "Error" ? "err"
    : (tt.status === "Installed" || tt.status === "Completed") ? "ok" : "";

  const meta = [];
  if (tt.status === "Downloading") meta.push(fmtSpeed(tt.speedBps));
  if (tt.status === "Installing") meta.push(fmtSpeed(tt.installSpeedBps));
  if (tt.status === "Fetching" && fetchPct !== null) meta.push(`fetch ${fetchPct.toFixed(0)}%`);
  if (tt.status === "Installing" || tt.status === "Committing") {
    if (tt.installTotalBytes) meta.push(`${fmtBytes(tt.installedBytes)} / ${fmtBytes(tt.installTotalBytes)}`);
  } else if (wantedTotal) {
    meta.push(`${fmtBytes(wantedDone)} / ${fmtBytes(wantedTotal)}`);
  } else if (tt.totalBytes) {
    meta.push(`${fmtBytes(tt.completedBytes)} / ${fmtBytes(tt.totalBytes)}`);
  }
  const eta = fmtEta(tt.etaSeconds);
  if (eta) meta.push("ETA " + eta);
  if (activeSet.has(tt.status)) meta.push(`${tt.peers || 0} peers`);
  if (tt.mode === "install" && tt.packageCount) meta.push(`pkg ${tt.packagesInstalled}/${tt.packageCount}`);

  const h = healthOf(tt);
  const btn = (label, cmd, cls = "") =>
    `<button class="btn ${cls}" data-task="${esc(tt.id)}" data-cmd="${cmd}">${esc(label)}</button>`;
  const actions = [];
  if (["Downloading", "Checking", "Queued", "Fetching"].includes(tt.status)) actions.push(btn(t("task.pause"), "pause"));
  if (tt.status === "Paused") actions.push(btn(t("task.resume"), "resume", "btn-primary"));
  if (tt.status === "Error") actions.push(btn(t("task.retry"), "retry", "btn-primary"));
  if (tt.status === "Queued") {
    actions.push(`<button class="btn btn-sm" data-task="${esc(tt.id)}" data-cmd="move-up" title="move up">${t("task.moveUp")}</button>`);
    actions.push(`<button class="btn btn-sm" data-task="${esc(tt.id)}" data-cmd="move-down" title="move down">${t("task.moveDown")}</button>`);
    actions.push(btn(t("task.top"), "move-front"));
  }
  if (["Paused", "Completed", "Error"].includes(tt.status)) actions.push(btn(t("task.verify"), "verify"));
  actions.push(btn(t("task.remove"), "remove", "btn-danger"));

  return `<div class="card task" data-id="${esc(tt.id)}">
    <div class="task-head">
      <div class="task-name" title="${esc(tt.name || tt.id)}">${esc(tt.name || tt.id)}</div>
      <span class="badge ${esc(tt.status.toLowerCase())}">${esc(tt.status)}</span>
    </div>
    ${tt.status === "Queued" && queueIdx > 0 ? `<div class="task-sub"><span class="queue-pos">#${queueIdx} ${lang === "ru" ? "в очереди" : "in queue"}</span></div>` : ""}
    <div class="bar"><div class="bar-fill ${barClass}" style="width:${pct}%"></div></div>
    <div class="task-meta">${meta.map(esc).join('<span class="dot">·</span>')}${h ? `<span class="health ${h.cls}">${esc(h.label)}</span>` : ""}</div>
    ${tt.currentPackage ? `<div class="task-sub">${esc(tt.currentPackage)}</div>` : ""}
    ${tt.error ? `<div class="task-error">${esc(tt.error)}</div>` : ""}
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
      <div class="task-name">${esc(j.title || j.infoHash || j.jobId)}</div>
      <span class="badge ${esc(j.state)}">${j.state === "resolving" ? "Resolving" : esc(j.state)}</span>
    </div>
    <div class="task-meta">${esc(detail)}</div>
    ${cancellable ? `<div class="task-actions"><button class="btn" data-job="${esc(j.jobId)}">Cancel</button></div>` : ""}
  </div>`;
}

function filteredTasks() {
  const q = ($("task-search").value || "").trim().toLowerCase();
  const f = $("task-filter").value;
  const sort = $("task-sort").value;
  let list = state.tasks.slice();
  if (q) list = list.filter((x) => (x.name || x.id).toLowerCase().includes(q));
  if (f === "active") list = list.filter((x) => activeSet.has(x.status));
  else if (f !== "all") list = list.filter((x) => x.status === f);
  const prog = (x) => {
    const wt = x.wantedTotalBytes || x.totalBytes || 0;
    const wd = x.wantedTotalBytes ? (x.wantedCompletedBytes || 0) : (x.completedBytes || 0);
    return wt ? wd / wt : 0;
  };
  if (sort === "name") list.sort((a, b) => (a.name || "").localeCompare(b.name || ""));
  else if (sort === "progress") list.sort((a, b) => prog(b) - prog(a));
  else if (sort === "speed") list.sort((a, b) => (b.speedBps || 0) - (a.speedBps || 0));
  else if (sort === "eta") list.sort((a, b) => (a.etaSeconds || 1e12) - (b.etaSeconds || 1e12));
  return list;
}

function renderDownloads() {
  const taskIds = new Set(state.tasks.map((x) => x.id));
  const jobs = (state.jobs || []).filter((j) => !(j.state === "done" && taskIds.has(j.taskId)));
  $("jobs").innerHTML = jobs.map(jobCard).join("");
  const list = filteredTasks();
  let qi = 0;
  $("tasks").innerHTML = list.map((x) => taskCard(x, x.status === "Queued" ? ++qi : 0)).join("");
  $("tasks-empty").hidden = state.tasks.length > 0 || jobs.length > 0;

  const activeCount = state.tasks.filter((x) => activeSet.has(x.status)).length;
  const dlBadge = $("dl-count");
  if (activeCount > 0) { dlBadge.hidden = false; dlBadge.textContent = activeCount; }
  else dlBadge.hidden = true;

  // Dashboard: prefer server summary, fall back to local aggregate.
  const s = state.summary;
  const dlSpeed = s ? s.downloadSpeedBps : state.tasks.reduce((a, x) => a + (x.speedBps || 0), 0);
  const instSpeed = s ? s.installSpeedBps : state.tasks.reduce((a, x) => a + (x.installSpeedBps || 0), 0);
  const peers = state.tasks.reduce((a, x) => a + (x.peers || 0), 0);
  const totalSpeed = dlSpeed + instSpeed;
  $("stat-speed").textContent = totalSpeed > 0 ? fmtSpeed(totalSpeed) : "—";
  $("stat-peers").textContent = peers > 0 ? `${peers} peers · DHT` : (activeCount > 0 ? `${activeCount} active` : "idle");
  const queued = s ? s.queued : state.tasks.filter((x) => x.status === "Queued").length;
  const paused = s ? s.paused : state.tasks.filter((x) => x.status === "Paused").length;
  const errors = s ? s.errors : state.tasks.filter((x) => x.status === "Error").length;
  $("stat-queue").textContent = `${activeCount} / ${state.tasks.length}`;
  $("stat-queue-sub").textContent =
    (lang === "ru" ? `очередь ${queued} · пауза ${paused} · ошибки ${errors}` : `queued ${queued} · paused ${paused} · errors ${errors}`);
  const remaining = s ? s.remainingBytes : 0;
  $("stat-remaining").textContent = remaining > 0 ? fmtBytes(remaining) : (state.tasks.length ? "—" : "0 B");
  const eta = s ? s.etaSeconds : 0;
  $("stat-eta").textContent = eta ? "ETA " + fmtEta(eta) : "";

  if (state.storage && state.storage.available) {
    const used = state.storage.totalBytes - state.storage.freeBytes;
    $("storage-fill").style.width = (100 * used / state.storage.totalBytes).toFixed(1) + "%";
    $("stat-storage").textContent = fmtBytes(state.storage.freeBytes);
    $("storage-text").textContent =
      (lang === "ru" ? "свободно из " : "free of ") + fmtBytes(state.storage.totalBytes);
  } else {
    $("stat-storage").textContent = "—";
    $("storage-text").textContent = "—";
  }

  // Speed history chart.
  speedHist.push(totalSpeed);
  if (speedHist.length > SPEED_HIST_N) speedHist.shift();
  const showChart = speedHist.some((v) => v > 0);
  $("chart-card").hidden = !showChart;
  if (showChart) {
    drawChart();
    $("chart-legend").textContent = `peak ${fmtSpeed(Math.max(...speedHist))}`;
  }
  refreshNotifyRow();
}

function drawChart() {
  const cv = $("speed-chart");
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth || cv.parentElement.clientWidth - 36;
  const h = 64;
  cv.width = w * dpr; cv.height = h * dpr;
  const g = cv.getContext("2d");
  g.scale(dpr, dpr);
  g.clearRect(0, 0, w, h);
  const max = Math.max(...speedHist, 1);
  const n = speedHist.length;
  const grad = g.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, "rgba(0,195,227,.45)");
  grad.addColorStop(1, "rgba(0,195,227,.03)");
  g.beginPath();
  speedHist.forEach((v, i) => {
    const x = (i / Math.max(1, SPEED_HIST_N - 1)) * w;
    const y = h - 4 - (v / max) * (h - 10);
    i === 0 ? g.moveTo(x, y) : g.lineTo(x, y);
  });
  g.strokeStyle = "#00c3e3"; g.lineWidth = 2; g.stroke();
  g.lineTo(w, h); g.lineTo(0, h); g.closePath();
  g.fillStyle = grad; g.fill();
}

["task-search", "task-filter", "task-sort"].forEach((id) =>
  $(id).addEventListener("input", renderDownloads));
$("task-filter").addEventListener("change", renderDownloads);
$("task-sort").addEventListener("change", renderDownloads);

$("bulk-pause").addEventListener("click", async () => {
  const r = await api("/api/queue/pause-all", { method: "POST",
    headers: { "Content-Type": "application/json" }, body: "{}" });
  if (!r.ok && r.status !== 401) toast("pause-all failed", true);
});
$("bulk-resume").addEventListener("click", async () => {
  const r = await api("/api/queue/resume-all", { method: "POST",
    headers: { "Content-Type": "application/json" }, body: "{}" });
  if (!r.ok && r.status !== 401) toast("resume-all failed", true);
});
$("bulk-clear").addEventListener("click", async () => {
  const r = await postJson("/api/queue/clear-completed", { deleteData: false });
  if (r.ok) toast(t("msg.cleared"));
  else if (r.status !== 401) {
    const b = await r.json().catch(() => ({}));
    toast(b.error || "clear failed", true);
  }
});

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
  const found = state.tasks.find((x) => x.id === id);
  openModal(`<div class="modal-body">
      <h3>${t("task.remove")}?</h3>
      <div class="modal-sub">${esc(found ? (found.name || id) : id)}</div>
      <label class="modal-check"><input type="checkbox" id="rm-data"> ${lang === "ru" ? "Удалить и скачанные данные" : "Also delete downloaded data"}</label>
    </div>
    <div class="modal-actions">
      <button class="btn" id="rm-cancel">${t("game.close")}</button>
      <button class="btn btn-danger" id="rm-ok">${t("task.remove")}</button>
    </div>`);
  $("rm-cancel").onclick = closeModal;
  $("rm-ok").onclick = async () => {
    const resp = await postJson(`/api/tasks/${id}/remove`, { deleteData: $("rm-data").checked });
    closeModal();
    if (!resp.ok) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "remove failed", true);
    }
  };
}

/* ================= CATALOG ================= */
let catalog = null, catalogFiltered = [], catalogShown = 0;
const CATALOG_CHUNK = 48;

async function loadCatalog() {
  if (catalog) { renderCatalogCount(); return; }
  const status = $("catalog-status");
  status.hidden = false;
  status.innerHTML = `<div class="skel"></div>`;
  try {
    const resp = await api("/api/catalog");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    catalog = await resp.json();
    status.hidden = true;
    buildCatalogFilters();
    applyCatalogFilter();
  } catch (err) {
    catalog = null;
    status.innerHTML = "";
    status.hidden = false;
    status.textContent = "Failed to load the catalog: " + err.message;
  }
}

function uniqSorted(vals) {
  return [...new Set(vals.filter(Boolean))].sort((a, b) => String(a).localeCompare(String(b)));
}
function buildCatalogFilters() {
  const genres = uniqSorted(catalog.map((e) => (e.genre || "").split(/[,/;]/)[0].trim()));
  const years = uniqSorted(catalog.map((e) => e.year)).reverse();
  const gsel = $("flt-genre"), ysel = $("flt-year");
  const gcur = gsel.value, ycur = ysel.value;
  gsel.innerHTML = `<option value="">${esc(t("catalog.allGenres"))}</option>` +
    genres.slice(0, 60).map((g) => `<option value="${esc(g)}">${esc(g)}</option>`).join("");
  ysel.innerHTML = `<option value="">${esc(t("catalog.allYears"))}</option>` +
    years.slice(0, 40).map((y) => `<option value="${esc(y)}">${esc(y)}</option>`).join("");
  if ([...gsel.options].some((o) => o.value === gcur)) gsel.value = gcur;
  if ([...ysel.options].some((o) => o.value === ycur)) ysel.value = ycur;
  renderCatalogCount();
}
function renderCatalogCount() {
  const total = catalog ? catalog.length : 0;
  const shown = catalogFiltered.length || total;
  $("catalog-count").textContent = total
    ? (lang === "ru" ? `Показано ${shown} из ${total}` : `Showing ${shown} of ${total}`)
    : "";
}

function applyCatalogFilter() {
  const q = $("search").value.trim().toLowerCase();
  const genre = $("flt-genre").value;
  const year = $("flt-year").value;
  const size = $("flt-size").value;
  const sort = $("flt-sort").value;
  catalogFiltered = catalog.filter((e) => {
    if (q && !((e.title || "").toLowerCase().includes(q) ||
               (e.developer || "").toLowerCase().includes(q) ||
               (e.publisher || "").toLowerCase().includes(q))) return false;
    if (genre && !(e.genre || "").toLowerCase().startsWith(genre.toLowerCase()) &&
        !(e.genre || "").toLowerCase().includes(genre.toLowerCase())) return false;
    if (year && String(e.year) !== year) return false;
    if (size === "small" && !(e.size < 4 * 1024 ** 3)) return false;
    if (size === "medium" && !(e.size >= 4 * 1024 ** 3 && e.size <= 10 * 1024 ** 3)) return false;
    if (size === "large" && !(e.size > 10 * 1024 ** 3)) return false;
    return true;
  });
  if (sort === "title") catalogFiltered.sort((a, b) => (a.title || "").localeCompare(b.title || ""));
  else if (sort === "year-desc") catalogFiltered.sort((a, b) => String(b.year || "").localeCompare(String(a.year || "")));
  else if (sort === "size-desc") catalogFiltered.sort((a, b) => (b.size || 0) - (a.size || 0));
  else if (sort === "size-asc") catalogFiltered.sort((a, b) => (a.size || 0) - (b.size || 0));
  else if (sort === "recent") catalogFiltered.sort((a, b) => (b.publishedAt || 0) - (a.publishedAt || 0));
  catalogShown = 0;
  $("catalog-grid").innerHTML = "";
  renderCatalogCount();
  appendCatalogChunk();
}

function healthChip(e) {
  if (!e.health || e.health === "unknown") return "";
  const bad = ["dead", "replaced", "notRegistered", "noPeers", "metadataTimeout"].includes(e.health);
  return `<span class="game-health${bad ? " bad" : ""}">${e.health === "ok" ? "● healthy" : esc(e.health)}</span>`;
}

function appendCatalogChunk() {
  if (!catalogFiltered) return;
  const slice = catalogFiltered.slice(catalogShown, catalogShown + CATALOG_CHUNK);
  catalogShown += slice.length;
  const html = slice.map((e, i) => {
    const idx = catalogShown - slice.length + i;
    const sub = [e.year, e.size ? fmtBytes(e.size) : ""].filter(Boolean).join(" · ");
    const genreTop = (e.genre || "").split(/[,/;]/)[0].trim();
    return `<div class="game" data-idx="${idx}">
      <div class="game-cover-wrap">
        ${e.posterUrl
          ? `<img class="game-cover" loading="lazy" src="${esc(e.posterUrl)}" alt="" onerror="this.remove()">`
          : `<div class="game-cover-fallback">🎮</div>`}
        ${healthChip(e)}
        ${e.size ? `<span class="game-size">${esc(fmtBytes(e.size))}</span>` : ""}
      </div>
      <div class="game-body">
        ${genreTop ? `<div class="game-genre">${esc(genreTop)}</div>` : ""}
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
["flt-genre", "flt-year", "flt-size", "flt-sort"].forEach((id) =>
  $(id).addEventListener("change", () => { if (catalog) applyCatalogFilter(); }));
$("flt-reset").addEventListener("click", () => {
  $("search").value = "";
  $("flt-genre").value = ""; $("flt-year").value = "";
  $("flt-size").value = ""; $("flt-sort").value = "title";
  if (catalog) applyCatalogFilter();
});

$("catalog-grid").addEventListener("click", (e) => {
  const card = e.target.closest(".game");
  if (!card) return;
  const entry = catalogFiltered[Number(card.dataset.idx)];
  if (entry) openGameModal(entry);
});

function openGameModal(entry) {
  const chips = [
    entry.year ? `<span class="chip">${esc(entry.year)}</span>` : "",
    entry.genre ? `<span class="chip accent">${esc(entry.genre)}</span>` : "",
    entry.size ? `<span class="chip">${esc(fmtBytes(entry.size))}</span>` : "",
    entry.health === "ok" ? `<span class="chip ok">● healthy</span>`
      : entry.health && entry.health !== "unknown" ? `<span class="chip warn">${esc(entry.health)}</span>` : "",
    entry.peerCount ? `<span class="chip">${esc(entry.peerCount)} peers</span>` : "",
  ].filter(Boolean).join("");
  const rows = [
    [t("game.developer"), entry.developer],
    [t("game.publisher"), entry.publisher],
    [t("game.genre"), entry.genre],
    [t("game.year"), entry.year],
    [t("game.size"), entry.size ? fmtBytes(entry.size) : ""],
    [t("game.titleId"), entry.titleId],
    [t("game.multiplayer"), entry.multiplayer],
    [t("game.performance"), entry.performance],
    [t("game.added"), entry.publishedAt ? fmtDate(entry.publishedAt) : ""],
  ].filter(([, v]) => v).map(([k, v]) => `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`).join("");
  const shots = (entry.screenshots || []).map((u) =>
    `<img loading="lazy" src="${esc(u)}" alt="" onerror="this.remove()">`).join("");
  openModal(`
    ${entry.posterUrl ? `<div class="modal-hero"><img src="${esc(entry.posterUrl)}" alt="" onerror="this.remove()"><div class="modal-hero-fade"></div></div>` : ""}
    <div class="modal-body">
      <div class="modal-title-row">
        ${entry.posterUrl ? `<img class="modal-cover" src="${esc(entry.posterUrl)}" alt="" onerror="this.remove()">` : ""}
        <div><h3>${esc(entry.title)}</h3><div class="modal-badges">${chips}</div></div>
      </div>
      ${entry.description ? `<div class="modal-desc">${esc(entry.description)}</div>` : ""}
      ${rows ? `<table class="facts">${rows}</table>` : ""}
      ${entry.infoHash ? `<div class="hash">${t("game.hash")}: ${esc(entry.infoHash)}</div>` : ""}
      ${shots ? `<div class="shots">${shots}</div>` : ""}
    </div>
    <div class="modal-actions">
      <button class="btn" id="game-cancel">${t("game.close")}</button>
      <button class="btn" id="game-download">${t("game.download")}</button>
      <button class="btn btn-primary" id="game-install">${t("game.install")}</button>
    </div>`);
  $("game-cancel").onclick = closeModal;
  const start = (mode) => async () => {
    closeModal();
    const resp = await postJson("/api/add/catalog", { infoHash: entry.infoHash, mode });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      toast(t("msg.addedQueue"));
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
  if (!magnet) return toast("magnet:?…", true);
  $("magnet-btn").disabled = true;
  try {
    const resp = await postJson("/api/add/magnet", { magnet, mode: checkedMode("magnet-mode") });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("magnet-input").value = "";
      toast(t("msg.resolving"));
      location.hash = "#downloads";
    } else {
      toast(body.error || "add failed", true);
    }
  } finally {
    $("magnet-btn").disabled = false;
  }
});

async function uploadTorrentFile(file) {
  if (!file) return toast("*.torrent", true);
  $("torrent-btn").disabled = true;
  try {
    const resp = await api("/api/add/torrent?mode=" + checkedMode("torrent-mode"), {
      method: "POST",
      headers: { "Content-Type": "application/x-bittorrent" },
      body: file,
    });
    const body = await resp.json().catch(() => ({}));
    if (resp.ok) {
      $("torrent-file").value = "";
      $("drop-name").textContent = "";
      toast(t("msg.addedQueue"));
      location.hash = "#downloads";
    } else {
      toast(body.error || "upload failed", true);
    }
  } finally {
    $("torrent-btn").disabled = false;
  }
}
$("torrent-btn").addEventListener("click", () => uploadTorrentFile($("torrent-file").files[0]));
$("torrent-file").addEventListener("change", () => {
  $("drop-name").textContent = $("torrent-file").files[0]?.name || "";
});
const dz = $("dropzone");
["dragenter", "dragover"].forEach((ev) => dz.addEventListener(ev, (e) => { e.preventDefault(); dz.classList.add("over"); }));
["dragleave", "drop"].forEach((ev) => dz.addEventListener(ev, (e) => { e.preventDefault(); dz.classList.remove("over"); }));
dz.addEventListener("drop", (e) => {
  const f = e.dataTransfer.files?.[0];
  if (f) { $("drop-name").textContent = f.name; uploadTorrentFile(f); }
});
// Drop anywhere on the Add view.
$("view-add").addEventListener("dragover", (e) => e.preventDefault());
$("view-add").addEventListener("drop", (e) => {
  e.preventDefault();
  const f = e.dataTransfer.files?.[0];
  if (f && /\.torrent$/i.test(f.name)) uploadTorrentFile(f);
});

/* ---------- settings ---------- */
let settingsLoaded = false;
function syncAppearanceControls() {
  $("set-theme").value = localStorage.getItem("pipensxTheme") || "dark";
  $("set-lang").value = lang;
}
$("set-theme").addEventListener("change", () => {
  localStorage.setItem("pipensxTheme", $("set-theme").value);
  applyTheme();
});
$("set-lang").addEventListener("change", () => {
  lang = $("set-lang").value;
  localStorage.setItem("pipensxLang", lang);
  applyI18n();
  renderDownloads();
  if (catalog) { buildCatalogFilters(); applyCatalogFilter(); }
});

async function loadSettings() {
  syncAppearanceControls();
  const resp = await api("/api/settings");
  if (!resp.ok) {
    if (resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "failed to load settings", true);
    }
    return;
  }
  const s = await resp.json();
  $("set-max-active").value = String(s.maxActiveDownloads || 1);
  $("set-stream").value = s.streamSelection || "allFiles";
  $("set-install").value = s.installLocation || "sdCard";
  $("set-debrid").value = s.debridProvider || "torbox";
  $("set-torrserver").value = s.torrserverUrl || "";
  $("set-proxy").value = s.proxyUrl || "";
  $("set-catalog-url").value = s.catalogSourceUrl || "";
  $("set-catalog-refresh").checked = !!s.refreshCatalogOnLaunch;
  $("set-torbox").value = "";
  $("set-realdebrid").value = "";
  $("set-torbox").placeholder = s.torboxConfigured ? "••••••••" : "";
  $("set-realdebrid").placeholder = s.realdebridConfigured ? "••••••••" : "";
  settingsLoaded = true;
}
$("settings-save").addEventListener("click", async () => {
  if (!settingsLoaded) return toast("…", true);
  const patch = {
    maxActiveDownloads: Number($("set-max-active").value),
    streamSelection: $("set-stream").value,
    installLocation: $("set-install").value,
    debridProvider: $("set-debrid").value,
    torrserverUrl: $("set-torrserver").value.trim(),
    proxyUrl: $("set-proxy").value.trim(),
    catalogSourceUrl: $("set-catalog-url").value.trim(),
    refreshCatalogOnLaunch: $("set-catalog-refresh").checked,
  };
  const torbox = $("set-torbox").value;
  const realdebrid = $("set-realdebrid").value;
  if (torbox) patch.torboxApiKey = torbox;
  if (realdebrid) patch.realdebridApiKey = realdebrid;
  $("settings-save").disabled = true;
  try {
    const resp = await api("/api/settings", {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(patch),
    });
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) return toast(body.error || "save failed", true);
    toast(t("msg.saved"));
    await loadSettings();
  } finally {
    $("settings-save").disabled = false;
  }
});

/* ---------- boot ---------- */
applyTheme();
applyI18n();
syncAppearanceControls();
showTab();
refreshNotifyRow();
api("/api/tasks").then((r) => r.json()).then((s) => {
  state = Object.assign(state, s);
  renderDownloads();
}).catch(() => {});
window.addEventListener("resize", () => { if (!$("chart-card").hidden) drawChart(); });
