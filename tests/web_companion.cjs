const assert = require("node:assert/strict");
const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || "playwright");

const root = path.resolve(__dirname, "../resources/web");
const server = http.createServer((request, response) => {
  const name = new URL(request.url, "http://localhost").pathname;
  const file = path.join(root, name === "/" ? "index.html" : name);
  if (!file.startsWith(root + path.sep)) { response.writeHead(403).end(); return; }
  fs.readFile(file, (error, data) => {
    if (error) { response.writeHead(404).end(); return; }
    response.setHeader("Content-Type", file.endsWith(".js") ? "application/javascript" :
      file.endsWith(".css") ? "text/css" : file.endsWith(".png") ? "image/png" : "text/html");
    response.end(data);
  });
});

(async () => {
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 }, locale: "en-US" });
  const page = await context.newPage();
  const errors = [], calls = [];
  page.on("pageerror", error => errors.push(error.message));
  const hash = "a".repeat(40);
  const capabilities = Object.fromEntries(["pause", "resume", "verify", "remove", "move"].map(name => [name, { allowed: true }]));
  const state = { version: "test-1", catalogGeneration: 1, tasks: [{
    id: hash, name: "Example download", status: "Downloading", mode: "download", totalBytes: 1000,
    completedBytes: 500, speedBps: 100, capabilities
  }], jobs: [], summary: { remainingBytes: 500, downloadSpeedBps: 100 }, storage: { available: true, freeBytes: 20 * 1024 ** 3 }, source: { direct: true } };
  let settings = { language: "en-US", theme: "dark", maxActiveDownloads: 1, debridProvider: "torbox",
    streamSelection: "allFiles", installLocation: "sdCard", torboxConfigured: true, torrentingEnabled: true };
  let favorite = false, failCatalog = false, failMagnet = false, applied = false;
  let update = { currentVersion: "test-1", targetPath: "sdmc:/switch/pipensx/pipensx.nro", stagedReady: false, canApply: true };
  const catalog = Array.from({ length: 54 }, (_, index) => ({
    infoHash: index.toString(16).padStart(40, "0"), title: "Game " + String(index).padStart(2, "0"),
    section: "games", year: 2024, size: 1024 ** 3, genre: "Adventure", posterUrl: "/icon-192.png",
    description: "Description from the catalog.", screenshots: ["/icon-192.png"]
  }));
  catalog.push({ infoHash: "b".repeat(40), title: "Example port", section: "ports", size: 1024 });
  catalog.push({ infoHash: "c".repeat(40), title: "Unfinished AutoRun", section: "autorun" });
  let titles = [
    { titleId: "0100000000001000", name: "Installed game", displayVersion: "1.0", latestVersion: "2.0", updateState: "update_available", updateHash: hash, dlcCount: 1 },
    { titleId: "0100000000002000", name: "Installed port", isPort: true, updateState: "up_to_date", deployedFiles: 4, deployedBytes: 1024, wholeFolders: ["port-data"] }
  ];
  const prepared = { token: "1", name: "File selection", files: [{ path: "game.nsp", size: 1024, installable: true }, { path: "notes.txt", size: 12, installable: false }] };
  await page.route("**/api/**", async route => {
    const request = route.request(), url = new URL(request.url()), name = url.pathname;
    const body = request.headers()["content-type"]?.includes("application/json") ? request.postDataJSON() : null;
    calls.push({ name, method: request.method(), body });
    const fulfill = (json, status = 200) => route.fulfill({ status, json });
    if (name === "/api/events") return route.abort();
    const publicRead = request.method() === "GET" && ["/api/info", "/api/tasks", "/api/catalog", "/api/catalog/description"].includes(name);
    if (!publicRead && request.headers()["x-pipensx-pin"] !== "1234") return fulfill({ error: "pin" }, 401);
    if (name === "/api/info") return fulfill({ version: applied ? "test-2" : "test-1", instanceId: applied ? 2 : 1,
      features: { favorites: true, library: true, ports: true, update: true, catalogRefresh: true, maintenance: true } });
    if (name === "/api/tasks") return fulfill(state);
    if (name === "/api/catalog") return failCatalog ? fulfill({ error: "catalog unavailable" }, 503) : fulfill(catalog);
    if (name === "/api/catalog/description") return fulfill({ description: "Localized catalog description." });
    if (name === "/api/catalog/refresh") return fulfill({ jobId: "catalog-job" }, 202);
    if (name === "/api/favorites") {
      if (request.method() === "POST") favorite = !favorite;
      return fulfill({ hashes: favorite ? [body?.infoHash || catalog[0].infoHash] : [] });
    }
    if (name.endsWith("/files")) return fulfill({ rootPath: "sdmc:/downloads/example", complete: true,
      files: [{ path: "game.nsp", size: 1000, action: "download", state: "present", localPath: "game.nsp" }] });
    if (name.startsWith("/api/tasks/")) {
      const command = name.split("/").pop();
      if (command === "pause") state.tasks[0].status = "Paused";
      if (command === "resume") state.tasks[0].status = "Downloading";
      if (command === "remove") state.tasks = [];
      return fulfill({});
    }
    if (name.startsWith("/api/queue/")) return fulfill({ cleared: 1, skippedBusy: 0 });
    if (name === "/api/settings") {
      if (request.method() === "PATCH") settings = { ...settings, ...body };
      return fulfill(settings);
    }
    if (name === "/api/maintenance") return fulfill({ storage: { images: 100, temporary: 100, torrents: 100,
      orphanDownloads: 100, systemBytes: 100, systemItems: 2, tickets: 1, systemAvailable: true, ticketsAvailable: true },
      jobs: [{ jobId: "maintenance-job", state: "done" }] });
    if (name.startsWith("/api/maintenance/")) return fulfill({ jobId: "maintenance-job" }, 202);
    if (name === "/api/library") return fulfill({ titles, jobs: [{ jobId: "catalog-job", action: "catalog-refresh", state: "done" }] });
    if (name.startsWith("/api/library/")) {
      if (name.endsWith("/uninstall")) titles = titles.filter(title => !name.includes(title.titleId));
      return fulfill({ jobId: "library-job" }, 202);
    }
    if (name.startsWith("/api/preview/")) return fulfill(prepared);
    if (name === "/api/add/magnet" && failMagnet) return fulfill({ error: "Magnet rejected" }, 409);
    if (name.startsWith("/api/add/")) return fulfill({ jobId: "add-job" }, 202);
    if (name === "/api/update") return fulfill(update);
    if (name === "/api/update/upload") { update = { ...update, stagedReady: true, stagedBytes: 144 }; return fulfill(update); }
    if (name === "/api/update/discard") { update = { ...update, stagedReady: false }; return fulfill(update); }
    if (name === "/api/update/apply") {
      applied = true; state.version = "test-2";
      update = { ...update, stagedReady: false, currentVersion: "test-2" };
      return fulfill({ applying: true }, 202);
    }
    if (name === "/api/auth/check") return fulfill({});
    if (name === "/api/logs") return route.fulfill({ contentType: "text/plain", body: "[install] ready\n[error] test error\n[telemetry] speed=100" });
    if (name.startsWith("/api/ports/")) {
      if (name.includes("folders")) return fulfill({ folders: ["/switch"], truncated: false });
      return fulfill({ files: [{ source: "port.nro", size: 100, enabled: true, available: true, directory: "/switch", members: [] }],
        editable: true, manual: true, canStart: true });
    }
    throw new Error("Uncovered API call: " + name);
  });
  const base = "http://127.0.0.1:" + server.address().port;
  await page.goto(base + "/?pin=1234");
  await page.waitForSelector(".task");
  assert.equal(await page.locator("h1:visible").textContent(), "Downloads");
  const downloadTask = { ...state.tasks[0] };
  Object.assign(state.tasks[0], {
    status: "Installing", mode: "stream", currentPackage: "update.NSZ",
    recovery: { work: "reread", reworkBytes: 8 * 1024 ** 3, packagesInstalled: 1 }
  });
  await page.evaluate(() => reloadTasks());
  assert.match(await page.locator(".task").textContent(), /8 GiB processed.*already installed: 1/);
  assert.match(await page.locator(".task").textContent(), /NSZ is unpacked/);
  assert.doesNotMatch(await page.locator(".task").textContent(), /restarts|read again/);
  for (const locale of ["en", "ru", "pt-BR", "fr", "es", "it", "zh-CN", "ko"]) {
    await page.evaluate(locale => { lang = locale; renderDownloads(); }, locale);
    for (const width of [320, 1440]) {
      await page.setViewportSize({ width, height: 900 });
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false,
        "NSZ notice overflows in " + locale + " at " + width);
    }
  }
  await page.evaluate(() => { lang = "en"; renderDownloads(); });
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.locator('[data-command="pause"]').click();
  await page.waitForFunction(() => document.querySelector(".task-state")?.textContent === "Paused");
  assert.match(await page.locator(".task").textContent(), /only the unfinished package restarts.*kept installed: 1/);
  assert.doesNotMatch(await page.locator(".task").textContent(), /NSZ is unpacked/);
  state.tasks[0].status = "Committing";
  state.tasks[0].currentPackage = "update.nsp";
  await page.evaluate(() => reloadTasks());
  assert.match(await page.locator(".task").textContent(), /8 GiB processed/);
  assert.doesNotMatch(await page.locator(".task").textContent(), /NSZ is unpacked|restarts/);
  state.tasks[0].currentPackage = "update.nsz";
  state.tasks[0].recovery.work = "package";
  await page.evaluate(() => reloadTasks());
  assert.match(await page.locator(".task").textContent(), /NSZ is unpacked/);
  assert.doesNotMatch(await page.locator(".task").textContent(), /restarts/);
  state.tasks[0] = downloadTask;
  await page.evaluate(() => reloadTasks());
  await page.locator('[data-command="pause"]').click();
  await page.waitForFunction(() => document.querySelector(".task-state")?.textContent === "Paused");
  await page.locator('[data-command="resume"]').click();
  await page.locator('[data-command="details"]').first().click();
  await page.waitForSelector(".file-list li");
  await page.locator('[data-command="verify"]').click();
  await page.locator('[data-command="move-up"]').click();
  await page.locator('[data-command="move-down"]').click();
  await page.locator('[data-command="move-front"]').click();
  await page.locator('[data-command="port-files"]').click();
  await page.locator("#port-preview").click();
  await page.locator("#port-save").click();
  await page.locator("[data-port-browse]").click();
  await page.locator("[data-port-folder]").click();
  await page.locator("#port-folder-select").click();
  await page.locator("#port-save").click();
  await page.locator("#port-start").click();
  await page.waitForFunction(() => !document.querySelector("#modal").open);
  await page.locator("#bulk-pause").click();
  await page.locator("#bulk-resume").click();
  state.tasks[0].status = "Completed";
  await page.locator("#downloads-refresh").click();
  for (const id of ["bulk-clear", "bulk-clear-files"]) {
    await page.locator("#" + id).click();
    await page.locator("#control-submit").click();
  }
  state.tasks[0].status = "Downloading";
  await page.locator("#downloads-refresh").click();
  await page.locator('[data-page="catalog"]').click();
  await page.waitForSelector(".game");
  assert.equal(await page.locator(".game").count(), 48);
  await page.locator("#catalog-more").click();
  assert.equal(await page.locator(".game").count(), 54);
  assert.equal(await page.getByText("Unfinished AutoRun", { exact: true }).count(), 0);
  await page.locator("#catalog-search").fill("does not exist");
  assert.match(await page.locator("#catalog-count").textContent(), /0 of 54/);
  await page.locator("#flt-reset").click();
  await page.locator(".game-open").first().click();
  await page.locator("#game-favorite").click();
  await page.waitForFunction(() => document.querySelector("#game-favorite")?.getAttribute("aria-pressed") === "true");
  await page.locator("[data-shot]").click();
  await page.locator("#shot-back").click();
  await page.locator("#game-files").click();
  await page.locator('[data-file-choice="0"]').selectOption("0");
  await page.locator('[data-file-choice="1"]').selectOption("0");
  await page.locator("#file-choice-add").click();
  await page.waitForFunction(() => document.querySelector("#toast")?.textContent === "Select at least one file.");
  await page.locator('[data-file-choice="0"]').selectOption("2");
  await page.locator("#file-choice-add").click();
  await page.waitForFunction(() => location.hash === "#downloads");
  const chosen = calls.find(call => call.name === "/api/add/prepared");
  assert.deepEqual(chosen.body.files.map(file => file.action), [2, 0]);
  await page.locator('[data-page="catalog"]').click();
  await page.locator("#catalog-favorites").click();
  assert.equal(await page.locator(".game").count(), 1);
  await page.locator("#catalog-games").click();
  await page.locator("#catalog-select-mode").check();
  await page.locator("[data-select]").nth(0).check();
  await page.locator("[data-select]").nth(1).check();
  await page.locator("#batch-install").click();
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => location.hash === "#downloads");
  assert.equal(calls.filter(call => call.name === "/api/add/catalog").length, 2);
  await page.locator('[data-page="catalog"]').click();
  await page.locator("#catalog-fetch").click();
  await page.waitForFunction(() => !document.querySelector("#catalog-fetch").disabled);
  await page.locator("#catalog-ports").click();
  assert.equal(await page.locator(".game").count(), 1);
  failCatalog = true;
  await page.locator("#catalog-refresh").click();
  await page.waitForFunction(() => document.querySelector("#catalog-status").textContent.includes("catalog unavailable"));
  failCatalog = false;
  await page.locator("#catalog-refresh").click();
  await page.locator('[data-page="library"]').click();
  await page.waitForSelector("[data-library]");
  await page.locator("#library-search").fill("missing");
  assert.equal(await page.locator("#library-list .task").count(), 0);
  await page.locator("#library-search").fill("");
  await page.locator('[data-library-action="update"]').click();
  await page.locator("#control-cancel").click();
  await page.locator("#library-update-all").click();
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => location.hash === "#downloads");
  await page.locator('[data-page="library"]').click();
  await page.locator("#library-filter").selectOption("ports");
  await page.locator('[data-library-action="uninstall"]').click();
  assert.match(await page.locator("#control-message").textContent(), /4 deployed files/);
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => document.querySelectorAll("#library-list .task").length === 0);
  await page.locator('[data-page="settings"]').click();
  await page.waitForSelector("[data-setting]");
  await page.locator("#setting-sleepAfterDownloads").check();
  await page.locator("#setting-blankScreenDuringTransfers").check();
  await page.locator("#clear-torboxApiKey").check();
  await page.locator("#settings-save").click();
  await page.waitForFunction(() => document.querySelector("#settings-status").textContent === "Settings saved");
  assert.equal(settings.sleepAfterDownloads, true);
  assert.equal(settings.torboxApiKey, "");
  await page.locator("#browser-language").selectOption("ru");
  await page.locator("#browser-theme").selectOption("dark");
  assert.equal(await page.locator("html").getAttribute("lang"), "ru");
  await page.locator("#browser-language").selectOption("en");
  await page.evaluate(() => localStorage.removeItem("pipensxPin"));
  await page.locator("#settings-reload").click();
  await page.waitForSelector("#control-dialog[open]");
  await page.locator("#control-pin").fill("9999");
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => document.querySelector("#settings-status").textContent.includes("Incorrect PIN"));
  await page.locator("#settings-reload").click();
  await page.waitForSelector("#control-dialog[open]");
  await page.locator("#control-pin").fill("1234");
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => document.querySelector("#settings-status").textContent === "");
  await page.locator(".topbar a").click();
  failMagnet = true;
  await page.locator("#magnet-input").fill("magnet:?xt=urn:btih:" + hash);
  await page.locator('#magnet-form button[type="submit"]').click();
  await page.waitForFunction(() => document.querySelector("#add-status").textContent.includes("Magnet rejected"));
  failMagnet = false;
  await page.locator('#magnet-form button[type="submit"]').click();
  await page.waitForFunction(() => location.hash === "#downloads");
  await page.locator(".topbar a").click();
  await page.locator("#torrent-file").setInputFiles({ name: "example.torrent", mimeType: "application/x-bittorrent", buffer: Buffer.from("fixture") });
  await page.locator('#torrent-form button[type="submit"]').click();
  await page.waitForSelector("#file-choice-add");
  await page.keyboard.press("Escape");
  await page.locator('[data-page="maintenance"]').click();
  await page.locator("#storage-scan").click();
  await page.waitForFunction(() => document.querySelector("#storage-status").textContent === "Storage scan complete.");
  for (const action of ["images", "torrents", "downloads", "temporary", "system", "tickets"]) {
    await page.locator('[data-storage="' + action + '"]').click();
    await page.locator("#control-submit").click();
    await page.waitForFunction(() => document.querySelector("#storage-status").textContent.includes("Cleanup complete"));
  }
  await page.waitForFunction(() => document.querySelector("#log-output").textContent.includes("test error"));
  await page.locator("#log-filter").selectOption("errors");
  assert.equal((await page.locator("#log-output").textContent()).trim(), "[error] test error");
  await page.evaluate(() => Object.defineProperty(window, "isSecureContext", { value: false, configurable: true }));
  await page.locator("#log-copy").click();
  await page.locator("#log-download").click();
  await page.locator("#update-file").setInputFiles({ name: "bad.nro", mimeType: "application/octet-stream", buffer: Buffer.from("invalid") });
  await page.locator("#update-upload").click();
  await page.waitForFunction(() => document.querySelector("#update-status").textContent === "Choose a valid .nro file.");
  const nro = Buffer.alloc(144); nro.write("NRO0", 0x10); nro.writeUInt32LE(128, 0x18);
  await page.locator("#update-file").setInputFiles({ name: "pipensx.nro", mimeType: "application/octet-stream", buffer: nro });
  await page.locator("#update-upload").click();
  await page.waitForSelector("#update-apply:visible");
  await page.locator("#update-discard").click();
  await page.waitForFunction(() => document.querySelector("#update-apply").hidden);
  await page.locator("#update-file").setInputFiles({ name: "pipensx.nro", mimeType: "application/octet-stream", buffer: nro });
  await page.locator("#update-upload").click();
  await page.waitForSelector("#update-apply:visible");
  for (const width of [320, 375, 740, 1024, 1440]) {
    await page.setViewportSize({ width, height: 900 });
    for (const name of ["downloads", "catalog", "library", "settings", "maintenance", "add"]) {
      await page.evaluate(name => { location.hash = "#" + name; }, name);
      await page.waitForSelector("#view-" + name + ":visible");
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, name + " overflows at " + width);
    }
  }
  const luminance = hex => {
    let value = hex.trim().replace("#", "");
    if (value.length === 3) value = [...value].map(channel => channel + channel).join("");
    const rgb = value.match(/../g).map(value => {
      const channel = parseInt(value, 16) / 255;
      return channel <= .04045 ? channel / 12.92 : ((channel + .055) / 1.055) ** 2.4;
    });
    return rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722;
  };
  const contrast = (a, b) => (Math.max(luminance(a), luminance(b)) + .05) / (Math.min(luminance(a), luminance(b)) + .05);
  for (const theme of ["light", "dark"]) {
    await page.evaluate(theme => document.documentElement.dataset.theme = theme, theme);
    const colors = await page.evaluate(() => {
      const css = getComputedStyle(document.documentElement);
      return Object.fromEntries(["text", "muted", "bg", "surface", "raised", "accent", "accent-ink", "error", "ok", "line"].map(key => [key, css.getPropertyValue("--" + key)]));
    });
    for (const foreground of ["text", "muted", "error", "ok"]) for (const background of ["bg", "surface", "raised"])
      assert.ok(contrast(colors[foreground], colors[background]) >= 4.5, theme + ": " + foreground + " on " + background);
    assert.ok(contrast(colors["accent-ink"], colors.accent) >= 4.5, theme + " primary button");
    for (const background of ["bg", "surface", "raised"])
      assert.ok(contrast(colors.line, colors[background]) >= 3, theme + " component boundary on " + background);
  }
  await page.setViewportSize({ width: 375, height: 812 });
  await page.evaluate(() => { document.documentElement.style.fontSize = "30px"; });
  for (const name of ["downloads", "catalog", "library", "settings", "maintenance", "add"]) {
    await page.evaluate(name => { location.hash = "#" + name; }, name);
    await page.waitForSelector("#view-" + name + ":visible");
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
    if (overflow) {
      console.error(await page.evaluate(() => [...document.querySelectorAll("body *")].filter(element => {
        const rect = element.getBoundingClientRect();
        return rect.width && rect.right > innerWidth + 1;
      }).map(element => ({ tag: element.tagName, id: element.id, class: element.className, text: element.textContent.slice(0, 60) }))));
      await page.screenshot({ path: "/tmp/pipensx-web-overflow.png", fullPage: true });
    }
    assert.equal(overflow, false, name + " overflows at 200% text");
  }
  await page.evaluate(() => { location.hash = "#settings"; });
  await page.waitForSelector("#view-settings:visible");
  await page.locator("#browser-language").selectOption("ru");
  for (const name of ["downloads", "catalog", "library", "settings", "maintenance", "add"]) {
    await page.evaluate(name => { location.hash = "#" + name; }, name);
    await page.waitForSelector("#view-" + name + ":visible");
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false, name + " overflows in Russian at 200% text");
  }
  await page.evaluate(() => { location.hash = "#settings"; });
  await page.waitForSelector("#view-settings:visible");
  await page.locator("#browser-language").selectOption("en");
  await page.evaluate(() => { document.documentElement.style.fontSize = ""; });
  await page.locator("#menu-button").click();
  await page.keyboard.press("Escape");
  assert.equal(await page.locator("#sidebar").evaluate(el => el.inert), true);
  await page.evaluate(() => { location.hash = "#maintenance"; });
  await page.waitForSelector("#view-maintenance:visible");
  await page.locator("#update-apply").click();
  await page.locator("#control-submit").click();
  await page.waitForFunction(() => document.querySelector("#connection-banner").textContent.includes("HOME"));
  await page.waitForFunction(() => document.querySelector("#device-version").textContent === "vtest-2");
  assert.deepEqual(errors, []);
  await browser.close();
  server.close();
  console.log("web companion: queue, file choices, favorites, batch, library, settings, logs, NRO and six pages at five widths passed");
})().catch(error => { console.error(error); server.close(); process.exit(1); });
