---
name: debrid
description: >
  Adding or changing a debrid provider (TorBox, TorrServer, Real-Debrid),
  DebridProvider, *Client/*Provider, debrid UI, first-run debrid cards, or
  persisted settings/download-state versioning in app_settings.cpp and
  download_manager.cpp saveLocked/load.
---

# Debrid providers and persisted state

Three download sources: TorBox (cloud), TorrServer (self-hosted LAN), and
Real-Debrid (cloud). Each implements `DebridProvider` (`src/app/debrid_provider.hpp`),
a pure-virtual interface with eight required methods: `validate`, `createFromMagnet`, `createFromFile`, `fetchInfo`, `selectFiles`, `resolveDownloadUrl`, `remove`, and `name`.

## Adding a new provider

Follow the existing pattern (`TorboxProvider` + `TorboxClient`). Touchpoints:

| Layer | Files |
|-------|-------|
| Provider interface | `src/app/debrid_provider.hpp` — add to `DebridProviderKind` enum |
| Client + provider | `src/app/<name>_client.{hpp,cpp}`, `src/app/<name>_provider.{hpp,cpp}` |
| Settings | `src/app/app_settings.hpp` — add key field; `app_settings.cpp` — serialize/deserialize field and provider kind string |
| Download manager | `download_manager.{hpp,cpp}` — `setXxxKey()`, `apiKeyFor()` branch, `makeProvider()` branch, `saveLocked`/`load` provider name |
| UI helpers | `src/ui/debrid_ui.hpp` — `activeDebridKey()`, `makeDebridProvider()`, `debridProviderName()`, `DebridLinkView` members |
| Settings UI | `src/ui/settings/settings_view.hpp` — selector list and `applyValues()` |
| First-run | `src/ui/first_run_view.hpp` — option card, diagram kind, `updateSelection()` case |
| Switch init | `src/main_switch.cpp` — call `setXxxKey()` on the manager after settings load |
| Build | `CMakeLists.txt` — add to `APP_SERVICE_SOURCES`; `Makefile.pc` — object rule, test targets, link into `test_manager` |
| Locale | `resources/i18n/*/pipensx.json` — first-run and diagram strings |
| Tests | `tests/test_<name>_client.cpp` (static parsers), `tests/test_<name>_provider.cpp` (mock transport) |

## Provider test pattern

Tests inject a `Transport` functor (e.g. `RdTransport`) that returns canned JSON
responses instead of making real HTTP calls. The provider and its client both
accept a transport in their constructors so the curl path is never hit during
testing. The transport matches requests by URL substring (endpoint name) and
returns pre-baked JSON.

## Account cleanup

When a debrid task is removed, `DownloadManager::removeFromDebridAsync` fires
a detached thread that calls `makeProvider(…)->remove(id)` to clean the account.
A detached thread is used because the caller must never block on the HTTPS
round-trip — `mutex_` may be held.

## Settings and state versioning

- **App settings** (`src/app/app_settings.cpp`): `kSettingsVersion` (currently 3).
  Adding a new stored field that requires migration needs a version bump and a
  rule in `parseSettings()`. The old field comment `"realdebrid" was a provider we
  no longer ship` was written before Real-Debrid was added — do not cargo-cult it.
- **Download state** (`src/app/download_manager.cpp`): `saveLocked` writes
  version 6 (bencode `7:versioni6e`). The `load()` function gates optional
  fields behind
  `version.ival >= N`. Adding a new persisted task field requires bumping to 7,
  adding a `version.ival >= 7` load gate, and writing the new key to `saveLocked`.
