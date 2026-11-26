# AutoRun forwarder components

Adapted from autorunhq/autorun commit
`1ac9b6cf37b90f2d5783dd7ef389bebbe1c68877`:
`horizon-wine/source/forwarder.c`, `low_window.h`, and `horizon-wine/hbl/`.

`builder.c` retains the NCA/PFS0/RomFS/NPDM/NACP construction code, replacing
the installer with an in-memory NSP export. Installation uses pipensx's existing
InstallBackend transaction. The hbloader reads nextNroPath and nextArgv from
RomFS and loads the runtime into an application process. No console keys are
stored on disk. The host format test also adapts upstream
`horizon-wine/tests/switch_shim.h` as `tests/autorun_switch/switch.h`. See LICENSE, COPYING.LIB, and hbl/LICENSE.md for licensing.

`pipensxBuildForwarderEx` also accepts an explicit publisher for the Retro
caller. The original entry point retains the AutoRun publisher. Both produce
the same NSP structure and use the same loader assets.
