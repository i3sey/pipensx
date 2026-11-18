"use strict";

const $ = id => document.getElementById(id);
const translations = window.PIPENSX_I18N;
const languages = { en: "English", ru: "Русский", "pt-BR": "Português (Brasil)", fr: "Français", es: "Español", it: "Italiano", "zh-CN": "简体中文", ko: "한국어" };
const browserLocale = navigator.language.toLowerCase();
let lang = localStorage.getItem("pipensxLang") ||
  (browserLocale.startsWith("pt") ? "pt-BR" : browserLocale.startsWith("zh") ? "zh-CN" : browserLocale.split("-")[0]);
if (!languages[lang]) lang = "en";
const t = (key, ...args) => (translations[lang]?.[key] || translations.en[key] || key)
  .replace(/\{(\d+)\}/g, (_, i) => args[i] ?? "");
const esc = value => String(value ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const bytes = value => {
  let n = Number(value) || 0, i = 0;
  while (n >= 1024 && i < 4) { n /= 1024; i++; }
  return new Intl.NumberFormat(lang, { maximumFractionDigits: i ? 1 : 0 }).format(n) + " " +
    (lang === "ru" ? ["Б", "КиБ", "МиБ", "ГиБ", "ТиБ"] : ["B", "KiB", "MiB", "GiB", "TiB"])[i];
};
const speed = value => bytes(value) + (lang === "ru" ? "/с" : "/s");
const activeStates = new Set(["Checking", "Fetching", "Downloading", "Installing", "Committing", "Verifying", "Removing"]);
const pages = ["downloads", "catalog", "library", "settings", "maintenance", "add"];
let state = { tasks: [], jobs: [], summary: {}, storage: {}, source: {} };
let stateLoaded = false, features = {}, online = null, restarting = false;
let catalog = null, catalogBusy = false, catalogSection = "games", favoritesOnly = false;
let catalogGeneration = null;
let favorites = new Set(), selected = new Set(), filteredCatalog = [], shown = 0;
let library = null, libraryBusy = false, settings = null, logText = "", logBusy = false;
let updateStatus = null, updateBusy = false, toastTimer, pendingPin, navigating = false;
let instanceId = null;
let storageSnapshot = null, storageBusy = false;
const busyTasks = new Set();
const seenLibraryJobs = new Set();

function translate(root = document) {
  document.documentElement.lang = lang;
  root.querySelectorAll("[data-i18n]").forEach(el => { el.textContent = t(el.dataset.i18n); });
}
function applyTheme() {
  const pref = localStorage.getItem("pipensxTheme") || "auto";
  document.documentElement.dataset.theme = pref === "auto"
    ? (matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark") : pref;
  $("browser-theme").value = pref;
}
function toast(message, error = false) {
  $("toast").textContent = message;
  $("toast").classList.toggle("error", error);
  $("toast").hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { $("toast").hidden = true; }, error ? 9000 : 4500);
}
function errorText(error) {
  if (error instanceof TypeError) return t("web.networkError");
  return error.message || t("web.failed");
}
async function busy(button, action, status) {
  if (button?.disabled) return;
  if (button) button.disabled = true;
  try { return await action(); }
  catch (error) {
    if (status) { status.textContent = errorText(error); status.classList.add("error"); }
    toast(errorText(error), true);
  } finally { if (button?.isConnected) button.disabled = false; }
}
function ask(title, message, pinMode = false, action = t("control.continue")) {
  const dialog = $("control-dialog");
  $("control-title").textContent = title;
  $("control-message").textContent = message;
  $("control-pin-field").hidden = !pinMode;
  $("control-pin").required = pinMode;
  $("control-pin").value = "";
  $("control-submit").textContent = action;
  return new Promise(resolve => {
    let result = false;
    const finish = () => {
      $("control-form").onsubmit = null;
      $("control-cancel").onclick = null;
      dialog.removeEventListener("close", finish);
      resolve(result);
    };
    $("control-form").onsubmit = event => {
      event.preventDefault();
      result = pinMode ? $("control-pin").value : true;
      dialog.close();
    };
    $("control-cancel").onclick = () => dialog.close();
    dialog.addEventListener("close", finish);
    dialog.showModal();
    if (pinMode) $("control-pin").focus();
  });
}
function requestPin() {
  if (!pendingPin) pendingPin = ask(t("control.pin"), t("control.pinHelp"), true)
    .then(pin => {
      if (pin) localStorage.setItem("pipensxPin", pin);
      return pin;
    }).finally(() => { pendingPin = null; });
  return pendingPin;
}
async function api(path, options = {}, retry = true) {
  const headers = new Headers(options.headers);
  const pin = localStorage.getItem("pipensxPin") || "";
  if (pin) headers.set("X-Pipensx-Pin", pin);
  let response;
  try { response = await fetch(path, { cache: "no-store", ...options, headers }); }
  catch (error) { setConnection(false); throw error; }
  setConnection(true);
  if (response.status === 401 && retry) {
    const saved = localStorage.getItem("pipensxPin");
    if ((saved && saved !== pin) || await requestPin()) return api(path, options, false);
  }
  if (!response.ok) {
    const data = await response.json().catch(() => ({}));
    const reason = data.error || "";
    const localized = translations[lang]?.["action." + reason];
    throw new Error(response.status === 401 ? t("control.invalidPin") :
      localized || (reason ? t("web.serverError", reason) : t("web.httpError", response.status)));
  }
  if (response.status === 204) return null;
  return options.raw ? response.text() : response.json();
}
const post = (path, body = {}) => api(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
function modal(title, body, actions = "") {
  const dialog = $("modal");
  dialog.innerHTML = '<div class="dialog-body"><div class="dialog-heading"><h2 id="modal-title">' + esc(title) +
    '</h2><button data-close>' + esc(t("game.close")) + '</button></div>' + body +
    (actions ? '<div class="dialog-actions">' + actions + "</div>" : "") + "</div>";
  dialog.querySelector("[data-close]").onclick = () => dialog.close();
  if (!dialog.open) dialog.showModal();
}
$("modal").addEventListener("click", event => { if (event.target === $("modal")) $("modal").close(); });

function closeMenu() {
  $("sidebar").classList.remove("open");
  $("menu-button").setAttribute("aria-expanded", "false");
  $("scrim").hidden = true;
  $("sidebar").inert = matchMedia("(max-width: 740px)").matches;
}
$("menu-button").onclick = () => {
  const open = !$("sidebar").classList.contains("open");
  $("sidebar").classList.toggle("open", open);
  $("sidebar").inert = !open;
  $("menu-button").setAttribute("aria-expanded", String(open));
  $("scrim").hidden = !open;
  if (open) $("sidebar").querySelector("a").focus();
};
$("scrim").onclick = closeMenu;
document.addEventListener("keydown", event => {
  if (event.key === "Escape" && $("sidebar").classList.contains("open")) { closeMenu(); $("menu-button").focus(); }
});
matchMedia("(max-width: 740px)").addEventListener("change", closeMenu);
function page() {
  const value = location.hash.slice(1);
  return pages.includes(value) ? value : null;
}
function showPage() {
  if (!page()) return;
  const current = page();
  for (const name of pages) $("view-" + name).hidden = current !== name;
  document.querySelectorAll("[data-page]").forEach(link => {
    if (link.dataset.page === current) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  });
  closeMenu();
  document.title = (current === "maintenance" ? t("web.maintenance") : current === "add" ? t("web.add") : t("tabs." + current)) + " · pipensx";
  if (navigating) { window.scrollTo(0, 0); $("main").focus({ preventScroll: true }); }
  navigating = true;
  if (current === "downloads") renderDownloads();
  if (current === "catalog") loadCatalog();
  if (current === "library") loadLibrary();
  if (current === "settings" && !settings) loadSettings();
  if (current === "maintenance") { loadUpdate(); loadLogs(); loadMaintenance(); }
}
window.addEventListener("hashchange", showPage);
function setConnection(connected) {
  online = connected;
  $("connection").dataset.online = String(connected);
  $("connection").textContent = t(connected ? "web.online" : "web.offline");
  $("connection-banner").hidden = connected && !restarting;
  $("connection-banner").textContent = t(restarting ? "web.restarting" : "web.disconnected");
}
function renderDevice() {
  $("device-version").textContent = state.version ? "v" + state.version : "";
  $("storage-summary").textContent = state.storage?.available ? t("web.freeSpace", bytes(state.storage.freeBytes)) : "";
  const providers = { torbox: "TorBox", realdebrid: "Real-Debrid", alldebrid: "AllDebrid", torrserver: "TorrServer" };
  $("source-name").textContent = state.source?.direct ? "BitTorrent" : providers[state.source?.provider] || "";
}
function acceptState(next) {
  state = next;
  stateLoaded = true;
  renderDevice();
  if (!page() && location.hash !== "#main") {
    history.replaceState(null, "", state.tasks.length || state.jobs.length ? "#downloads" : "#catalog");
    showPage();
  }
  if (page() === "downloads") renderDownloads();
  if (page() === "catalog" && catalog && catalogGeneration !== state.catalogGeneration) loadCatalog(true);
  const count = state.tasks.filter(task => activeStates.has(task.status)).length;
  $("dl-count").hidden = !count;
  $("dl-count").textContent = count;
}
async function reloadTasks() { acceptState(await api("/api/tasks")); }
const events = new EventSource("/api/events");
events.addEventListener("state", event => {
  try { acceptState(JSON.parse(event.data)); setConnection(true); }
  catch (error) { toast(errorText(error), true); }
});
events.onerror = () => setConnection(false);

function progress(task) {
  if ((task.status === "Installing" || task.status === "Committing") && task.installTotalBytes)
    return Math.min(100, task.installedBytes / task.installTotalBytes * 100);
  if (task.status === "Fetching") return Math.min(100, (task.fetchProgress || 0) * 100);
  const total = task.wantedTotalBytes || task.totalBytes;
  return total ? Math.min(100, (task.wantedTotalBytes ? task.wantedCompletedBytes : task.completedBytes) / total * 100) : 0;
}
function packageNotice(task) {
  const installing = task.status === "Installing" || task.status === "Committing";
  const recovery = task.recovery;
  const notices = [];
  if (recovery?.work === "reread" && recovery.reworkBytes > 0)
    notices.push(t(installing ? "package.processing" : "recovery.reread",
      bytes(recovery.reworkBytes), recovery.packagesInstalled));
  if (installing && /\.nsz$/i.test(task.currentPackage || ""))
    notices.push(t("package.nszHint"));
  return notices;
}
function taskMarkup(task) {
  const done = task.wantedTotalBytes ? task.wantedCompletedBytes : task.completedBytes;
  const total = task.wantedTotalBytes || task.totalBytes;
  const details = [bytes(done) + " / " + bytes(total)];
  if (activeStates.has(task.status)) details.push(t("speed.download") + ": " + speed(task.speedBps));
  if (task.status === "Installing" || task.status === "Committing") details.push(t("speed.install") + ": " + speed(task.installSpeedBps));
  if (task.seeding) details.push(t("web.seeding") + ": " + speed(task.uploadSpeedBps));
  if (task.etaSeconds) details.push(t("sort.eta") + ": " + Math.ceil(task.etaSeconds / 60) + " " + t("web.minutes"));
  const button = (command, key, cls = "") => '<button data-task="' + esc(task.id) + '" data-command="' + command + '" class="' + cls + '"' +
    (busyTasks.has(task.id) ? " disabled" : "") + ">" + esc(t(key)) + "</button>";
  const actions = [];
  for (const [command, key] of [["pause", "task.pause"], ["resume", task.status === "Error" ? "task.retry" : "task.resume"]])
    if (task.capabilities?.[command]?.allowed) actions.push(button(command, key));
  actions.push(button("details", "web.details"));
  return '<article class="task" data-state="' + esc(task.status) + '"><div class="task-heading"><button class="task-title" data-task="' + esc(task.id) +
    '" data-command="details">' + esc(task.name || task.id) + '</button><span class="task-state">' + esc(t("status." + task.status)) +
    '</span></div><progress max="100" value="' + progress(task) + '" aria-label="' + esc(t("sort.progress")) + '"></progress><div class="task-meta">' +
    details.map(value => "<span>" + esc(value) + "</span>").join("") + "</div>" +
    (task.currentPackage ? '<p class="muted">' + esc(task.currentPackage) + "</p>" : "") +
    packageNotice(task).map(notice => '<p class="muted">' + esc(notice) + "</p>").join("") +
    (task.error ? '<p class="task-error">' + esc(task.error) + "</p>" : "") +
    '<div class="task-actions">' + actions.join("") + "</div></article>";
}
function renderDownloads() {
  if (!stateLoaded) return;
  const query = $("downloads-search").value.trim().toLowerCase(), filter = $("task-filter").value;
  const list = state.tasks.filter(task => (task.name || task.id).toLowerCase().includes(query) &&
    (filter === "all" || (filter === "active" ? activeStates.has(task.status) : task.status === filter)));
  const sort = $("task-sort").value;
  if (sort === "name") list.sort((a, b) => (a.name || "").localeCompare(b.name || "", lang));
  if (sort === "speed") list.sort((a, b) => b.speedBps - a.speedBps);
  if (sort === "progress") list.sort((a, b) => progress(b) - progress(a));
  // Keep focused controls in place while SSE updates the rest of the queue.
  const focus = document.activeElement;
  const focusTask = focus?.dataset.task, focusCommand = focus?.dataset.command;
  $("tasks").innerHTML = list.map(taskMarkup).join("");
  if (focusTask) Array.from($("tasks").querySelectorAll("[data-task]")).find(el =>
    el.dataset.task === focusTask && el.dataset.command === focusCommand)?.focus({ preventScroll: true });
  const jobs = (state.jobs || []).filter(job => !(job.state === "done" && state.tasks.some(task => task.id === job.taskId)) && job.state !== "cancelled");
  $("jobs").innerHTML = jobs.map(job => '<article class="job"><strong>' + esc(job.title) + '</strong><p>' +
    esc(t("web.job." + job.state)) + (job.error ? ": " + esc(job.error) : "") + "</p>" +
    (["queued", "resolving", "importing"].includes(job.state) ? '<button data-cancel-job="' + esc(job.jobId) + '">' + esc(t("control.cancel")) + "</button>" : "") + "</article>").join("");
  $("downloads-status").hidden = list.length > 0 || jobs.length > 0;
  $("downloads-status").textContent = state.tasks.length ? t("web.noResults") : t("downloads.emptySub");
  const summary = state.summary || {};
  $("queue-summary").innerHTML = [
    ["speed.download", speed(summary.downloadSpeedBps)], ["speed.install", speed(summary.installSpeedBps)],
    ["dash.remaining", bytes(summary.remainingBytes)], ["dash.queue", state.tasks.length]
  ].map(([key, value]) => "<div><span>" + esc(t(key)) + "</span><strong>" + esc(value) + "</strong></div>").join("");
  $("bulk-pause").disabled = !state.tasks.some(task => task.capabilities?.pause?.allowed);
  $("bulk-resume").disabled = !state.tasks.some(task => task.capabilities?.resume?.allowed);
  const clearable = state.tasks.some(task => ["Completed", "Installed"].includes(task.status) && task.capabilities?.remove?.allowed);
  $("bulk-clear").disabled = $("bulk-clear-files").disabled = !clearable;
}
async function taskCommand(id, command, body = {}) {
  busyTasks.add(id);
  try { await post("/api/tasks/" + encodeURIComponent(id) + "/" + command, body); await reloadTasks(); }
  finally { busyTasks.delete(id); renderDownloads(); }
}
async function openTask(id) {
  const task = state.tasks.find(item => item.id === id);
  if (!task) return;
  const action = (command, key, cls = "") => '<button data-task="' + esc(id) + '" data-command="' + command + '" class="' + cls + '">' + esc(t(key)) + "</button>";
  const actions = [];
  if (features.ports && ["port", "download"].includes(task.mode)) actions.push(action("port-files", "port.files"));
  if (task.capabilities?.verify?.allowed) actions.push(action("verify", "task.verify"));
  if (task.capabilities?.move?.allowed) actions.push(action("move-up", "web.moveUp"), action("move-down", "web.moveDown"), action("move-front", "task.top"));
  if (task.capabilities?.remove?.allowed) actions.push(action("remove", "task.remove", "danger"));
  modal(task.name || id, '<p>' + esc(t("status." + task.status)) + "</p>" +
    (task.error ? '<p class="error">' + esc(task.error) + "</p>" : "") +
    '<div class="task-meta"><span>' + esc(t("game.peers")) + ": " + (task.peers || 0) +
    "</span><span>DHT: " + (task.dhtGood || 0) + "</span><span>" + esc(t("speed.install")) + ": " +
    (task.packagesInstalled || 0) + " / " + (task.packageCount || 0) + '</span></div><h3 class="file-heading">' +
    esc(t("web.files")) + '</h3><div id="task-files" role="status">' + esc(t("web.loading")) + "</div>", actions.join(""));
  try {
    const data = await api("/api/tasks/" + encodeURIComponent(id) + "/files");
    if (!$("task-files") || !$("modal").open) return;
    $("task-files").innerHTML = '<p class="muted">' + esc(data.rootPath) + "</p>" +
      (!data.complete ? "<p>" + esc(t("web.incompleteFiles")) + "</p>" : "") +
      (data.files.length ? '<ul class="file-list">' + data.files.map(file => "<li><strong>" + esc(file.path) +
        "</strong><small>" + esc(bytes(file.size)) + " · " + esc(t("web.file." + file.state)) + " · " +
        esc(t("web.action." + file.action)) + "</small>" +
        (file.destination || file.localPath ? "<small>" + esc(file.destination || file.localPath) + "</small>" : "") + "</li>").join("") + "</ul>" : "<p>" + esc(t("web.noResults")) + "</p>");
  } catch (error) { if ($("task-files")) $("task-files").textContent = errorText(error); }
}
async function removeTask(id) {
  const task = state.tasks.find(item => item.id === id);
  modal(t("task.remove"), "<p>" + esc(task?.name || id) + '</p><label><input id="remove-data" type="checkbox"><span>' + esc(t("remove.data")) + "</span></label>",
    '<button id="remove-confirm" class="danger">' + esc(t("task.remove")) + "</button>");
  $("remove-confirm").onclick = event => busy(event.currentTarget, async () => {
    await taskCommand(id, "remove", { deleteData: $("remove-data").checked });
    $("modal").close();
  });
}
document.addEventListener("click", event => {
  const button = event.target.closest("[data-command]");
  if (button) busy(button, async () => {
    const id = button.dataset.task, command = button.dataset.command;
    if (command === "details") return openTask(id);
    if (command === "port-files") return openPortEditor(id);
    if (command === "remove") return removeTask(id);
    await taskCommand(id, command);
    if ($("modal").open) await openTask(id);
  });
  const job = event.target.closest("[data-cancel-job]");
  if (job) busy(job, async () => { await post("/api/jobs/" + encodeURIComponent(job.dataset.cancelJob) + "/cancel"); await reloadTasks(); });
});
for (const name of ["pause", "resume"]) $("bulk-" + name).onclick = event => busy(event.currentTarget, async () => {
  await post("/api/queue/" + name + "-all"); await reloadTasks();
});
for (const [id, deleteData] of [["bulk-clear", false], ["bulk-clear-files", true]]) $(id).onclick = event => busy(event.currentTarget, async () => {
  const completed = state.tasks.filter(task => ["Completed", "Installed"].includes(task.status));
  const clearable = completed.filter(task => task.capabilities?.remove?.allowed);
  const skipped = completed.length - clearable.length;
  const message = deleteData ? t("downloads.clearFilesAsk", clearable.length,
    bytes(clearable.reduce((sum, task) => sum + (task.completedBytes || 0), 0)), skipped) : t("downloads.clearEntriesAsk", clearable.length, skipped);
  if (!await ask(t(deleteData ? "downloads.clearFiles" : "downloads.clearDone"), message)) return;
  const result = await post("/api/queue/clear-completed", { deleteData });
  toast(t(deleteData ? "msg.clearedFiles" : "msg.clearedEntries", result.cleared, result.skippedBusy));
  await reloadTasks();
});
$("downloads-refresh").onclick = event => busy(event.currentTarget, reloadTasks, $("downloads-status"));
for (const id of ["downloads-search", "task-filter", "task-sort"]) $(id).addEventListener(id.endsWith("search") ? "input" : "change", renderDownloads);

async function loadCatalog(force = false) {
  if (catalogBusy) return;
  if (catalog && !force) { renderCatalog(); return; }
  catalogBusy = true;
  $("catalog-status").hidden = false;
  $("catalog-status").textContent = t("web.loading");
  try {
    catalog = (await api("/api/catalog")).filter(entry => entry.section !== "autorun");
    catalogGeneration = state.catalogGeneration;
    if (features.favorites) favorites = new Set((await api("/api/favorites")).hashes);
    for (const [id, field, empty] of [["flt-genre", "genre", "catalog.allGenres"], ["flt-year", "year", "catalog.allYears"]]) {
      const previous = $(id).value;
      const values = [...new Set(catalog.map(entry => String(entry[field] || "")).filter(Boolean))].sort((a, b) => a.localeCompare(b, lang));
      if (field === "year") values.reverse();
      $(id).innerHTML = '<option value="">' + esc(t(empty)) + "</option>" + values.map(value => '<option value="' + esc(value) + '">' + esc(value) + "</option>").join("");
      if (values.includes(previous)) $(id).value = previous;
    }
    renderCatalog();
  } catch (error) {
    $("catalog-status").hidden = false;
    $("catalog-status").textContent = errorText(error);
  } finally { catalogBusy = false; }
}
function renderCatalog() {
  if (!catalog) return;
  const query = $("catalog-search").value.trim().toLowerCase(), genre = $("flt-genre").value, year = $("flt-year").value, size = $("flt-size").value;
  const section = catalog.filter(entry => favoritesOnly ? favorites.has(entry.infoHash) : entry.section === catalogSection);
  filteredCatalog = section.filter(entry => [entry.title, entry.developer, entry.publisher].some(value => String(value || "").toLowerCase().includes(query)) &&
    (!genre || entry.genre === genre) && (!year || String(entry.year) === year) &&
    (!size || (size === "small" ? entry.size < 4 * 1024 ** 3 : size === "medium" ? entry.size >= 4 * 1024 ** 3 && entry.size <= 10 * 1024 ** 3 : entry.size > 10 * 1024 ** 3)));
  const sort = $("flt-sort").value;
  filteredCatalog.sort((a, b) => sort === "recent" ? (b.publishedAt || 0) - (a.publishedAt || 0) : sort === "year" ? Number(b.year || 0) - Number(a.year || 0) :
    sort === "size" ? a.size - b.size : (a.title || "").localeCompare(b.title || "", lang));
  $("catalog-count").textContent = t("catalog.count", filteredCatalog.length, section.length);
  $("catalog-status").hidden = filteredCatalog.length > 0;
  $("catalog-status").textContent = t("web.noResults");
  $("catalog-grid").innerHTML = "";
  shown = 0;
  appendCatalog();
  renderSelection();
}
function appendCatalog() {
  const selectionMode = $("catalog-select-mode").checked;
  const chunk = filteredCatalog.slice(shown, shown + 48);
  shown += chunk.length;
  $("catalog-grid").insertAdjacentHTML("beforeend", chunk.map(entry =>
    '<article class="game" data-hash="' + esc(entry.infoHash) + '" data-selected="' + selected.has(entry.infoHash) + '"><button class="game-open" data-game="' +
    esc(entry.infoHash) + '"><span class="game-cover">' + (safeImage(entry.posterUrl) ? '<img src="' + esc(entry.posterUrl) +
    '" alt="" width="180" height="240" loading="lazy">' : '<span>' + esc(t("web.noCover")) + "</span>") +
    '</span><span class="game-name">' + esc(entry.title) + '</span><span class="game-meta">' +
    esc([entry.year, entry.size ? bytes(entry.size) : ""].filter(Boolean).join(" · ")) + "</span></button>" +
    (selectionMode ? '<label class="game-select"><input type="checkbox" data-select="' + esc(entry.infoHash) + '" aria-label="' +
      esc(t("web.selectTitle", entry.title)) + '"' + (selected.has(entry.infoHash) ? " checked" : "") + "></label>" : "") + "</article>").join(""));
  $("catalog-more").hidden = shown >= filteredCatalog.length;
}
function safeImage(url) {
  try { return ["http:", "https:"].includes(new URL(url, location.href).protocol) && Boolean(url); }
  catch { return false; }
}
document.addEventListener("error", event => {
  if (event.target instanceof HTMLImageElement) event.target.hidden = true;
}, true);
$("catalog-grid").onclick = event => {
  const button = event.target.closest("[data-game]");
  if (!button) return;
  if ($("catalog-select-mode").checked) {
    toggleSelection(button.dataset.game);
    renderCatalog();
  } else openGame(button.dataset.game);
};
$("catalog-grid").onchange = event => {
  const input = event.target.closest("[data-select]");
  if (input) {
    toggleSelection(input.dataset.select);
    input.closest(".game").dataset.selected = String(input.checked);
  }
};
function toggleSelection(hash) {
  if (selected.has(hash)) selected.delete(hash); else selected.add(hash);
  renderSelection();
}
function renderSelection() {
  $("catalog-batch").hidden = selected.size === 0;
  $("selected-count").textContent = t("web.selected", selected.size);
}
for (const section of ["games", "ports", "favorites"]) $("catalog-" + section).onclick = () => {
  favoritesOnly = section === "favorites";
  if (!favoritesOnly) catalogSection = section;
  for (const name of ["games", "ports", "favorites"]) $("catalog-" + name).setAttribute("aria-pressed", String(name === section));
  renderCatalog();
};
for (const id of ["catalog-search", "flt-genre", "flt-year", "flt-size", "flt-sort"]) $(id).addEventListener(id.endsWith("search") ? "input" : "change", renderCatalog);
$("catalog-select-mode").onchange = () => { if (!$("catalog-select-mode").checked) selected.clear(); renderCatalog(); };
$("catalog-more").onclick = appendCatalog;
$("catalog-refresh").onclick = event => busy(event.currentTarget, () => loadCatalog(true));
$("catalog-fetch").onclick = event => busy(event.currentTarget, async () => {
  const job = await post("/api/catalog/refresh");
  $("catalog-status").hidden = false;
  $("catalog-status").textContent = t("web.loading");
  await new Promise((resolve, reject) => {
    const poll = async () => {
      try {
        const data = await api("/api/library");
        const current = (data.jobs || []).find(item => item.jobId === job.jobId);
        if (current?.state === "error") throw new Error(current.error || t("web.failed"));
        if (current?.state === "done") { await loadCatalog(true); resolve(); }
        else setTimeout(poll, 2000);
      } catch (error) { reject(error); }
    };
    setTimeout(poll, 1000);
  });
}, $("catalog-status"));
$("flt-reset").onclick = () => {
  for (const id of ["catalog-search", "flt-genre", "flt-year", "flt-size"]) $(id).value = "";
  $("flt-sort").value = "title"; renderCatalog();
};
$("batch-clear").onclick = () => { selected.clear(); $("catalog-select-mode").checked = false; renderCatalog(); };
async function addCatalog(hash, mode) { return post("/api/add/catalog", { infoHash: hash, mode }); }
for (const mode of ["install", "download"]) $("batch-" + mode).onclick = event => busy(event.currentTarget, async () => {
  if (!await ask(t("game." + mode), t("web.batchAsk", selected.size))) return;
  const failed = [];
  for (const hash of [...selected]) {
    try { await addCatalog(hash, mode); selected.delete(hash); }
    catch (error) { failed.push((catalog.find(entry => entry.infoHash === hash)?.title || hash) + ": " + errorText(error)); }
  }
  renderCatalog();
  if (failed.length) modal(t("web.failed"), "<ul>" + failed.map(message => "<li>" + esc(message) + "</li>").join("") + "</ul>");
  else { toast(t("msg.addedQueue")); location.hash = "#downloads"; }
});
function openGame(hash) {
  const entry = catalog?.find(item => item.infoHash === hash);
  if (!entry) return;
  const facts = [["game.developer", entry.developer], ["game.publisher", entry.publisher], ["game.genre", entry.genre], ["game.year", entry.year],
    ["game.size", entry.size ? bytes(entry.size) : ""], ["game.titleId", entry.titleId], ["game.performance", entry.performance]];
  const screenshots = (entry.screenshots || []).filter(safeImage);
  modal(entry.title,
    '<div class="game-detail-top">' + (safeImage(entry.posterUrl) ? '<img class="detail-cover" src="' + esc(entry.posterUrl) + '" alt="" width="150" height="200">' : "") +
    '<dl class="facts">' + facts.filter(([, value]) => value).map(([key, value]) => "<dt>" + esc(t(key)) + "</dt><dd>" + esc(value) + "</dd>").join("") +
    '</dl></div><p class="description" id="game-description"></p>' +
    (screenshots.length ? '<div class="shots">' + screenshots.map((url, index) => '<button data-shot="' + index + '" aria-label="' + esc(t("web.screenshot", index + 1)) +
      '"><img src="' + esc(url) + '" width="640" height="360" alt="" loading="lazy"></button>').join("") + "</div>" : "") +
    '<details><summary>' + esc(t("web.details")) + "</summary><p>" + esc(hash) + "</p></details>",
    (features.favorites ? '<button id="game-favorite" aria-pressed="' + favorites.has(hash) + '">' + esc(t(favorites.has(hash) ? "web.unfavorite" : "web.favorite")) + "</button>" : "") +
    '<button id="game-files">' + esc(t("web.files")) + '</button><button id="game-download">' + esc(t("game.download")) + '</button><button class="primary" id="game-install">' + esc(t("game.install")) + "</button>");
  $("game-description").textContent = entry.description || "";
  const descriptionElement = $("game-description");
  api("/api/catalog/description?hash=" + encodeURIComponent(hash) + "&lang=" + encodeURIComponent(lang))
    .then(data => { if (descriptionElement.isConnected) descriptionElement.textContent = data.description; })
    .catch(() => {});
  for (const mode of ["download", "install"]) $("game-" + mode).onclick = event => busy(event.currentTarget, async () => {
    await addCatalog(hash, mode); $("modal").close(); toast(t("msg.addedQueue")); location.hash = "#downloads";
  });
  $("game-files").onclick = event => busy(event.currentTarget, async () => {
    openFileChooser(await post("/api/preview/catalog", { infoHash: hash }), true);
  });
  if ($("game-favorite")) $("game-favorite").onclick = event => {
    const button = event.currentTarget;
    busy(button, async () => {
    favorites = new Set((await post("/api/favorites", { infoHash: hash })).hashes);
    button.textContent = t(favorites.has(hash) ? "web.unfavorite" : "web.favorite");
    button.setAttribute("aria-pressed", String(favorites.has(hash)));
    renderCatalog();
    });
  };
  $("modal").querySelectorAll("[data-shot]").forEach(button => button.onclick = () => {
    modal(entry.title, '<img class="screenshot" src="' + esc(screenshots[Number(button.dataset.shot)]) +
      '" alt="' + esc(t("web.screenshot", Number(button.dataset.shot) + 1)) + '" width="1280" height="720">',
      '<button id="shot-back">' + esc(t("port.back")) + "</button>");
    $("shot-back").onclick = () => openGame(hash);
  });
}

function renderLibrary() {
  if (!library) return;
  const query = $("library-search").value.trim().toLowerCase(), filter = $("library-filter").value;
  const rows = library.titles.filter(row => (row.name + " " + row.titleId).toLowerCase().includes(query) &&
    (filter === "all" || (filter === "archived" ? row.isArchived :
      (filter === "ports" ? row.isPort : !row.isArchived && row.updateState === "update_available"))));
  const busyIds = new Set((library.jobs || []).filter(job => ["queued", "working"].includes(job.state)).map(job => job.titleId));
  $("library-status").textContent = t("library.count", rows.length, library.titles.length);
  $("library-list").innerHTML = rows.length ? rows.map(row => {
    const busyRow = busyIds.has(row.titleId);
    const kindLabel = t(row.isArchived ? "library.archived" : (row.isPort ? "catalog.ports" : "catalog.games"));
    const statusLine = row.isArchived
      ? esc(t("library.archivedStatus")) + (row.freedBytes ? " · " + esc(t("library.freedSize", bytes(row.freedBytes))) : "")
      : esc(t("library." + row.updateState)) + (row.latestVersion ? " · " + esc(row.latestVersion) : "") +
        (row.canArchive && row.freedBytes ? " · " + esc(t("library.freedSize", bytes(row.freedBytes))) : "");
    const metaParts = [];
    if (!row.isArchived) {
      metaParts.push(esc(t("library.dlc", row.dlcCount || 0)));
      if (row.hasLayeredFsMods) metaParts.push(esc(t("library.mods")));
      if (!row.isPort && !row.canArchive && row.archiveBlockReason === "no_source")
        metaParts.push(esc(t("library.archiveNoSource")));
    }
    const actions = row.isArchived
      ? '<button class="primary" data-library="' + esc(row.titleId) + '" data-library-action="restore"' + (busyRow ? " disabled" : "") + ">" + esc(t("library.restore")) + "</button>" +
        '<button class="danger" data-library="' + esc(row.titleId) + '" data-library-action="uninstall"' + (busyRow ? " disabled" : "") + ">" + esc(t("library.removeShortcut")) + "</button>"
      : (row.updateState === "update_available" && row.updateHash ? '<button data-library="' + esc(row.titleId) + '" data-library-action="update"' +
          (busyRow ? " disabled" : "") + ">" + esc(t("library.update")) + "</button>" : "") +
        (row.canArchive ? '<button data-library="' + esc(row.titleId) + '" data-library-action="archive"' +
          (busyRow ? " disabled" : "") + ">" + esc(t("library.archive")) + "</button>" : "") +
        '<button class="danger" data-library="' + esc(row.titleId) + '" data-library-action="uninstall"' + (busyRow ? " disabled" : "") + ">" + esc(t("library.uninstall")) + "</button>";
    return '<article class="task"><div class="task-heading"><h2>' + esc(row.name || row.titleId) +
      '</h2><span class="task-state">' + esc(kindLabel) + '</span></div><p class="muted">' +
      esc(row.titleId) + " · " + esc(row.displayVersion || row.version || t("library.unknown")) +
      "</p><p>" + statusLine + '</p>' +
      (metaParts.length ? '<p class="muted">' + metaParts.join(" · ") + "</p>" : "") +
      '<div class="task-actions">' + actions + "</div></article>";
  }).join("") : '<p class="empty">' + esc(t("library.empty")) + "</p>";
  $("library-jobs").innerHTML = (library.jobs || []).slice(-5).map(job => '<p class="' + (job.state === "error" ? "error" : "muted") + '">' +
    esc(t("library." + job.action)) + " · " + esc(t("library.job." + job.state)) + (job.error ? ": " + esc(job.error) : "") + "</p>").join("");
  $("library-update-all").disabled = !library.titles.some(row => !row.isArchived && row.updateState === "update_available" && row.updateHash && !row.hasLayeredFsMods);
}
async function loadLibrary() {
  if (libraryBusy) return;
  libraryBusy = true;
  if (!library) $("library-status").textContent = t("library.loading");
  try {
    library = await api("/api/library");
    renderLibrary();
    for (const job of library.jobs || []) if (["done", "error"].includes(job.state) && !seenLibraryJobs.has(job.jobId)) {
      seenLibraryJobs.add(job.jobId);
      toast(t("library.job." + job.state) + (job.error ? ": " + job.error : ""), job.state === "error");
    }
  } catch (error) { $("library-status").textContent = errorText(error); }
  finally { libraryBusy = false; }
}
$("library-list").onclick = event => {
  const button = event.target.closest("[data-library]");
  if (!button) return;
  busy(button, async () => {
    const row = library.titles.find(item => item.titleId === button.dataset.library);
    if (!row) return;
    const action = button.dataset.libraryAction;
    if (action === "restore") {
      await post("/api/library/" + encodeURIComponent(row.titleId) + "/restore", {});
      toast(t("library.restoreQueued"));
      location.hash = "#downloads";
      return;
    }
    const titleLabel = row.name || row.titleId;
    let promptTitle = t("library." + action);
    let message = "";
    if (action === "archive") {
      message = t("library.archiveAsk", titleLabel, bytes(row.installedBytes || 0), bytes(row.freedBytes || row.installedBytes || 0));
    } else if (action === "uninstall") {
      if (row.isArchived) {
        promptTitle = t("library.removeShortcut");
        message = t("library.removeShortcutAsk", titleLabel, row.titleId);
      } else {
        message = t("library.uninstallAsk", titleLabel, row.titleId);
        if (row.isPort) message += "\n\n" + t("web.portRemoval", row.deployedFiles, bytes(row.deployedBytes)) +
          ((row.wholeFolders || []).length ? "\n" + row.wholeFolders.join("\n") : "");
      }
    } else {
      message = t("library.updateAsk", titleLabel, row.displayVersion || row.version, row.latestVersion || t("library.unknown"));
      if (row.hasLayeredFsMods) message += "\n\n" + t("library.modsWarning");
    }
    if (!await ask(promptTitle, message)) return;
    await post("/api/library/" + encodeURIComponent(row.titleId) + "/" + action, { confirm: true, acknowledgeMods: true });
    if (action === "update") toast(t("library.updateQueued"));
    await loadLibrary();
  });
};
$("library-refresh").onclick = event => busy(event.currentTarget, async () => { await post("/api/library/refresh"); await loadLibrary(); });
$("library-update-all").onclick = event => busy(event.currentTarget, async () => {
  const rows = library.titles.filter(row => row.updateState === "update_available" && row.updateHash && !row.hasLayeredFsMods);
  if (!await ask(t("library.updateAll"), t("library.updateAllAsk", rows.length))) return;
  let count = 0;
  for (const row of rows) { await post("/api/library/" + encodeURIComponent(row.titleId) + "/update", {}); count++; }
  toast(t("library.queuedCount", count)); location.hash = "#downloads";
});
$("library-search").oninput = renderLibrary;
$("library-filter").onchange = renderLibrary;

for (const kind of ["magnet", "torrent"]) $(kind + "-form").onsubmit = event => {
  event.preventDefault();
  busy(event.submitter, async () => {
    $("add-status").textContent = t("msg.resolving");
    if (kind === "magnet") await post("/api/add/magnet", { magnet: $("magnet-input").value.trim(), mode: $("magnet-mode").value });
    else {
      const file = $("torrent-file").files[0];
      if (!file || !/\.torrent$/i.test(file.name)) throw new Error(t("web.invalidTorrent"));
      const preview = await api("/api/preview/torrent", { method: "POST", headers: { "Content-Type": "application/x-bittorrent" }, body: file });
      openFileChooser(preview, $("torrent-mode").value === "install");
      $("add-status").textContent = "";
      return;
    }
    $("add-status").textContent = t("msg.addedQueue");
    $(kind + "-form").reset();
    location.hash = "#downloads"; await reloadTasks();
  }, $("add-status"));
};
function openFileChooser(data, install) {
  let submitted = false;
  modal(data.name, '<div class="file-list">' + data.files.map((file, index) =>
    '<label class="port-file"><span>' + esc(file.path) + ' · ' + esc(bytes(file.size)) + '</span><select data-file-choice="' + index +
    '"><option value="0">' + esc(t("web.action.skip")) + '</option><option value="1"' + (!install || !file.installable ? " selected" : "") + ">" +
    esc(t("web.action.download")) + '</option>' + (file.installable ? '<option value="2"' + (install ? " selected" : "") + ">" + esc(t("web.action.install")) + "</option>" : "") +
    "</select></label>").join("") + "</div>", '<button class="primary" id="file-choice-add">' + esc(t("web.add")) + "</button>");
  const cleanup = () => {
    $("modal").removeEventListener("close", cleanup);
    if (!submitted) post("/api/preview/discard", { token: data.token }).catch(error => toast(errorText(error), true));
  };
  $("modal").addEventListener("close", cleanup);
  $("file-choice-add").onclick = event => busy(event.currentTarget, async () => {
    const files = data.files.map((file, index) => ({ path: file.path,
      action: Number($("modal").querySelector('[data-file-choice="' + index + '"]').value) }));
    if (!files.some(file => file.action)) throw new Error(t("web.noFilesSelected"));
    await post("/api/add/prepared", { token: data.token, files });
    submitted = true; $("modal").close(); $("torrent-form").reset();
    toast(t("msg.addedQueue")); location.hash = "#downloads"; await reloadTasks();
  });
}
function fileDrop(zone, input, onChange) {
  zone.addEventListener("dragover", event => { event.preventDefault(); zone.classList.add("dragging"); });
  zone.addEventListener("dragleave", () => zone.classList.remove("dragging"));
  zone.addEventListener("drop", event => {
    event.preventDefault(); zone.classList.remove("dragging");
    if (event.dataTransfer.files.length) { input.files = event.dataTransfer.files; input.dispatchEvent(new Event("change")); }
  });
  input.addEventListener("change", onChange);
}
fileDrop($("torrent-drop"), $("torrent-file"), () => {});

const settingsGroups = [
  ["web.console", [
    ["language", "settings.language", "select", [["auto", "settings.auto"], ...Object.keys(languages).map(value => [value === "en" ? "en-US" : value, languages[value], true])]],
    ["theme", "settings.theme", "select", [["auto", "settings.auto"], ["dark", "settings.dark"], ["light", "settings.light"], ["liquid_glass", "Liquid Glass · Dark", true], ["liquid_glass_light", "Liquid Glass · Light", true], ["terminal", "theme.terminalDark"], ["material_you", "theme.materialLight"], ["terminal_light", "theme.terminalLight"], ["material_you_dark", "theme.materialDark"]]],
    ["themeAccent", "theme.accent", "select", [["default", "theme.default"], ["green", "theme.green"], ["blue", "theme.blue"], ["purple", "theme.purple"], ["amber", "theme.amber"], ["rose", "theme.rose"]]],
    ["confirmExit", "web.confirmExit", "checkbox"], ["warnOnActiveDownload", "web.warnExit", "checkbox"], ["screenSaverEnabled", "web.saver", "checkbox"]
  ]],
  ["settings.downloads", [
    ["maxActiveDownloads", "settings.concurrent", "select", [1, 2, 3, 4].map(value => [value, String(value), true])],
    ["streamSelection", "settings.stream", "select", [["allFiles", "settings.allFiles"], ["packagesOnly", "settings.packagesOnly"]]],
    ["installLocation", "settings.location", "select", [["sdCard", "web.sd"], ["systemMemory", "web.nand"]]],
    ["showCompletedDownloads", "web.showCompleted", "checkbox"], ["blankScreenDuringTransfers", "web.blankScreen", "checkbox"], ["sleepAfterDownloads", "web.sleep", "checkbox"]
  ]],
  ["web.source", [
    ["torrentingEnabled", "web.direct", "checkbox"], ["liveSeedingEnabled", "web.seeding", "checkbox"],
    ["debridProvider", "web.provider", "select", [["torbox", "TorBox", true], ["torrserver", "TorrServer", true], ["realdebrid", "Real-Debrid", true], ["alldebrid", "AllDebrid", true]]],
    ["torboxApiKey", "TorBox API key", "password"], ["realdebridApiKey", "Real-Debrid API token", "password"], ["alldebridApiKey", "AllDebrid API key", "password"],
    ["torrserverUrl", "TorrServer URL", "url"], ["keepUncachedDebrid", "web.keepUncached", "checkbox"]
  ]],
  ["settings.network", [["proxyUrl", "Proxy URL", "url"]]],
  ["settings.catalog", [["catalogSourceUrl", "settings.catalogUrl", "url"], ["refreshCatalogOnLaunch", "settings.refresh", "checkbox"]]],
  ["web.diagnostics", [["extendedTelemetry", "logs.telemetry", "checkbox"]]]
];
function renderSettings() {
  if (!settings) return;
  $("settings-fields").innerHTML = settingsGroups.map(([group, fields]) => '<section class="form-section"><h2 data-i18n="' + group + '">' + esc(t(group)) +
    '</h2>' + (group === "web.console" ? '<p data-i18n="web.restartSettings">' + esc(t("web.restartSettings")) + "</p>" : "") + '<div class="fields">' + fields.map(([key, label, type, options]) => {
      const name = key.includes("ApiKey") || key.endsWith("Url") && !translations.en[label] ? label : t(label);
      const labelAttr = translations.en[label] ? ' data-i18n="' + label + '"' : "";
      const id = "setting-" + key;
      if (type === "checkbox") return '<label><input id="' + id + '" data-setting="' + key + '" type="checkbox"' + (settings[key] ? " checked" : "") + "><span" + labelAttr + ">" + esc(name) + "</span></label>";
      const control = type === "select" ? '<select id="' + id + '" data-setting="' + key + '">' +
        options.map(([value, text, literal]) => '<option value="' + esc(value) + '"' + (!literal ? ' data-i18n="' + text + '"' : "") + (String(settings[key]) === String(value) ? " selected" : "") + ">" + esc(literal ? text : t(text)) + "</option>").join("") + "</select>" :
        '<input id="' + id + '" data-setting="' + key + '" type="' + type + '" value="' + (type === "password" ? "" : esc(settings[key])) + '" autocomplete="off">';
      if (type === "password") {
        const help = settings[key.replace("ApiKey", "Configured")] ? "web.keySaved" : "web.keyEmpty";
        return '<div class="secret-field"><label for="' + id + '"><span>' + esc(name) + "</span>" + control +
          '</label><small data-i18n="' + help + '">' + esc(t(help)) + '</small><label><input id="clear-' + key +
          '" type="checkbox"><span data-i18n="web.clearKey">' + esc(t("web.clearKey")) + "</span></label></div>";
      }
      return '<label><span' + labelAttr + ">" + esc(name) + "</span>" + control + "</label>";
    }).join("") + "</div></section>").join("");
  $("settings-save").disabled = false;
}
async function loadSettings() {
  $("settings-status").textContent = t("web.loading");
  try {
    settings = await api("/api/settings"); renderSettings();
    $("settings-status").textContent = "";
  } catch (error) { $("settings-status").textContent = errorText(error); }
}
$("settings-reload").onclick = event => busy(event.currentTarget, loadSettings);
$("settings-form").addEventListener("input", () => { $("settings-status").textContent = t("web.unsaved"); });
$("settings-form").onsubmit = event => {
  event.preventDefault();
  busy(event.submitter, async () => {
    const patch = {};
    document.querySelectorAll("[data-setting]").forEach(input => {
      const key = input.dataset.setting;
      if (input.type === "password") { if ($("clear-" + key).checked) patch[key] = ""; else if (input.value) patch[key] = input.value; }
      else patch[key] = input.type === "checkbox" ? input.checked : key === "maxActiveDownloads" ? Number(input.value) : input.value;
    });
    settings = await api("/api/settings", { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify(patch) });
    renderSettings(); $("settings-status").textContent = t("msg.saved"); toast(t("msg.saved"));
    await reloadTasks();
  }, $("settings-status"));
};
$("browser-language").innerHTML = Object.entries(languages).map(([value, name]) => '<option value="' + value + '">' + name + "</option>").join("");
$("browser-language").value = lang;
$("browser-language").onchange = () => {
  lang = $("browser-language").value;
  localStorage.setItem("pipensxLang", lang);
  translate(); renderDevice(); if (online !== null) setConnection(online);
  if (page() === "downloads") renderDownloads();
  if (catalog) renderCatalog();
  if (library) renderLibrary();
  document.title = (page() === "maintenance" ? t("web.maintenance") : page() === "add" ? t("web.add") : t("tabs." + page())) + " · pipensx";
};
$("browser-theme").onchange = () => { localStorage.setItem("pipensxTheme", $("browser-theme").value); applyTheme(); };
matchMedia("(prefers-color-scheme: light)").addEventListener("change", applyTheme);

function renderStorage() {
  if (!storageSnapshot || !Object.hasOwn(storageSnapshot, "images")) return;
  const data = storageSnapshot;
  const rows = [
    ["images", "web.images", bytes(data.images), data.images],
    ["torrents", "web.orphanTorrents", bytes(data.torrents), data.torrents],
    ["downloads", "web.orphanDownloads", bytes(data.orphanDownloads), data.orphanDownloads],
    ["temporary", "web.temporary", bytes(data.temporary), data.temporary],
    ["system", "web.installGarbage", bytes(data.systemBytes) + " · " + (data.systemItems || 0), data.systemAvailable && data.systemItems],
    ["tickets", "web.tickets", String(data.tickets || 0), data.ticketsAvailable && data.tickets]
  ];
  $("storage-details").innerHTML = rows.map(([action, key, size, enabled]) => '<div class="storage-row"><div><strong data-i18n="' + key + '">' + esc(t(key)) +
    '</strong><p>' + esc(size) + '</p></div><button class="danger" data-storage="' + action + '"' + (!enabled || storageBusy ? " disabled" : "") + ">" + esc(t("web.clean")) + "</button></div>").join("") +
    (data.systemError ? '<p class="error">' + esc(data.systemError) + "</p>" : "");
}
async function loadMaintenance() {
  if (!features.maintenance || storageBusy) return;
  try {
    const data = await api("/api/maintenance");
    storageSnapshot = data.storage; renderStorage();
  } catch (error) { $("storage-status").textContent = errorText(error); }
}
async function storageAction(action) {
  if (storageBusy) return;
  storageBusy = true; $("storage-scan").disabled = true; renderStorage();
  $("storage-status").textContent = t("web.loading");
  try {
    const job = await post("/api/maintenance/" + action, { confirm: true });
    const started = Date.now();
    await new Promise((resolve, reject) => {
      const poll = async () => {
        try {
          const data = await api("/api/maintenance");
          const current = data.jobs.find(item => item.jobId === job.jobId);
          if (current?.state === "error") throw new Error(current.error || t("web.failed"));
          if (current?.state === "done") { storageSnapshot = data.storage; resolve(); }
          else if (Date.now() - started > 180000) throw new Error(t("web.jobTimeout"));
          else setTimeout(poll, 1000);
        } catch (error) { reject(error); }
      };
      setTimeout(poll, 500);
    });
    $("storage-status").textContent = t(action === "scan" ? "web.scanDone" : "web.cleaned");
  } finally { storageBusy = false; $("storage-scan").disabled = false; renderStorage(); }
}
$("storage-scan").onclick = event => busy(event.currentTarget, () => storageAction("scan"), $("storage-status"));
$("storage-details").onclick = event => {
  const button = event.target.closest("[data-storage]");
  if (!button) return;
  busy(button, async () => {
    const message = t("web.cleanAsk") + (button.dataset.storage === "tickets" ? "\n\n" + t("web.ticketHelp") : "");
    if (await ask(t("web.clean"), message)) await storageAction(button.dataset.storage);
  }, $("storage-status"));
};
function renderUpdate() {
  if (!updateStatus) return;
  const data = updateStatus;
  $("update-status").textContent = t("update.current", data.currentVersion) + (data.targetPath ? " · " + data.targetPath : "") +
    "\n" + (data.stagedReady ? t("update.ready", bytes(data.stagedBytes)) : t("update.none")) + (!data.canApply ? "\n" + t("web.cannotRestart") : "");
  $("update-apply").hidden = $("update-discard").hidden = !data.stagedReady;
  $("update-apply").disabled = !data.canApply || updateBusy || restarting;
}
async function loadUpdate() {
  if (!features.update || updateBusy || restarting) return;
  try { updateStatus = await api("/api/update"); renderUpdate(); }
  catch (error) { $("update-status").textContent = errorText(error); }
}
fileDrop($("update-drop"), $("update-file"), () => {
  const file = $("update-file").files[0];
  $("update-file-info").textContent = file ? file.name + " · " + bytes(file.size) : "";
});
async function uploadNro(file, checksum = "") {
  const header = new Uint8Array(await file.slice(0, 0x80).arrayBuffer());
  if (!/\.nro$/i.test(file.name) || header.length < 0x80 || String.fromCharCode(...header.slice(0x10, 0x14)) !== "NRO0")
    throw new Error(t("web.invalidNro"));
  if (file.size > (updateStatus?.maxUploadBytes || 64 * 1024 * 1024)) throw new Error(t("web.largeNro"));
  if (!localStorage.getItem("pipensxPin") && !await requestPin()) throw new Error(t("control.pinHelp"));
  // Validate the PIN before sending a large body; api can retry only this small request.
  await post("/api/auth/check");
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", "/api/update/upload");
    request.setRequestHeader("Content-Type", "application/octet-stream");
    if (checksum) request.setRequestHeader("X-Pipensx-Sha256", checksum);
    request.setRequestHeader("X-Pipensx-Pin", localStorage.getItem("pipensxPin"));
    request.timeout = 180000;
    request.upload.onprogress = event => {
      if (event.lengthComputable) {
        $("update-progress").value = event.loaded / event.total * 100;
        $("update-status").textContent = t("update.uploading") + " " + Math.round(event.loaded / event.total * 100) + "%";
      }
    };
    request.onload = () => {
      let data;
      try { data = JSON.parse(request.responseText); } catch { reject(new Error(t("web.failed"))); return; }
      if (request.status >= 200 && request.status < 300) resolve(data);
      else reject(new Error(data.error ? t("web.serverError", data.error) : t("web.httpError", request.status)));
    };
    request.onerror = request.ontimeout = () => reject(new Error(t("web.networkError")));
    request.send(file);
  });
}
async function phoneFetch(url, limit) {
  if (!url.startsWith("https://")) throw new Error(t("web.phoneFailed"));
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(180000), credentials: "omit" });
    if (!response.ok || Number(response.headers.get("content-length")) > limit) throw new Error();
    const reader = response.body.getReader(), chunks = [];
    let size = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) { await reader.cancel(); throw new Error(); }
      chunks.push(value);
    }
    return new Blob(chunks);
  } catch (_) { throw new Error(t("web.phoneFailed")); }
}
async function sendCatalog(file, checksum = "") {
  if (!file || !file.size || file.size > 48 * 1024 * 1024) throw new Error(t("web.catalogFile"));
  await post("/api/auth/check");
  $("catalog-transfer-status").textContent = t("web.loading");
  const headers = { "Content-Type": "application/json" };
  if (checksum) headers["X-Pipensx-Sha256"] = checksum;
  await api("/api/catalog/upload", { method: "POST", headers, body: file });
  await loadCatalog(true);
  $("catalog-transfer-status").textContent = t("web.catalogDone");
}
$("catalog-upload").onclick = event => busy(event.currentTarget,
  () => sendCatalog($("catalog-file").files[0]), $("catalog-transfer-status"));
$("catalog-phone").onclick = event => busy(event.currentTarget, async () => {
  const config = await api("/api/settings");
  const language = config.language && config.language !== "auto" ? config.language : lang;
  const names = { ru: "RU", en: "EN", "en-US": "EN", fr: "FR", es: "ES", it: "IT", "pt-BR": "PT_BR", "zh-CN": "ZH_Hans" };
  const mirrorBases = ["https://gitlab.com/i3sey/catalog-data/-/raw/main/",
    "https://gitverse.ru/api/repos/i3sey/catalog-data/raw/branch/main/"];
  const filename = (names[language] || "EN") + "_catalog.json";
  const url = mirrorBases.includes(config.catalogSourceUrl) ? config.catalogSourceUrl + filename :
    config.catalogSourceUrl || "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/" + filename;
  $("catalog-transfer-status").textContent = t("web.loading");
  let checksum = "";
  if (url === "https://github.com/i3sey/pipensx-catalog/releases/latest/download/catalog.json") {
    const manifest = JSON.parse(await (await phoneFetch(url.replace("catalog.json", "manifest.json"), 64 * 1024)).text());
    checksum = manifest.catalog?.sha256 || "";
    if (manifest.schemaVersion !== 2 || !/^[a-f0-9]{64}$/.test(checksum)) throw new Error(t("web.phoneFailed"));
  }
  await sendCatalog(await phoneFetch(url, 48 * 1024 * 1024), checksum);
}, $("catalog-transfer-status"));
const phoneUpdate = document.createElement("button");
phoneUpdate.type = "button";
phoneUpdate.dataset.i18n = "web.phoneUpdate";
phoneUpdate.textContent = t("web.phoneUpdate");
$("update-form").before(phoneUpdate);
phoneUpdate.onclick = event => busy(event.currentTarget, async () => {
  if (updateBusy || restarting) return;
  updateBusy = true;
  $("update-upload").disabled = $("update-discard").disabled = true;
  renderUpdate();
  try {
    await post("/api/auth/check");
    $("update-status").textContent = t("web.loading");
    const release = JSON.parse(await (await phoneFetch("https://gitlab.com/api/v4/projects/88330126/releases/permalink/latest", 1024 * 1024)).text());
    const links = release.assets?.links || [];
    const asset = name => {
      const entry = links.find(item => item.name === name);
      const url = entry?.direct_asset_url || entry?.url || "";
      if (!url.startsWith("https://gitlab.com/") && !url.startsWith("https://github.com/i3sey/pipensx/releases/download/"))
        throw new Error(t("web.phoneFailed"));
      return url;
    };
    const checksum = (await (await phoneFetch(asset("pipensx.nro.sha256"), 4096)).text()).trim().split(/\s+/)[0].toLowerCase();
    if (!/^[a-f0-9]{64}$/.test(checksum)) throw new Error(t("web.phoneFailed"));
    const nro = await phoneFetch(asset("pipensx.nro"), 64 * 1024 * 1024);
    updateStatus = await uploadNro(new File([nro], "pipensx.nro"), checksum);
  } finally {
    updateBusy = false;
    $("update-upload").disabled = $("update-discard").disabled = false;
    renderUpdate();
  }
}, $("update-status"));
$("update-form").onsubmit = event => {
  event.preventDefault();
  busy(event.submitter, async () => {
    updateBusy = true; $("update-apply").disabled = true; $("update-discard").disabled = true;
    $("update-progress").hidden = false; $("update-progress").value = 0;
    try { updateStatus = await uploadNro($("update-file").files[0]); }
    finally { updateBusy = false; $("update-discard").disabled = false; renderUpdate(); }
  }, $("update-status"));
};
$("update-discard").onclick = event => busy(event.currentTarget, async () => {
  updateStatus = await post("/api/update/discard"); renderUpdate(); $("update-progress").hidden = true;
  $("update-form").reset(); $("update-file-info").textContent = "";
});
$("update-apply").onclick = event => busy(event.currentTarget, async () => {
  if (!await ask(t("web.replaceRestart"), t("control.updateHelp"))) return;
  const previousVersion = updateStatus.currentVersion;
  const previousInstance = instanceId;
  await post("/api/update/apply");
  restarting = true; setConnection(false);
  $("update-upload").disabled = $("update-discard").disabled = true;
  const started = Date.now();
  let disconnected = false;
  const check = async () => {
    try {
      const response = await fetch("/api/info", { cache: "no-store", signal: AbortSignal.timeout(4000) });
      if (!response.ok) throw new Error();
      const info = await response.json();
      if (disconnected || info.version !== previousVersion || (previousInstance && info.instanceId && info.instanceId !== previousInstance)) {
        restarting = false; setConnection(true);
        toast(t("web.reconnected", info.version));
        location.reload();
        return;
      }
    } catch { disconnected = true; }
    if (Date.now() - started < 120000) setTimeout(check, 2000);
    else { $("update-status").textContent = t("web.reopen"); $("connection-banner").textContent = t("web.reopen"); }
  };
  setTimeout(check, 500);
});

function filteredLog() {
  const query = $("log-search").value.toLowerCase(), filter = $("log-filter").value;
  return logText.split("\n").filter(line => (!query || line.toLowerCase().includes(query)) &&
    (filter === "all" || (filter === "errors" ? /\berror\b|\bfail(?:ed|ure)?\b|fatal/i.test(line) : filter === "install" ?
      /install|ncm|package|deploy/i.test(line) : /telemetry|performance|speed|benchmark/i.test(line)))).join("\n");
}
function renderLog() { $("log-output").textContent = filteredLog(); }
async function loadLogs() {
  if (logBusy) return;
  logBusy = true; $("log-status").textContent = t("web.loading");
  try { logText = await api("/api/logs", { raw: true }); renderLog(); $("log-status").textContent = new Date().toLocaleTimeString(lang); }
  catch (error) { $("log-status").textContent = errorText(error); }
  finally { logBusy = false; }
}
$("log-refresh").onclick = event => busy(event.currentTarget, loadLogs);
$("log-search").oninput = renderLog; $("log-filter").onchange = renderLog;
$("log-copy").onclick = event => busy(event.currentTarget, async () => {
  const value = filteredLog();
  if (navigator.clipboard && isSecureContext) await navigator.clipboard.writeText(value);
  else {
    const input = document.createElement("textarea"); input.value = value; document.body.append(input); input.select();
    const copied = document.execCommand("copy"); input.remove();
    if (!copied) throw new Error(t("log.copyFailed"));
  }
  toast(t("log.copied"));
});
$("log-download").onclick = () => {
  const url = URL.createObjectURL(new Blob([filteredLog()], { type: "text/plain;charset=utf-8" }));
  const link = document.createElement("a"); link.href = url; link.download = "pipensx.log"; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
setInterval(() => {
  if (document.hidden || online === false) return;
  if (page() === "maintenance" && $("log-auto").checked) loadLogs();
  if (page() === "library") loadLibrary();
}, 5000);
document.addEventListener("visibilitychange", () => { if (!document.hidden) busy(null, reloadTasks); });

let portEditor = null;
const portRequest = (path, method = "GET", body) => api(path, { method,
  ...(body ? { headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {}) });
function capturePortRules() {
  document.querySelectorAll("[data-port-row]").forEach(row => {
    const file = portEditor.data.files[Number(row.dataset.portRow)];
    file.enabled = row.querySelector('input[type="checkbox"]').checked;
    const directory = row.querySelector('input[type="text"]');
    if (directory) file.directory = directory.value.trim();
  });
}
async function openPortEditor(id) {
  const data = await portRequest("/api/ports/" + encodeURIComponent(id));
  portEditor = { id, data, saved: data.manual };
  renderPortEditor();
}
function renderPortEditor() {
  const editor = portEditor, data = editor.data;
  const disabled = data.editable ? "" : " disabled";
  modal(t("port.files"), "<p>" + esc(t("port.help")) + '</p><p id="port-plan-status" role="status">' + esc(data.detail || t(data.reviewRequired ? "port.help" : "port.ready")) +
    '</p><div class="port-file-list">' + data.files.map((file, index) => '<section class="port-file" data-port-row="' + index +
    '"><label><input type="checkbox"' + (file.enabled ? " checked" : "") + (disabled || (file.selectable === false ? " disabled" : "")) +
    "><span>" + esc(file.source) + " · " + esc(bytes(file.size)) + "</span></label>" +
    (file.package ? "<p>" + esc(t("port.package")) + "</p>" : "<label><span>" + esc(t("port.destination")) +
      '</span><input type="text" value="' + esc(file.directory || "") + '"' + disabled + '></label><button data-port-browse="' + index + '"' + disabled + ">" + esc(t("port.browse")) + "</button>") +
    (!file.available ? "<p>" + esc(t("port.pending")) + "</p>" : "") +
    (file.error ? '<p class="error">' + esc(file.error) + "</p>" : "") +
    ((file.members || []).length ? "<details><summary>" + esc(t("port.members", file.members.length)) + '</summary><div class="port-members">' +
      file.members.map(member => "<div><span>" + esc(member.source) + "</span><span>" + esc(member.destination) + "</span></div>").join("") + "</div></details>" : "") +
    "</section>").join("") + "</div>",
    '<button id="port-preview"' + disabled + ">" + esc(t("port.preview")) + '</button><button id="port-save"' + disabled + ">" + esc(t("port.save")) +
    '</button><button id="port-start" class="primary"' + (!editor.saved || !data.canStart || data.active ? " disabled" : "") + ">" + esc(t("port.start")) + "</button>");
  for (const [id, save] of [["port-preview", false], ["port-save", true]]) $(id).onclick = event => busy(event.currentTarget, async () => {
    capturePortRules();
    editor.data = await portRequest("/api/ports/" + encodeURIComponent(editor.id) + (save ? "" : "/preview"), save ? "PATCH" : "POST",
      { files: editor.data.files.map(({ source, enabled, directory }) => ({ source, enabled, directory })) });
    if (save) { editor.saved = true; toast(t("port.saved")); }
    renderPortEditor();
  });
  $("modal").querySelectorAll("[data-port-row] input").forEach(input => input.oninput = () => {
    editor.saved = false; $("port-start").disabled = true; input.closest("section").querySelector("details")?.remove();
    $("port-plan-status").textContent = t("port.preview");
  });
  $("modal").querySelectorAll("[data-port-browse]").forEach(button => button.onclick = event => busy(event.currentTarget, async () => {
    capturePortRules(); await showPortFolders(Number(button.dataset.portBrowse), "/");
  }));
  $("port-start").onclick = event => busy(event.currentTarget, async () => {
    await portRequest("/api/ports/" + encodeURIComponent(editor.id) + "/start", "POST", {});
    $("modal").close(); toast(t("port.started")); await reloadTasks();
  });
}
async function showPortFolders(index, path) {
  const data = await portRequest("/api/ports/folders?path=" + encodeURIComponent(path));
  const folders = path === "/" ? data.folders : [path.slice(0, path.lastIndexOf("/")) || "/", ...data.folders];
  modal(t("port.browse"), "<p>" + esc(path) + '</p><div class="port-folders">' + folders.map(folder => '<button data-port-folder="' + esc(folder) + '">' +
    esc(folder === "/" ? "/" : folder.split("/").pop()) + "</button>").join("") + "</div>" + (data.truncated ? "<p>" + esc(t("port.folderLimit")) + "</p>" : ""),
    '<button id="port-back">' + esc(t("port.back")) + '</button><button id="port-folder-select" class="primary">' + esc(t("port.useFolder")) + "</button>");
  $("port-back").onclick = renderPortEditor;
  $("port-folder-select").onclick = () => {
    portEditor.data.files[index].directory = path; portEditor.data.files[index].members = [];
    portEditor.data.detail = t("port.preview"); portEditor.saved = false; renderPortEditor();
  };
  $("modal").querySelectorAll("[data-port-folder]").forEach(button => button.onclick = event => busy(event.currentTarget,
    () => showPortFolders(index, button.dataset.portFolder)));
}

const queryPin = new URLSearchParams(location.search).get("pin");
if (queryPin) { localStorage.setItem("pipensxPin", queryPin); history.replaceState(null, "", location.pathname + location.hash); }
translate(); applyTheme(); closeMenu(); showPage();
async function start() {
  const info = await api("/api/info");
  features = info.features || {};
  instanceId = info.instanceId;
  $("catalog-favorites").hidden = !features.favorites;
  $("catalog-fetch").hidden = !features.catalogRefresh;
  $("catalog-transfer").hidden = !features.catalogUpload;
  $("update-section").hidden = !features.update;
  $("storage-section").hidden = !features.maintenance;
  const libraryLink = document.querySelector('[data-page="library"]');
  libraryLink.hidden = !features.library;
  await reloadTasks();
  if (page() === "catalog") await loadCatalog(true);
  if (page() === "maintenance") await loadUpdate();
}
busy(null, start, $("downloads-status"));
