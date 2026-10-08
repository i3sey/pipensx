/* pipensx web companion SPA — game-store edition. Vanilla JS, no build step.
   Talks to the REST/SSE API served by src/app/web_server.cpp. */
"use strict";

const $ = (id) => document.getElementById(id);

/* ================= i18n ================= */
const I18N = {
  en: {
    "control.pin": "Authorize panel", "control.pinHelp": "Enter the PIN set on the Switch in Settings → Web companion.",
    "control.continue": "Continue", "control.cancel": "Cancel", "control.invalidPin": "Incorrect PIN. Try again.",
    "control.updateHelp": "pipensx will close briefly to apply the uploaded update. Reopen it on the Switch if needed.",
    "tabs.downloads": "Downloads", "tabs.catalog": "Catalog", "tabs.add": "Add", "tabs.logs": "Logs", "tabs.settings": "Settings",
    "dash.speed": "Speed", "dash.queue": "Queue", "dash.remaining": "Remaining", "dash.storage": "SD storage",
    "dash.traffic": "Download traffic", "speed.download": "Download", "speed.install": "Installation",
    "downloads.search": "Search downloads…", "downloads.all": "All", "downloads.active": "Active",
    "downloads.pauseAll": "Pause all", "downloads.resumeAll": "Resume all", "downloads.clearDone": "Clear entries",
    "downloads.clearFiles": "Delete files",
    "downloads.clearNone": "No completed entries.",
    "downloads.clearEntriesAsk": "Remove {0} completed entries from the list? {1} in-use entries will be left. Downloaded files stay. Installed games and ports are not removed.",
    "downloads.clearFilesAsk": "Delete downloaded files for {0} completed entries and free about {1}? {2} in-use entries will be left, including their files. Installed games and ports are not removed.",
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
    "update.title": "pipensx update", "update.sub": "Upload a new pipensx.nro directly to the Switch. GitHub is not used for this update.",
    "update.drop": "Choose the new pipensx.nro", "update.upload": "Upload update", "update.apply": "Apply on Switch",
    "update.current": "Current version: {0}", "update.ready": "Update staged: {0}. Ready to apply.",
    "update.none": "No uploaded update is staged.", "update.uploading": "Uploading and verifying…", "update.applying": "Applying update — the Switch will close pipensx.",
    "logs.title": "Diagnostic log", "logs.sub": "Live tail of pipensx.log from the Switch.",
    "logs.search": "Search log…", "logs.all": "All", "logs.install": "Install", "logs.errors": "Errors",
    "logs.refresh": "Refresh", "logs.auto": "Auto refresh", "logs.copy": "Copy", "logs.download": "Download",
    "logs.performance": "Performance", "logs.telemetry": "Detailed speed diagnostics",
    "game.install": "Install", "game.download": "Download", "game.close": "Close",
    "game.developer": "Developer", "game.publisher": "Publisher", "game.genre": "Genre",
    "game.year": "Year", "game.size": "Size", "game.titleId": "Title ID",
    "game.multiplayer": "Multiplayer", "game.performance": "Notes", "game.hash": "Info hash",
    "game.peers": "Peers", "game.added": "Added",
    "task.moveUp": "↑", "task.moveDown": "↓", "task.top": "Top",
    "task.pause": "Pause", "task.resume": "Resume", "task.retry": "Retry",
    "task.verify": "Verify", "task.remove": "Remove",
    "action.leased": "Files are being copied to the SD card",
    "action.committing": "The package is being committed and cannot be paused",
    "action.not_pausable": "This download cannot be paused right now",
    "action.not_resumable": "This download is not paused",
    "action.not_completed": "Only a finished download can be verified",
    "action.debrid": "A debrid download cannot be verified",
    "action.not_queued": "Only a queued download can be moved",
    "action.removing": "This download is already being removed",
    "action.not_found": "This download is no longer in the list",
    "msg.addedQueue": "Added to the Switch queue", "msg.resolving": "Resolving on the Switch…",
    "msg.saved": "Settings saved",
    "msg.clearedEntries": "Removed {0} entries. Left {1} because they are in use.",
    "msg.clearedFiles": "Removed files for {0} entries. Left {1} because they are in use.",
    "recovery.downloaded": "Downloaded",
    "recovery.installed": "Installed",
    "recovery.verifying": "Verifying saved data",
    "recovery.verify": "Checking saved data again ({0}). Download stays at {1}.",
    "recovery.reread": "The current package is read again ({0}). {1} package(s) stay installed.",
    "recovery.refetch": "The current file is fetched again ({0}). Saved {1} stays.",
    "recovery.recopy": "This copy starts again ({0}). Finished downloads stay.",
    "recovery.pieces": "Continues from saved pieces, not from an exact byte.",
    "recovery.byte": "Continues from the saved point in this file.",
  },
  ru: {
    "control.pin": "Доступ к панели", "control.pinHelp": "Введите PIN из настроек Switch: Настройки → Web companion.",
    "control.continue": "Продолжить", "control.cancel": "Отмена", "control.invalidPin": "Неверный PIN. Попробуйте ещё раз.",
    "control.updateHelp": "pipensx закроется для установки загруженного обновления. При необходимости откройте его снова на Switch.",
    "tabs.downloads": "Загрузки", "tabs.catalog": "Каталог", "tabs.add": "Добавить", "tabs.logs": "Логи", "tabs.settings": "Настройки",
    "dash.speed": "Скорость", "dash.queue": "Очередь", "dash.remaining": "Осталось", "dash.storage": "Память SD",
    "speed.download": "Загрузка", "speed.install": "Установка",
    "dash.traffic": "Трафик",
    "downloads.search": "Поиск загрузок…", "downloads.all": "Все", "downloads.active": "Активные",
    "downloads.pauseAll": "Пауза всем", "downloads.resumeAll": "Продолжить все", "downloads.clearDone": "Очистить записи",
    "downloads.clearFiles": "Удалить файлы",
    "downloads.clearNone": "Нет завершённых записей.",
    "downloads.clearEntriesAsk": "Убрать {0} завершённых записей из списка? {1} занятых записей останутся. Скачанные файлы останутся. Установленные игры и порты не удаляются.",
    "downloads.clearFilesAsk": "Удалить скачанные файлы у {0} завершённых записей и освободить около {1}? {2} занятых записей останутся вместе с файлами. Установленные игры и порты не удаляются.",
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
    "update.title": "Обновление pipensx", "update.sub": "Загрузите новый pipensx.nro прямо на Switch. GitHub для обновления не используется.",
    "update.drop": "Выбрать новый pipensx.nro", "update.upload": "Загрузить обновление", "update.apply": "Применить на Switch",
    "update.current": "Текущая версия: {0}", "update.ready": "Обновление подготовлено: {0}. Можно применять.",
    "update.none": "Загруженного обновления нет.", "update.uploading": "Загрузка и проверка…", "update.applying": "Обновление применяется — pipensx закроется на Switch.",
    "logs.title": "Диагностический лог", "logs.sub": "Конец pipensx.log прямо со Switch.",
    "logs.search": "Поиск в логе…", "logs.all": "Все", "logs.install": "Установка", "logs.errors": "Ошибки",
    "logs.refresh": "Обновить", "logs.auto": "Автообновление", "logs.copy": "Копировать", "logs.download": "Скачать",
    "logs.performance": "Производительность", "logs.telemetry": "Подробная диагностика скорости",
    "game.install": "Установить", "game.download": "Скачать", "game.close": "Закрыть",
    "game.developer": "Разработчик", "game.publisher": "Издатель", "game.genre": "Жанр",
    "game.year": "Год", "game.size": "Размер", "game.titleId": "Title ID",
    "game.multiplayer": "Мультиплеер", "game.performance": "Заметки", "game.hash": "Инфо-хеш",
    "game.peers": "Пиры", "game.added": "Добавлена",
    "task.moveUp": "↑", "task.moveDown": "↓", "task.top": "Вверх",
    "task.pause": "Пауза", "task.resume": "Продолжить", "task.retry": "Повторить",
    "task.verify": "Проверить", "task.remove": "Удалить",
    "action.leased": "Файлы копируются на карту памяти",
    "action.committing": "Пакет фиксируется, пауза недоступна",
    "action.not_pausable": "Эту загрузку сейчас нельзя поставить на паузу",
    "action.not_resumable": "Эта загрузка не на паузе",
    "action.not_completed": "Проверить можно только завершённую загрузку",
    "action.debrid": "Загрузку через debrid нельзя проверить",
    "action.not_queued": "Перемещать можно только задачу в очереди",
    "action.removing": "Эта загрузка уже удаляется",
    "action.not_found": "Этой загрузки больше нет в списке",
    "msg.addedQueue": "Добавлено в очередь Switch", "msg.resolving": "Распознаётся на Switch…",
    "msg.saved": "Настройки сохранены",
    "msg.clearedEntries": "Убрано записей: {0}. Пропущено занятых: {1}.",
    "msg.clearedFiles": "Удалены файлы у {0} записей. Пропущено занятых: {1}.",
    "recovery.downloaded": "Скачано",
    "recovery.installed": "Установлено",
    "recovery.verifying": "Проверяем сохранённые данные",
    "recovery.verify": "Сохранённые данные проверяются снова ({0}). Скачано остаётся {1}.",
    "recovery.reread": "Текущий пакет читается заново ({0}). Установленные пакеты остаются: {1}.",
    "recovery.refetch": "Текущий файл скачивается заново ({0}). Сохранено {1}.",
    "recovery.recopy": "Копирование начинается заново ({0}). Уже скачанное остаётся.",
    "recovery.pieces": "Продолжение с сохранённых частей, не с точного байта.",
    "recovery.byte": "Продолжение с сохранённого места в этом файле.",
  },
};
Object.assign(I18N, window.PIPENSX_I18N || {});
const browserLanguage = (navigator.language || "en").toLowerCase();
let lang = localStorage.getItem("pipensxLang") ||
  (browserLanguage.startsWith("pt") ? "pt-BR" : browserLanguage.startsWith("zh") ? "zh-CN" : browserLanguage.split("-")[0]);
const catalogLanguages = ["en", "ru", "pt-BR", "fr", "es", "zh-CN", "it", "ko"];
if (!catalogLanguages.includes(lang)) lang = "en";
let descriptionGeneration = 0;
let clearUiReady = false;
const t = (k) => (I18N[lang] && I18N[lang][k]) || I18N.en[k] || k;
function applyI18n() {
  document.documentElement.lang = lang;
  document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
  document.querySelectorAll("[data-i18n-ph]").forEach((el) => { el.placeholder = t(el.dataset.i18nPh); });
  if (clearUiReady) updateClearButtons();
  $("lang-btn").textContent = lang.toUpperCase();
  $("alerts-state").textContent = t(alertsOn() ? "common.on" : "common.off");
  if (library) renderLibrary();
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
  lang = catalogLanguages[(catalogLanguages.indexOf(lang) + 1) % catalogLanguages.length];
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

// HTML dialogs work in browsers that disable window.prompt/window.confirm.
function controlDialog(title, message, pinMode = false, actionText = "") {
  const dialog = $("control-dialog");
  const form = $("control-form");
  const input = $("control-pin");
  const previousFocus = document.activeElement;
  $("control-title").textContent = title;
  $("control-message").textContent = message;
  $("control-pin-field").hidden = !pinMode;
  input.value = "";
  input.required = pinMode;
  $("control-cancel").textContent = t("control.cancel");
  $("control-submit").textContent = pinMode ? t("control.continue") : (actionText || t("update.apply"));
  return new Promise((resolve) => {
    const finish = (value) => {
      form.removeEventListener("submit", submit);
      $("control-submit").removeEventListener("click", submit);
      dialog.removeEventListener("keydown", keydown);
      $("control-cancel").removeEventListener("click", cancel);
      dialog.hidden = true;
      input.value = "";
      if (previousFocus?.isConnected) previousFocus.focus();
      resolve(value);
    };
    const submit = (event) => {
      event.preventDefault();
      if (pinMode && !input.value.trim()) return;
      finish(pinMode ? input.value.trim() : true);
    };
    const cancel = (event) => { event.preventDefault(); finish(false); };
    const keydown = (event) => {
      if (event.key === "Escape") return cancel(event);
      if (event.key === "Enter" && event.target === input) return submit(event);
      if (event.key === "Tab") {
        const first = pinMode ? input : $("control-cancel");
        const last = $("control-submit");
        if (event.shiftKey && document.activeElement === first) {
          event.preventDefault(); last.focus();
        } else if (!event.shiftKey && document.activeElement === last) {
          event.preventDefault(); first.focus();
        }
      }
    };
    form.addEventListener("submit", submit);
    $("control-submit").addEventListener("click", submit);
    dialog.addEventListener("keydown", keydown);
    $("control-cancel").addEventListener("click", cancel);
    dialog.hidden = false;
    (pinMode ? input : $("control-submit")).focus();
  });
}

let pendingPin = null;
function requestPin() {
  // Settings and update status can both return 401 at the same time.
  if (!pendingPin) {
    pendingPin = controlDialog(t("control.pin"), t("control.pinHelp"), true)
      .then((pin) => {
        if (pin) localStorage.setItem("pipensxPin", pin);
        return pin;
      }).finally(() => { pendingPin = null; });
  }
  return pendingPin;
}

async function api(path, options = {}, retryPin = true) {
  options.headers = Object.assign({}, options.headers);
  const sentPin = getPin();
  if (sentPin) options.headers["X-Pipensx-Pin"] = sentPin;
  const resp = await fetch(path, options);
  if (resp.status === 401 && retryPin) {
    const pin = getPin() && getPin() !== sentPin ? getPin() : await requestPin();
    if (pin) {
      const retry = await api(path, options, false);
      if (retry.status === 401) toast(t("control.invalidPin"), true);
      return retry;
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
function tf(k, ...args) {
  let value = t(k);
  args.forEach((arg, index) => { value = value.replace(`{${index}}`, arg); });
  return value;
}
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
  try { return new Date(Number(ts) * 1000).toLocaleDateString(lang === "en" ? "en-US" : lang); }
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
  modal.classList.toggle("port-dialog", html.includes('class="modal-body port-editor"'));
  if (!modal.open) modal.showModal();
  modal.onclick = (e) => { if (e.target === modal) closeModal(); };
}

/* ---------- tabs ---------- */
const tabs = ["downloads", "catalog", "library", "add", "logs", "settings"];
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
  if (active === "library") loadLibrary();
  if (active === "logs") startLogView();
  else stopLogView();
  if (active === "settings") loadSettings();
}
window.addEventListener("hashchange", showTab);
$("empty-browse")?.addEventListener("click", () => { location.hash = "#catalog"; });

/* ---------- live state (SSE) ---------- */
let state = { tasks: [], jobs: [], storage: null, summary: null, version: "" };
clearUiReady = true;
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

/* ---------- diagnostic log ---------- */
let logText = "";
let logTimer = null;
let logLoading = false;

function filteredLogText() {
  const mode = $("log-filter")?.value || "all";
  const query = ($("log-search")?.value || "").trim().toLowerCase();
  const lines = logText.split("\n");
  return lines.filter((line) => {
    const lower = line.toLowerCase();
    let matchesMode = true;
    if (mode === "realdebrid")
      matchesMode = lower.includes("[realdebrid]") || lower.includes("[debrid]") || lower.includes("real-debrid");
    else if (mode === "install")
      matchesMode = lower.includes("[install]") || lower.includes("package") || lower.includes("nsp") || lower.includes("nsz");
    else if (mode === "errors")
      matchesMode = lower.includes("error") || lower.includes("failed") || lower.includes("rejected") || lower.includes("[diagnostic]");
    else if (mode === "performance")
      matchesMode = lower.includes("[telemetry]") || lower.includes("[debrid-intent]");
    return matchesMode && (!query || lower.includes(query));
  }).join("\n");
}

function renderLog() {
  const output = $("log-output");
  if (!output) return;
  const nearBottom = output.scrollHeight - output.scrollTop - output.clientHeight < 80;
  output.textContent = filteredLogText();
  if (nearBottom) output.scrollTop = output.scrollHeight;
}

async function loadLogs() {
  if (logLoading) return;
  logLoading = true;
  const status = $("log-status");
  try {
    const resp = await api("/api/logs", { cache: "no-store" });
    if (!resp.ok) {
      if (resp.status !== 401) {
        const body = await resp.json().catch(() => ({}));
        if (status) status.textContent = body.error || `HTTP ${resp.status}`;
      }
      return;
    }
    logText = await resp.text();
    if (status) status.textContent = `${fmtBytes(new Blob([logText]).size)} · ${new Date().toLocaleTimeString()}`;
    renderLog();
  } catch (e) {
    if (status) status.textContent = "offline";
  } finally {
    logLoading = false;
    loadInstallBenchmark();
  }
}

async function loadInstallBenchmark() {
  const card = $("log-benchmark");
  if (!card) return;
  try {
    const response = await api("/api/benchmark", { cache: "no-store" });
    if (!response.ok) return;
    const result = await response.json();
    card.hidden = result.phase === "idle";
    if (card.hidden) return;
    const labels = lang === "pt-BR"
      ? ["Benchmark de instalação", "Bloco", "Repetições", "Tempo mediano", "Configuração escolhida", "Gravação média", "Teste com arquivos locais. Não inclui download nem registro final do jogo."]
      : ["Installation benchmark", "Batch", "Repetitions", "Median time", "Selected configuration", "Average write rate", "Test uses local files. Internet download and final game registration are excluded."];
    const rows = [262144, 1048576, 4194304].map(bytes => {
      const samples = (result.samples || []).filter(sample => sample.batchBytes === bytes);
      const times = samples
        .map(sample => sample.durationUs / 1e6).sort((a, b) => a - b);
      const median = times.length === 3 ? `${times[1].toFixed(2)} s` : "—";
      const rate = times.length === 3 ? fmtSpeed(samples[0].installedBytes / times[1]) : "—";
      return `<tr><td>${fmtBytes(bytes)}</td><td>${times.length}/3</td><td>${median}</td><td>${rate}</td></tr>`;
    }).join("");
    const phase = lang === "pt-BR" ? ({ warming: "Aquecimento", measuring: "Medindo", completed: "Concluído", cancelled: "Cancelado", error: "Erro" }[result.phase] || result.phase) : result.phase;
    card.innerHTML = `<h2>${esc(labels[0])}</h2>
      <p>${esc(phase)} · ${Math.min(9, result.pass || 0)}/9</p>
      <p>${esc(labels[6])}</p>
      <table class="facts"><tr><th>${esc(labels[1])}</th><th>${esc(labels[2])}</th><th>${esc(labels[3])}</th><th>${esc(labels[5])}</th></tr>${rows}</table>
      ${result.phase === "completed" ? `<p>${esc(labels[4])}: ${fmtBytes(result.chosenBatchBytes)}</p>` : ""}
      ${result.error ? `<p>${esc(result.error)}</p>` : ""}`;
  } catch (_) { }
}

function startLogView() {
  loadLogs();
  loadLogTelemetry();
  stopLogView();
  if ($("log-auto")?.checked)
    logTimer = setInterval(() => { if (currentTab() === "logs") loadLogs(); }, 3000);
}

function stopLogView() {
  if (logTimer) clearInterval(logTimer);
  logTimer = null;
}

async function loadLogTelemetry() {
  const control = $("log-telemetry");
  if (!control || control.disabled) return;
  try {
    const resp = await api("/api/settings", { cache: "no-store" });
    if (resp.ok && !control.disabled) control.checked = !!(await resp.json()).extendedTelemetry;
  } catch (_) { /* Offline state is already shown by the log view. */ }
}

$("log-telemetry")?.addEventListener("change", async (event) => {
  const control = event.target;
  const enabled = control.checked;
  control.disabled = true;
  try {
    const resp = await api("/api/settings", {
      method: "PATCH", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ extendedTelemetry: enabled }),
    });
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) { control.checked = !enabled; return toast(body.error || "save failed", true); }
    control.checked = !!body.extendedTelemetry;
    loadLogs();
  } catch (error) {
    control.checked = !enabled;
    toast(error.message || "offline", true);
  } finally { control.disabled = false; }
});

$("log-search")?.addEventListener("input", renderLog);
$("log-filter")?.addEventListener("change", renderLog);
$("log-refresh")?.addEventListener("click", loadLogs);
$("log-auto")?.addEventListener("change", () => {
  if (currentTab() === "logs") startLogView();
});
$("log-copy")?.addEventListener("click", async () => {
  const text = filteredLogText();
  try {
    await navigator.clipboard.writeText(text);
    toast(t("log.copied"));
  } catch {
    toast(t("log.copyFailed"), true);
  }
});
$("log-download")?.addEventListener("click", () => {
  const blob = new Blob([filteredLogText()], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = `pipensx-log-${new Date().toISOString().replace(/[:.]/g, "-")}.txt`;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 0);
});

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
  $("alerts-state").textContent = t(alertsOn() ? "common.on" : "common.off");
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
  if ((tt.peers || 0) === 0 && !(tt.speedBps > 0)) return { cls: "poor", label: t("peer.none") };
  if (!(tt.speedBps > 0)) return { cls: "slow", label: t("peer.stalled") };
  if ((tt.speedBps || 0) < 512 * 1024) return { cls: "slow", label: t("peer.slow") };
  return { cls: "excellent", label: "● " + fmtSpeed(tt.speedBps) };
}

function recoveryText(tt) {
  const r = tt.recovery;
  if (!r) return "";
  const quiet = ["Downloading", "Installing", "Committing", "Fetching"].includes(tt.status);
  const fill = (key, ...args) => args.reduce((s, a, i) => s.replace("{" + i + "}", a), t(key));
  let notice = "";
  if ((!quiet || r.verifyingSaved) && r.work === "verify" && r.reworkBytes > 0)
    notice = fill("recovery.verify", fmtBytes(r.reworkBytes), fmtBytes(r.savedBytes));
  else if (!quiet && r.work === "reread" && r.reworkBytes > 0)
    notice = fill("recovery.reread", fmtBytes(r.reworkBytes), String(r.packagesInstalled));
  else if (!quiet && r.work === "refetch" && r.reworkBytes > 0)
    notice = fill("recovery.refetch", fmtBytes(r.reworkBytes), fmtBytes(r.savedBytes));
  else if (!quiet && r.work === "recopy" && r.reworkBytes > 0)
    notice = fill("recovery.recopy", fmtBytes(r.reworkBytes));
  else if (!quiet && r.work === "pieces")
    notice = t("recovery.pieces");
  else if (!quiet && r.byteExact)
    notice = t("recovery.byte");
  else if (r.verifyingSaved)
    notice = t("recovery.verifying");
  if (!notice && !r.showInstalled && !r.verifyingSaved) return "";
  const parts = [];
  if (r.showDownloaded) parts.push(`${t("recovery.downloaded")} ${fmtBytes(r.downloadedBytes)} / ${fmtBytes(r.downloadTotalBytes)}`);
  if (r.showInstalled) parts.push(`${t("recovery.installed")} ${r.packagesInstalled} / ${r.packageCount}`);
  if (notice) parts.push(notice);
  return parts.join(" · ");
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
  if (tt.status === "Downloading" || tt.status === "Installing")
    meta.push(`${t("speed.download")}: ${fmtSpeed(tt.speedBps)}`);
  if (tt.status === "Installing" || tt.status === "Committing")
    meta.push(`${t("speed.install")}: ${fmtSpeed(tt.installSpeedBps)}`);
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
  const recoveryLine = recoveryText(tt);

  const h = healthOf(tt);
  const btn = (label, cmd, cls = "") =>
    `<button class="btn ${cls}" data-task="${esc(tt.id)}" data-cmd="${cmd}">${esc(label)}</button>`;
  const caps = tt.capabilities || {};
  const allowed = (name) => !!(caps[name] && caps[name].allowed);
  const actions = [];
  if (tt.mode === "port") actions.push(btn(t("port.files"), "port-files"));
  if (allowed("pause")) actions.push(btn(t("task.pause"), "pause"));
  if (tt.status === "Paused" && allowed("resume")) actions.push(btn(t("task.resume"), "resume", "btn-primary"));
  if (tt.status === "Error" && allowed("resume")) actions.push(btn(t("task.retry"), "retry", "btn-primary"));
  if (allowed("move")) {
    actions.push(`<button class="btn btn-sm" data-task="${esc(tt.id)}" data-cmd="move-up" title="move up">${t("task.moveUp")}</button>`);
    actions.push(`<button class="btn btn-sm" data-task="${esc(tt.id)}" data-cmd="move-down" title="move down">${t("task.moveDown")}</button>`);
    actions.push(btn(t("task.top"), "move-front"));
  }
  if (allowed("verify")) actions.push(btn(t("task.verify"), "verify"));
  if (allowed("remove")) actions.push(btn(t("task.remove"), "remove", "btn-danger"));

  return `<div class="card task" data-id="${esc(tt.id)}">
    <div class="task-head">
      <div class="task-name" title="${esc(tt.name || tt.id)}">${esc(tt.name || tt.id)}</div>
      <span class="badge ${esc(tt.status.toLowerCase())}">${esc(t("status." + tt.status))}</span>
    </div>
    ${tt.status === "Queued" && queueIdx > 0 ? `<div class="task-sub"><span class="queue-pos">#${queueIdx} ${t("queue.position")}</span></div>` : ""}
    <div class="bar"><div class="bar-fill ${barClass}" style="width:${pct}%"></div></div>
    <div class="task-meta">${meta.map(esc).join('<span class="dot">·</span>')}${h ? `<span class="health ${h.cls}">${esc(h.label)}</span>` : ""}</div>
    ${recoveryLine ? `<div class="task-sub">${esc(recoveryLine)}</div>` : ""}
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

  updateClearButtons();
  const activeCount = state.tasks.filter((x) => activeSet.has(x.status)).length;
  const dlBadge = $("dl-count");
  if (activeCount > 0) { dlBadge.hidden = false; dlBadge.textContent = activeCount; }
  else dlBadge.hidden = true;

  // Dashboard: prefer server summary, fall back to local aggregate.
  const s = state.summary;
  const dlSpeed = s ? s.downloadSpeedBps : state.tasks.reduce((a, x) => a + (x.speedBps || 0), 0);
  const instSpeed = s ? s.installSpeedBps : state.tasks.reduce((a, x) => a + (x.installSpeedBps || 0), 0);
  const peers = state.tasks.reduce((a, x) => a + (x.peers || 0), 0);
  // Streaming installs consume the same bytes they download. Summing these
  // rates double-counts that traffic and overstates network throughput.
  $("stat-speed").textContent = fmtSpeed(dlSpeed);
  $("stat-install-speed").textContent = `${t("speed.install")}: ${fmtSpeed(instSpeed)}`;
  $("stat-peers").textContent = peers > 0 ? `${peers} peers · DHT` : (activeCount > 0 ? `${activeCount} active` : "idle");
  const queued = s ? s.queued : state.tasks.filter((x) => x.status === "Queued").length;
  const paused = s ? s.paused : state.tasks.filter((x) => x.status === "Paused").length;
  const errors = s ? s.errors : state.tasks.filter((x) => x.status === "Error").length;
  $("stat-queue").textContent = `${activeCount} / ${state.tasks.length}`;
  $("stat-queue-sub").textContent =
    tf("queue.summary", queued, paused, errors);
  const remaining = s ? s.remainingBytes : 0;
  $("stat-remaining").textContent = remaining > 0 ? fmtBytes(remaining) : (state.tasks.length ? "—" : "0 B");
  const eta = s ? s.etaSeconds : 0;
  $("stat-eta").textContent = eta ? "ETA " + fmtEta(eta) : "";

  if (state.storage && state.storage.available) {
    const used = state.storage.totalBytes - state.storage.freeBytes;
    $("storage-fill").style.width = (100 * used / state.storage.totalBytes).toFixed(1) + "%";
    $("stat-storage").textContent = fmtBytes(state.storage.freeBytes);
    $("storage-text").textContent =
      tf("storage.free", fmtBytes(state.storage.totalBytes));
  } else {
    $("stat-storage").textContent = "—";
    $("storage-text").textContent = "—";
  }

  // Speed history chart.
  speedHist.push(dlSpeed || 0);
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
function fill(template, values) {
  return String(template).replace(/\{(\d+)\}/g, (_, i) => values[Number(i)] ?? "");
}
function clearPlan() {
  let clearable = 0, skipped = 0, bytes = 0;
  for (const task of (state && state.tasks) || []) {
    if (task.status !== "Completed" && task.status !== "Installed") continue;
    const reason = task.capabilities && task.capabilities.remove &&
      task.capabilities.remove.reason;
    if (reason === "leased") skipped += 1;
    else {
      clearable += 1;
      bytes += Number(task.completedBytes) || 0;
    }
  }
  return { clearable, skipped, bytes };
}
function updateClearButtons() {
  const entries = $("bulk-clear");
  const files = $("bulk-clear-files");
  if (!entries || !files) return;
  const plan = clearPlan();
  entries.textContent = t("downloads.clearDone") + " (" + plan.clearable + ")";
  files.textContent = t("downloads.clearFiles") +
    (plan.clearable ? " (" + fmtBytes(plan.bytes) + ")" : "");
}
function askClear(deleteData) {
  const plan = clearPlan();
  if (plan.clearable === 0) {
    toast(plan.skipped === 0
      ? t("downloads.clearNone")
      : fill(t(deleteData ? "msg.clearedFiles" : "msg.clearedEntries"),
             [0, plan.skipped]));
    return;
  }
  const message = deleteData
    ? fill(t("downloads.clearFilesAsk"),
           [plan.clearable, fmtBytes(plan.bytes), plan.skipped])
    : fill(t("downloads.clearEntriesAsk"), [plan.clearable, plan.skipped]);
  openModal(`<div class="modal-body">
      <h3>${esc(deleteData ? t("downloads.clearFiles") : t("downloads.clearDone"))}</h3>
      <div class="modal-sub">${esc(message)}</div>
    </div>
    <div class="modal-actions">
      <button class="btn" id="clr-cancel">${esc(t("game.close"))}</button>
      <button class="btn btn-danger" id="clr-ok">${esc(deleteData ? t("downloads.clearFiles") : t("downloads.clearDone"))}</button>
    </div>`);
  $("clr-cancel").onclick = closeModal;
  $("clr-ok").onclick = async () => {
    closeModal();
    const r = await postJson("/api/queue/clear-completed", { deleteData });
    if (r.ok) {
      const body = await r.json().catch(() => ({}));
      toast(fill(t(deleteData ? "msg.clearedFiles" : "msg.clearedEntries"),
                 [body.cleared || 0, body.skippedBusy || 0]));
      await reloadTasks();
    } else if (r.status !== 401) {
      const body = await r.json().catch(() => ({}));
      toast(body.error || "clear failed", true);
    }
  };
}
$("bulk-clear").addEventListener("click", () => askClear(false));
$("bulk-clear-files").addEventListener("click", () => askClear(true));

function explainRejection(code) {
  if (!code) return "";
  const key = "action." + code;
  return (I18N[lang] && I18N[lang][key]) || I18N.en[key] || code;
}
async function reloadTasks() {
  try {
    const resp = await api("/api/tasks");
    if (!resp.ok) return;
    state = Object.assign(state, await resp.json());
    renderDownloads();
  } catch (e) { /* the next SSE frame still replaces a stale card */ }
}
document.addEventListener("click", async (e) => {
  const cmdBtn = e.target.closest("[data-cmd]");
  if (cmdBtn) {
    const id = cmdBtn.dataset.task;
    const cmd = cmdBtn.dataset.cmd;
    if (cmd === "port-files") return openPortEditor(id);
    if (cmd === "remove") return confirmRemove(id);
    const resp = await api(`/api/tasks/${id}/${cmd}`, { method: "POST" });
    if (!resp.ok && resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(explainRejection(body.error) || `${cmd} failed`, true);
    }
    if (resp.status !== 401) await reloadTasks();
    return;
  }
  const jobBtn = e.target.closest("[data-job]");
  if (jobBtn) await api(`/api/jobs/${jobBtn.dataset.job}/cancel`, { method: "POST" });
});

let portEditor = null;
async function portRequest(path, method = "GET", body) {
  const response = await api(path, { method, cache: "no-store",
    ...(body ? { headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {}) });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || t("port.failed"));
  return data;
}
function capturePortRules() {
  if (!portEditor) return;
  document.querySelectorAll("[data-port-row]").forEach(row => {
    const file = portEditor.data.files[Number(row.dataset.portRow)];
    file.enabled = row.querySelector("input[type=checkbox]").checked;
    const directory = row.querySelector("input[type=text]");
    if (directory) file.directory = directory.value.trim();
  });
}
function portRulesBody() {
  capturePortRules();
  return { files: portEditor.data.files.map(({source, enabled, directory}) => ({source, enabled, directory})) };
}
async function openPortEditor(id) {
  try {
    const data = await portRequest(`/api/ports/${encodeURIComponent(id)}`);
    portEditor = { id, data, saved: data.manual };
    renderPortEditor();
  } catch (error) { toast(error.message, true); }
}
function renderPortEditor() {
  const editor = portEditor, data = editor.data;
  const disabled = data.editable ? "" : "disabled";
  openModal(`<div class="modal-body port-editor">
    <h3>${esc(t("port.files"))}</h3><p>${esc(t("port.help"))}</p>
    <p class="port-plan-status" role="status">${esc(data.detail || t(data.reviewRequired ? "port.help" : "port.ready"))}</p>
    ${data.active ? `<p>${esc(fmtBytes(data.bytesCopied))} / ${esc(fmtBytes(data.totalBytes))}<br>${esc(data.currentPath || "")}</p>` : ""}
    <div class="port-file-list">${data.files.map((file, index) => `
      <section class="port-file" data-port-row="${index}">
        <label><input type="checkbox" ${file.enabled ? "checked" : ""} ${disabled || (file.selectable === false ? "disabled" : "")}>
          <strong>${esc(file.source)}</strong> · ${esc(fmtBytes(file.size))}</label>
        ${file.package ? `<p>${esc(t("port.package"))}</p>` : `
          <label>${esc(t("port.destination"))}<input type="text" value="${esc(file.directory || "")}" placeholder="${esc(t("port.automatic"))}" ${disabled}></label>
          <button class="btn btn-sm" data-port-browse="${index}" ${disabled}>${esc(t("port.browse"))}</button>`}
        ${!file.available ? `<p>${esc(t("port.pending"))}</p>` : ""}
        ${file.error ? `<p class="task-error">${esc(file.error)}</p>` : ""}
        ${file.members.length ? `<details><summary>${esc(tf("port.members", file.members.length))}</summary>
          <div class="port-members">${file.members.map(member => `<div><span>${esc(member.source)}</span><span>→ ${esc(member.destination || "—")}</span></div>`).join("")}</div>
        </details>` : ""}
      </section>`).join("")}</div>
    </div><div class="modal-actions">
      <button class="btn" id="port-close">${esc(t("game.close"))}</button>
      <button class="btn" id="port-preview" ${disabled}>${esc(t("port.preview"))}</button>
      <button class="btn" id="port-save" ${disabled}>${esc(t("port.save"))}</button>
      <button class="btn btn-primary" id="port-start" ${!editor.saved || !data.canStart || data.active ? "disabled" : ""}>${esc(t("port.start"))}</button>
    </div>`);
  $("port-close").onclick = closeModal;
  $("port-preview").onclick = () => updatePortEditor(false);
  $("port-save").onclick = () => updatePortEditor(true);
  document.querySelectorAll("[data-port-row] input").forEach(input => input.addEventListener("input", () => {
    editor.saved = false; $("port-start").disabled = true;
    input.closest("[data-port-row]").querySelector("details")?.remove();
    document.querySelector(".port-plan-status").textContent = t("port.preview");
  }));
  document.querySelectorAll("[data-port-browse]").forEach(button => button.onclick = () => {
    capturePortRules(); showPortFolders(Number(button.dataset.portBrowse), "/");
  });
  $("port-start").onclick = async () => {
    $("port-start").disabled = true;
    try {
      await portRequest(`/api/ports/${encodeURIComponent(editor.id)}/start`, "POST", {});
      closeModal(); toast(t("port.started")); await reloadTasks();
    } catch (error) { toast(error.message, true); renderPortEditor(); }
  };
}
async function updatePortEditor(save) {
  const editor = portEditor;
  try {
    const data = await portRequest(`/api/ports/${encodeURIComponent(editor.id)}${save ? "" : "/preview"}`,
      save ? "PATCH" : "POST", portRulesBody());
    editor.data = data;
    if (save) { editor.saved = true; toast(t("port.saved")); }
    renderPortEditor();
  } catch (error) { toast(error.message, true); }
}
async function showPortFolders(index, path) {
  try {
    const data = await portRequest(`/api/ports/folders?path=${encodeURIComponent(path)}`);
    openModal(`<div class="modal-body port-editor"><h3>${esc(t("port.browse"))}</h3>
      <p>${esc(path)}</p><div class="port-folders">
      ${path !== "/" ? `<button class="btn" data-port-folder="${esc(path.slice(0, path.lastIndexOf("/")) || "/")}">..</button>` : ""}
      ${data.folders.map(folder => `<button class="btn" data-port-folder="${esc(folder)}">${esc(folder.split("/").pop())}</button>`).join("")}
      </div>${data.truncated ? `<p>${esc(t("port.folderLimit"))}</p>` : ""}</div>
      <div class="modal-actions"><button class="btn" id="port-back">${esc(t("port.back"))}</button>
      <button class="btn btn-primary" id="port-folder-select">${esc(t("port.useFolder"))}</button></div>`);
    $("port-back").onclick = renderPortEditor;
    $("port-folder-select").onclick = () => {
      portEditor.data.files[index].directory = path;
      portEditor.data.files[index].members = [];
      portEditor.data.detail = t("port.preview");
      portEditor.saved = false; renderPortEditor();
    };
    document.querySelectorAll("[data-port-folder]").forEach(button => button.onclick = () => showPortFolders(index, button.dataset.portFolder));
  } catch (error) { toast(error.message, true); }
}

function confirmRemove(id) {
  const found = state.tasks.find((x) => x.id === id);
  openModal(`<div class="modal-body">
      <h3>${t("task.remove")}?</h3>
      <div class="modal-sub">${esc(found ? (found.name || id) : id)}</div>
      <label class="modal-check"><input type="checkbox" id="rm-data"> ${t("remove.data")}</label>
    </div>
    <div class="modal-actions">
      <button class="btn" id="rm-cancel">${t("game.close")}</button>
      <button class="btn btn-danger" id="rm-ok">${t("task.remove")}</button>
    </div>`);
  $("rm-cancel").onclick = closeModal;
  $("rm-ok").onclick = async () => {
    const resp = await postJson(`/api/tasks/${id}/remove`, { deleteData: $("rm-data").checked });
    closeModal();
    if (!resp.ok && resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(explainRejection(body.error) || "remove failed", true);
    }
    if (resp.status !== 401) await reloadTasks();
  };
}

/* ================= CATALOG ================= */
let catalog = null, catalogFiltered = [], catalogShown = 0;
let catalogSection = "games";
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
    status.textContent = t("catalog.failed") + ": " + err.message;
  }
}

function uniqSorted(vals) {
  return [...new Set(vals.filter(Boolean))].sort((a, b) => String(a).localeCompare(String(b)));
}
function buildCatalogFilters() {
  const section = catalog.filter(e => e.section === catalogSection);
  const genres = uniqSorted(section.map((e) => (e.genre || "").split(/[,/;]/)[0].trim()));
  const years = uniqSorted(section.map((e) => e.year)).reverse();
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
  const total = catalog ? catalog.filter(e => e.section === catalogSection).length : 0;
  const shown = catalogFiltered.length;
  $("catalog-count").textContent = total
    ? tf("catalog.count", shown, total)
    : "";
}

function applyCatalogFilter() {
  const q = $("search").value.trim().toLowerCase();
  const genre = $("flt-genre").value;
  const year = $("flt-year").value;
  const size = $("flt-size").value;
  const sort = $("flt-sort").value;
  catalogFiltered = catalog.filter((e) => {
    if (e.section !== catalogSection) return false;
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

for (const id of ["catalog-games", "catalog-ports"]) $(id).addEventListener("click", () => {
  catalogSection = $(id).dataset.section;
  for (const buttonId of ["catalog-games", "catalog-ports"]) {
    const active = $(buttonId).dataset.section === catalogSection;
    $(buttonId).classList.toggle("btn-primary", active);
    $(buttonId).setAttribute("aria-pressed", String(active));
  }
  if (catalog) { buildCatalogFilters(); applyCatalogFilter(); }
});

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
  const generation = ++descriptionGeneration;
  const requestedLanguage = lang;
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
      <div class="modal-desc" id="game-description">${esc(entry.description || "")}</div>
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
  fetch(`/api/catalog/description?hash=${encodeURIComponent(entry.infoHash)}&lang=${encodeURIComponent(requestedLanguage)}`, { cache: "no-store" })
    .then(resp => resp.ok ? resp.json() : null)
    .then(body => {
      const description = $("game-description");
      if (body && description && $("modal").open &&
          generation === descriptionGeneration && lang === requestedLanguage)
        description.textContent = body.description || "";
    }).catch(() => {}); // Original prose remains usable when an old NRO serves the page.
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
let updateMaxBytes = 64 * 1024 * 1024;

async function loadUpdateStatus() {
  const status = $("update-status");
  try {
    const resp = await api("/api/update", { cache: "no-store" });
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) {
      if (resp.status !== 401) status.textContent = body.error || "update status unavailable";
      return;
    }
    updateMaxBytes = Number(body.maxUploadBytes) || updateMaxBytes;
    const current = tf("update.current", body.currentVersion || "?");
    status.textContent = body.stagedReady
      ? `${current} · ${tf("update.ready", fmtBytes(body.stagedBytes || 0))}`
      : `${current} · ${t("update.none")}`;
    $("update-apply").hidden = !body.stagedReady;
  } catch (e) {
    status.textContent = e.message || "update status unavailable";
  }
}

$("update-file")?.addEventListener("change", () => {
  $("update-file-name").textContent = $("update-file").files[0]?.name || "";
});

$("update-upload")?.addEventListener("click", async () => {
  const file = $("update-file").files[0];
  if (!file) return toast("pipensx.nro", true);
  if (!/\.nro$/i.test(file.name)) return toast("Choose a .nro file", true);
  if (file.size > updateMaxBytes) return toast("NRO is too large", true);
  $("update-upload").disabled = true;
  $("update-apply").hidden = true;
  $("update-status").textContent = t("update.uploading");
  try {
    const resp = await api("/api/update/upload", {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: file,
    });
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) return toast(body.error || "update upload failed", true);
    toast(t("update.ready").replace("{0}", fmtBytes(body.stagedBytes || file.size)));
    await loadUpdateStatus();
  } finally {
    $("update-upload").disabled = false;
  }
});

$("update-apply")?.addEventListener("click", async () => {
  if (!$("control-dialog").hidden) return;
  if (!await controlDialog(t("update.apply"), t("control.updateHelp"))) return;
  $("update-upload").disabled = true;
  $("update-apply").disabled = true;
  try {
    const resp = await api("/api/update/apply", { method: "POST", body: "" });
    const body = await resp.json().catch(() => ({}));
    if (!resp.ok) {
      toast(body.error || "update apply failed", true);
      $("update-upload").disabled = false;
      $("update-apply").disabled = false;
      return;
    }
    $("update-status").textContent = t("update.applying");
  } catch (e) {
    toast(e.message || "update apply failed", true);
    $("update-upload").disabled = false;
    $("update-apply").disabled = false;
  }
});

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
  loadUpdateStatus();
  const resp = await api("/api/settings");
  if (!resp.ok) {
    if (resp.status !== 401) {
      const body = await resp.json().catch(() => ({}));
      toast(body.error || "failed to load settings", true);
    }
    return;
  }
  const s = await resp.json();
  if (s.language && s.language !== "auto") {
    lang = s.language === "en-US" ? "en" : s.language;
    localStorage.setItem("pipensxLang", lang);
    syncAppearanceControls();
    applyI18n();
  }
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
  $("set-alldebrid").value = "";
  $("set-torbox").placeholder = s.torboxConfigured ? "••••••••" : "";
  $("set-realdebrid").placeholder = s.realdebridConfigured ? "••••••••" : "";
  $("set-alldebrid").placeholder = s.alldebridConfigured ? "••••••••" : "";
  settingsLoaded = true;
}
$("settings-save").addEventListener("click", async () => {
  if (!settingsLoaded) return toast("…", true);
  const patch = {
    language: lang === "en" ? "en-US" : lang,
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
  const alldebrid = $("set-alldebrid").value;
  if (torbox) patch.torboxApiKey = torbox;
  if (realdebrid) patch.realdebridApiKey = realdebrid;
  if (alldebrid) patch.alldebridApiKey = alldebrid;
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

/* ---------- installed library ---------- */
let library = null, libraryLoading = false;
const librarySeenJobs = new Set();
function libraryVersion(value) {
  if (!/^\d+$/.test(value || "")) return t("library.unknown");
  return `v${value}`;
}
function renderLibrary() {
  if (!library) return;
  const query = $("library-search").value.trim().toLowerCase();
  const updates = $("library-filter").value === "updates";
  const rows = (library.titles || []).filter(row =>
    (!query || `${row.name} ${row.titleId}`.toLowerCase().includes(query)) &&
    (!updates || row.updateState === "update_available"));
  const busy = new Set((library.jobs || []).filter(job => ["queued", "working"].includes(job.state)).map(job => job.titleId));
  $("library-status").textContent = tf("library.count", rows.length, (library.titles || []).length);
  $("library-list").innerHTML = rows.length ? rows.map(row => `
    <article class="card library-game">
      <div class="library-game-info"><h2>${esc(row.name || row.titleId)}</h2>
        <p class="library-meta">${esc(row.titleId)} · ${esc(row.publisher || "")}</p>
        <p>${esc(t("library.current"))}: ${esc(row.displayVersion || libraryVersion(row.version))}</p>
        <p>${esc(t("library." + row.updateState))}${row.latestVersion ? ` · ${esc(libraryVersion(row.latestVersion))}` : ""}</p>
        <p class="library-meta">${esc(tf("library.dlc", row.dlcCount || 0))}${row.hasLayeredFsMods ? ` · ${esc(t("library.mods"))}` : ""}</p>
      </div><div class="library-actions">
        ${row.updateState === "update_available" && row.updateHash ? `<button class="btn btn-primary" data-library="${esc(row.titleId)}" data-library-action="update" ${busy.has(row.titleId) ? "disabled" : ""}>${esc(t("library.update"))}</button>` : ""}
        <button class="btn btn-danger" data-library="${esc(row.titleId)}" data-library-action="uninstall" ${busy.has(row.titleId) ? "disabled" : ""}>${esc(t("library.uninstall"))}</button>
      </div></article>`).join("") : `<div class="empty">${esc(t("library.empty"))}</div>`;
  $("library-jobs").innerHTML = (library.jobs || []).slice(-5).map(job =>
    `<p class="library-job ${job.state === "error" ? "library-error" : ""}">${esc(t("library." + job.action))} · ${esc(t("library.job." + job.state))}${job.error ? `: ${esc(job.error)}` : ""}</p>`).join("");
  $("library-update-all").disabled = !(library.titles || []).some(row => row.updateState === "update_available" && row.updateHash && !row.hasLayeredFsMods);
}
async function loadLibrary() {
  if (libraryLoading) return;
  libraryLoading = true;
  if (!library) $("library-status").textContent = t("library.loading");
  try {
    const response = await api("/api/library", { cache: "no-store" });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "HTTP " + response.status);
    library = data;
    renderLibrary();
    for (const job of data.jobs || []) if (["done", "error"].includes(job.state) && !librarySeenJobs.has(job.jobId)) {
      librarySeenJobs.add(job.jobId);
      toast(t("library.job." + job.state) + (job.error ? ": " + job.error : ""), job.state === "error");
    }
  } catch (error) { $("library-status").textContent = `${t("library.failed")}: ${error.message}`; }
  finally { libraryLoading = false; }
}
async function libraryCommand(titleId, action, options = {}) {
  const path = action === "refresh" ? "/api/library/refresh" : `/api/library/${encodeURIComponent(titleId)}/${action}`;
  const response = await api(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(options) });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.error || "HTTP " + response.status);
  return body;
}
$("library-search").addEventListener("input", renderLibrary);
$("library-filter").addEventListener("change", renderLibrary);
$("library-refresh").addEventListener("click", async () => {
  $("library-refresh").disabled = true;
  try { await libraryCommand("", "refresh"); await loadLibrary(); }
  catch (error) { toast(error.message, true); }
  finally { $("library-refresh").disabled = false; }
});
$("library-list").addEventListener("click", async event => {
  const button = event.target.closest("[data-library-action]");
  if (!button) return;
  const row = library?.titles?.find(item => item.titleId === button.dataset.library);
  if (!row) return;
  const action = button.dataset.libraryAction;
  const message = action === "uninstall" ? tf("library.uninstallAsk", row.name, row.titleId) :
    tf("library.updateAsk", row.name, libraryVersion(row.version), libraryVersion(row.latestVersion)) +
    (row.hasLayeredFsMods ? "\n\n" + t("library.modsWarning") : "");
  if (!await controlDialog(t("library." + action), message, false, t("library." + action))) return;
  button.disabled = true;
  try {
    await libraryCommand(row.titleId, action, { confirm: true, acknowledgeMods: row.hasLayeredFsMods });
    toast(t(action === "update" ? "library.updateQueued" : "library.job.queued"));
    await loadLibrary();
  } catch (error) { toast(error.message, true); button.disabled = false; }
});
$("library-update-all").addEventListener("click", async () => {
  const candidates = (library?.titles || []).filter(row => row.updateState === "update_available" && row.updateHash && !row.hasLayeredFsMods);
  if (!candidates.length || !await controlDialog(t("library.updateAll"), tf("library.updateAllAsk", candidates.length), false, t("library.updateAll"))) return;
  $("library-update-all").disabled = true;
  let queued = 0;
  for (const row of candidates) {
    try { await libraryCommand(row.titleId, "update"); ++queued; }
    catch (error) { toast(`${row.name}: ${error.message}`, true); break; }
  }
  toast(tf("library.queuedCount", queued));
  await loadLibrary();
});
setInterval(() => { if (currentTab() === "library" && !document.hidden) loadLibrary(); }, 3000);

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
