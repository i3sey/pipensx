# Documentation map

Start with [README](../README.md) for the product and [BUILD](../BUILD.md) for
build setup. [AGENTS.md](../AGENTS.md) contains repository instructions;
[CONTRIBUTING](../CONTRIBUTING.md) covers changes and pull requests.

## Feature behavior

| Area | Document |
| --- | --- |
| Offline catalog descriptions | [Description packs](OFFLINE_DESCRIPTIONS.md) |
| Browser catalog and installed library | [Remote library](REMOTE_LIBRARY.md) |
| Port destinations and deployment review | [Remote port layout](REMOTE_PORT_LAYOUT.md) |
| Alternative port archives | [Archive selection](PORT_ARCHIVE_ALTERNATIVES.md) |
| Real-Debrid grouped links | [Grouped downloads](REALDEBRID_GROUPED_DOWNLOADS.md) |
| AutoRun dependency fixes | [Compatibility notes](autorun-compatibility.md) |
| Torrent sharing and RAM cache | [Live sharing](LIVE_SEEDING.md) |
| Client identity and build version | [Version resolution](CLIENT_VERSION.md) |

## Plans and measurements

Read these only for work in the relevant area. Plans describe intended behavior;
check source and tests to establish what has shipped.

- [Product roadmap](product-roadmap.md): proposal based on the September 30,
  2026 code snapshot, with links to earlier audits.
- [Live seeding](seeding-ram-research.md): approved target architecture and
  implementation phases, revised October 21, 2026.
- [Sysmodule probe results](plans/SYSMODULE_PROBE_RESULTS.md): hardware
  measurements; these supersede conflicting assumptions in the older plan.
- [Sysmodule phases](plans/SYSMODULE_PHASES.md): implementation plan based on
  those measurements.

## Historical material

- [Dated audits and bug investigations](archive/README.md).
- [Earlier implementation plans](plans/README.md).
- [Catalog UI prototype](mockups/catalog-next/README.md).

Historical observations can explain a decision but do not establish current
bugs or behavior. Track new work in GitHub issues; avoid adding parallel status
reports or copying repository instructions into another agent entry file.
