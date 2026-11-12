# Web companion

Open the address or QR code in Switch Settings → Web companion while pipensx
is running. The page works on phones and computers without external scripts,
fonts, or a frontend build. Enter the console's PIN when prompted.

The initial page is Downloads when a task or add job exists, otherwise Catalog.
Navigation contains Downloads, Catalog, Installed, Settings, and Maintenance;
Add download remains available in the header. AutoRun is hidden in the browser.
Connection, version, source, and free SD space come from the console.
After a connection failure the page labels the displayed data as potentially stale.

## Catalog and installed titles

- Catalog → Games / Ports uses the same classification as the native app.
  Filters and counts apply to the selected section.
- Favorites use the console's persisted favorites service. Multiple selected
  entries can be installed or downloaded in sequence; failures retain the
  affected selections and list their errors.
- A game's Files action opens embedded metainfo when available. An uploaded
  .torrent opens a file chooser. Each file supports Skip, Download, and, when
  the native planner permits it, Install. The server validates choices against
  the torrent or provider file list before importing.
  A release without embedded metainfo can still use automatic Install/Download;
  upload its .torrent to choose individual files.
- Refresh reloads the console's current catalog snapshot. Download catalog
  fetches the configured source in a worker and publishes it through the same
  adoption path as Switch refresh. Catalog generation changes invalidate the
  browser's cached snapshot.
- Installed lists applications, installed versions, and DLC counts. Check
  updates refreshes the installed library and compares it with local catalog
  metadata, using the native update checker. Unknown versions are not offered
  as verified updates.
- Install update preserves the installed base through native smart package
  selection and can select missing bundled DLC. Update all queues verified
  updates; applications with mods require individual review.
- Uninstall shows the application name and title ID for confirmation.
  Ports recorded by deployment receipts use the native port uninstaller: the
  confirmation includes deployed file counts, bytes and whole folders before
  removing the shortcut, deployed files and retained task data. Unrecorded files
  are outside this receipt-based removal.
- The interface has eight offline languages. Browser theme/language are local
  preferences. Separate Switch controls persist console settings; its language
  and theme take effect on the next application launch.

Opening Catalog or Installed does not add torrents to a debrid account.

## Downloads and settings

The queue supports pause/resume, retry, verification, ordering, removal with
optional data deletion, and separate clearing of completed entries or their
downloaded files. Details list the file inventory and native action availability.
Port destinations retain the preview/save/start flow and SD folder browser.
SSE supplies live queue state without a second mirrored activity panel.

Settings include install target, concurrent downloads, file selection defaults,
completed-task visibility, transfer screen blanking, sleep after completion,
direct BitTorrent, sharing, all four debrid providers, keeping uncached tasks,
proxy, catalog source, and diagnostics. API keys remain write-only: blank keeps
a saved key; Remove saved key explicitly clears it.

## Local NRO replacement and maintenance

Catalog → Update through this device downloads the configured catalog URL
in the browser and uploads the JSON to the console. With no custom URL, it
uses the Langegen catalog for the selected language. A local JSON file can
also be uploaded (up to 48 MiB). Import validates and saves the catalog before
publishing the new snapshot; cached statistics and collections are retained.

Maintenance → Download latest pipensx through this device fetches the stable
release and its checksum through the browser. The console verifies SHA-256
before staging the NRO. Use the existing Replace and restart action to apply it.

These downloads use the phone or PC connection, including its VPN. The VPN must
allow local network addresses to reach the console. Sources that deny browser
requests through CORS require downloading the file separately and uploading it
with the catalog or NRO file picker.

Maintenance accepts a local NRO of an older, identical, or newer version.
There is no version-order restriction on local uploads. The page checks the
NRO header and size limit, validates the PIN before uploading, shows upload
progress, and displays the active target path and server verification result.
A staged file can be discarded before application.

Apply schedules the existing updater helper, closes pipensx, and replaces the
active NRO with a backup/transaction. The helper returns to HOME; **open pipensx
again on the Switch**. Relaunching from the same loader session is deliberately
avoided because that path crashes. The browser polls for the returning server,
including a new server instance for same-version replacements, and reloads its
assets. A launch method without next-load support cannot apply the replacement.

Storage scanning and cleanup use the native cache, orphan-data, temporary-file,
NCM garbage, and unused-ticket services. Cleanup runs in the shared service
worker and requires explicit confirmation. Tickets are a separate opt-in
operation because archived eShop purchases can still need them.
Logs support filtering, optional polling, copying on LAN HTTP, and download.

## API

Library reads and commands require the PIN and same-origin requests.
GET /api/library returns titles, generation, and asynchronous jobs.
POST /api/library/refresh refreshes installed titles.
POST /api/library/&lt;16-hex-title-id&gt;/update queues an update; modded games
require {"acknowledgeMods":true}.
POST /api/library/&lt;16-hex-title-id&gt;/uninstall requires {"confirm":true}.
Service jobs are serialized, deduplicated while active, and bounded to 16 jobs.

Additional PIN-protected endpoints:

- GET/POST /api/favorites: fetch hashes or toggle an infoHash.
- GET /api/tasks/&lt;id&gt;/files: file inventory and destinations.
- POST /api/preview/torrent: raw metainfo upload.
- POST /api/preview/catalog: embedded metainfo for an infoHash.
- POST /api/add/prepared: preview token and files with path/action pairs;
  0/1/2 are Skip/Download/Install. Import runs on the add worker.
- POST /api/preview/discard: release a preview token. Previews expire after
  ten minutes; at most four are retained per server.
- POST /api/catalog/refresh: background catalog fetch; job state is in /api/library.
- POST /api/catalog/upload: raw catalog JSON, requires the PIN and same origin.
- GET /api/update, POST /api/update/upload, /apply, /discard: local NRO lifecycle.
- GET /api/maintenance: last storage snapshot and job state.
- POST /api/maintenance/&lt;action&gt;: scan, images, torrents, downloads,
  temporary, system, or tickets; all except scan require {"confirm":true}.

The /api/info response advertises available services. Controls for absent
services are hidden.

## Verification

Run the native suite, Switch build and locale checks described in
[AGENTS.md](../AGENTS.md). HTTP tests exercise PIN/origin protection, settings
validation, file preview/import, and service jobs.

For browser checks, install Playwright and its Chromium browser in your test
environment, then run:

    node tests/web_companion.cjs

If Playwright lives outside the repository, set PLAYWRIGHT_MODULE to its
installed module path. The test starts its own static server and intercepts API
calls with explicit fixtures. It checks queue actions, selection, favorites,
batch installation, library operations, credentials, storage cleanup, logs,
NRO upload/reconnection, both theme color palettes, and all six pages at
320, 375, 740, 1024, and 1440 pixels.
These checks do not replace testing installation or NRO replacement on hardware.

### UI delivery gate, 2026-11-09

Direction: the agreed Switch palette and artwork, compact queue rows, separate
form groups, and restrained motion. ENERGY 1 / RHYTHM 2 / MOTION 1.

- Hard Gate PASS: native controls and dialogs, loading/empty/error states,
  existing artwork only, server-supplied figures, and no decorative profiles.
  Browser tests cover five widths, English/Russian at 200% text size, and color
  contrast assertions for both palettes. Text pairs meet 4.5:1; component
  boundaries meet 3:1.
- Purpose-Gate PASS: the accent marks navigation, primary actions, and measured
  progress. The catalog grid serves cover browsing; queue rows expose transfer
  state; form groups separate browser preferences from console settings.
- Liveliness PASS: the existing pipensx mark, cyan accent, game artwork, and
  installation-specific content retain product identity. The grid, queue, and
  settings use different compositions; there are no decorative entrance effects.
- Craftsmanship PASS: the PC suite, Switch build, locale checks, and browser
  interaction tests passed. Browser data is explicitly mocked in the test;
  physical-console install, cleanup, and NRO replacement remain untested.
