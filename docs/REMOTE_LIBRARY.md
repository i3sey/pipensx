# Remote catalog and installed games

Open the Switch's companion address on a computer while pipensx is running.

- **Catalog → Games / Ports** uses the same classification as the native app,
  including joined title metadata. Filters and counts apply to the selected section.
- **Installed** lists applications, installed versions and DLC counts. **Check
  updates** refreshes the installed library and compares it with local catalog
  metadata, using the native update checker. Unknown versions are not offered
  as verified updates.
- **Install update** preserves the installed base through the native smart
  package selection. It can also select missing bundled DLC. **Update all**
  queues available updates; applications with mods need individual review.
- **Uninstall** shows the application name and title ID for confirmation and
  removes the installed application through the native uninstall service. Active
  transfers prevent this operation. This does not remove manually copied port
  files from the SD card.
- The interface has eight offline languages. Select the language in Settings;
  saving settings also applies the selected language to the homebrew.

Library reads and commands require the companion PIN and same-origin requests.
`GET /api/library` returns `titles`, `generation` and asynchronous `jobs`.
`POST /api/library/refresh` refreshes installed titles.
`POST /api/library/<16-hex-title-id>/update` queues an update; modded games
require `{"acknowledgeMods":true}`.
`POST /api/library/<16-hex-title-id>/uninstall` requires `{"confirm":true}`.
Refresh and uninstall jobs are serialized, deduplicated while active and bounded
to 16 jobs. Their completion or failure appears in the library and runtime log.
Opening Catalog or Installed does not add torrents to a debrid account.

Validation: `make -f Makefile.pc test`, `make pc`, Switch NRO build,
`python scripts/check_i18n.py`, `python scripts/check_web_i18n.py`.
HTTP tests use mock uninstall callbacks; UI checks use a local fixture server.
No real application is uninstalled as part of verification.
