# Contributing to pipensx

Read [README.md](README.md) for the application overview and
[BUILD.md](BUILD.md) for dependency and build instructions.

## Repository layout

```text
src/core/       Platform-independent torrent engine (C99)
src/app/        Application services and state (C++20)
src/install/    Streaming NSP/NSZ parsers and install backends
src/platform/   Platform integration and crash/performance support
src/ui/         Borealis views and activities
tests/          Unit, integration, fixture, and golden screenshot tests
scripts/        Build, deploy, and golden-test helpers
tools/          Developer diagnostics and the golden runner
docs/plans/     Archived implementation plans, not an active roadmap
vendor/         Pinned source and Git submodule dependencies
```

Keep Switch/Borealis behavior out of `src/core/`. Platform-specific code belongs
under `src/platform/` or behind an install backend.

## Build and test

Initialize dependencies, then use the public top-level targets:

```bash
git submodule update --init --recursive
make test
make golden
```

Changes affecting Switch-only code must also produce
`build-switch/pipensx.nro` with `make switch`. Sleep/wake, long downloads, and
content installation require final validation on physical hardware.

Add tests for new behavior and regressions. Do not commit generated build
outputs, credentials, Nintendo keys, games, firmware, catalog dumps, or metadata
indexes. Run `make audit` when gitleaks is installed.

## Commit conventions

Use [Conventional Commits](https://www.conventionalcommits.org/):

```text
<type>(<optional-scope>): <imperative subject>
```

Supported types are `feat`, `fix`, `perf`, `refactor`, `docs`, `build`, `test`,
`chore`, and `ci`. Keep the subject under 72 characters and explain non-obvious
rationale in the body.

## Pull requests

- Keep each pull request focused on one logical change.
- Include tests and the commands used to validate the change.
- Keep public interfaces and persisted data backward compatible, or document
  the migration explicitly.
- Do not hide failures by weakening assertions, golden thresholds, or compiler
  diagnostics.
- Report vulnerabilities through the private process in
  [SECURITY.md](SECURITY.md), never in a public issue.
