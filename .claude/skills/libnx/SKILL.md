---
name: libnx
description: >
  Rules for touching libnx / Switch-homebrew code in this repo. Use when the
  task involves a libnx call or service (applet, hid/pad, fs, ns, ncm, es, set,
  nifm, psm, spl, svc*, hosversion, romfs), an nx type (Result, Service, Handle,
  NacpStruct, FsFileSystem, NcmContentStorage), the install backend or package
  stream, NRO/NSP/NCA/CNMT/ticket handling, applet-mode or sysmodule
  constraints, devkitPro/devkitA64 toolchain issues, or anything under
  src/install, src/platform, src/main_switch.cpp, vendor/libnx-ext.
---

# libnx in pipensx

Two rules carry almost all the weight: **read the header instead of recalling
the signature**, and **know which of the two builds will see your call**.

## 1. The header on disk is the API, your memory is not

libnx moves, and a plausible-looking `fooInitialize()` that does not exist
costs a full Switch build to discover. Before writing any libnx call, read the
declaration:

```
/opt/devkitpro/libnx/include/switch/services/*.h    # 92 service headers
/opt/devkitpro/libnx/include/switch/kernel/svc.h    # svc*, and kernel/ for threads/events
/opt/devkitpro/libnx/include/switch/{nacp,nro,result,types}.h
grep -rn "ncmContentStorageWritePlaceHolder" /opt/devkitpro/libnx/include/
```

What stock libnx lacks is vendored, not reinvented — check there before adding
anything: `vendor/libnx-ext/libnx-ipcext/` (es, ns-ext, account-ext) and
`vendor/libnx-ext/libnx-ext/`. Both are on the Switch target's include path
only.

For what a call *means* rather than what it takes, WebFetch
`switchbrew.org/wiki/` (allowed in `.claude/settings.json`): per-command
firmware gates, service ownership, error semantics, and the NCA/CNMT/ticket
formats behind `package_stream.cpp`. The header will not tell you that
`ncmContentStorageRevertToPlaceHolder` needs 2.0.0+ — the wiki will.

## 2. Most sources compile twice, and the second build is easy to forget

`CORE_SOURCES`, `APP_SERVICE_SOURCES` and `UI_SOURCES` in `CMakeLists.txt` are
linked into **both** `pipensx` (aarch64, real libnx) and `golden_runner` (PC,
no libnx at all). Only `src/main_switch.cpp`, `src/install/install_backend_switch.cpp`,
`src/platform/switch_*` and the vendored ipcext are Switch-only.

So a libnx call in shared code has to pick one of three existing patterns —
do not invent a fourth:

**a. `#ifdef __SWITCH__` with a PC fallback.** The default; 14 shared files
already do it (`src/app/install_space.cpp`, `src/app/stream_ram_budget.cpp`,
`src/app/update_service.cpp`, `src/ui/settings/settings_view.hpp`,
`src/core/util.c`, `src/install/package_stream.cpp`, …). `__SWITCH__` is set
by `target_compile_definitions(pipensx PRIVATE __SWITCH__)` and is absent for
`golden_runner`.

**b. Unconditional `#include <switch.h>`, resolved by the PC shim.** Only
`src/app/installed_title_service.cpp` and `src/ui/common/ui_helpers.hpp` do
this. `src/platform/pc/switch.h` (a hand-written 182-line stand-in) is first on
`golden_runner`'s include path and supplies stubs. **Adding a libnx symbol to
one of these two files without adding a stub there leaves the Switch build
green and breaks `make golden`.** Stub behaviour is chosen for deterministic
rendering, not realism — read the header comment before adding to it.

**c. Behind `src/install/install_backend.hpp`.** The heavy content-install
surface (ncm, es, fs placeholders, CNMT) lives here, with
`install_backend_switch.cpp` and `install_backend_pc.cpp` implementing both
sides. New install-path work goes through this seam; both implementations get
written, not just the console one.

`Makefile.pc` builds core/app C only and never sees the shim — it relies purely
on pattern (a).

## 3. House idioms

- `Result rc = ...; if (R_FAILED(rc)) { ...close what you opened...; return; }`
  Every early return closes its own handles by hand — there is no RAII wrapper
  in this codebase, and adding one is a separate decision, not a drive-by.
- Surface a failure as `0x%08x`, never as a bare decimal:
  `diagnostic_error("installed", "list", "result=0x%08x", rc)`
  (`src/core/util.h:41`), or the user-facing
  `"Unable to query SD storage (0x%08x)."` form.
- Never `assert`/abort on a bad `Result`. This is homebrew running on someone's
  console — degrade and report. (Same reason `pipensx_utp` is built `NDEBUG`.)

## 4. Verify with both builds

```
make            # Switch: build-switch/pipensx.nro
make golden     # PC: compiles the shared sources against the shim, then renders
```

A missing shim stub only shows up in the second. Run both before calling a
libnx change done.

## 5. Constraints that live in prose, not headers

Applet RAM budget, tile override, sysmodule tradeoffs — already written up.
Read, do not re-derive or restate:
`docs/plans/ESHOP_APPLET_PLAN.md`, `docs/plans/SYSMODULE_PLAN.md`,
`docs/plans/PERF_PLAN.md`.
