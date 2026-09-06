cmake_minimum_required(VERSION 3.13)

# PIPENSX_GOLDEN=ON builds the PC golden-screenshot runner (SDL2 desktop,
# docs/plans/UI_PLAN.md F4) instead of the Switch homebrew. Use a separate
# build directory.
# SDL2 (not GLFW): the vendored borealis GLFWInputManager misses the 5-arg
# sendRumble override and does not compile; the SDL backend is complete.
option(PIPENSX_GOLDEN
    "Build the PC golden-screenshot runner instead of the Switch app" OFF)
set(PIPENSX_CATALOG_SNAPSHOT
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/catalog/switch_games.json"
    CACHE FILEPATH "Bundled Langegen catalog snapshot for offline first launch")
set(PIPENSX_METADATA_INDEX
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/catalog/game_metadata_index.json"
    CACHE FILEPATH "Bundled game metadata index snapshot for offline first launch")

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/library)
if (PIPENSX_GOLDEN)
    set(PLATFORM_DESKTOP ON CACHE BOOL "" FORCE)
    set(PLATFORM_SWITCH OFF CACHE BOOL "" FORCE)
    set(USE_DEKO3D OFF CACHE BOOL "" FORCE)
    set(USE_GLFW OFF CACHE BOOL "" FORCE)
    set(USE_SDL2 ON CACHE BOOL "" FORCE)
    # System SDL2: avoids initializing the bundled SDL submodule.
    set(USE_SYSTEM_SDL2 ON CACHE BOOL "" FORCE)
    # toolchain.cmake forces USE_GLFW=ON when "NOT USE_GLFW AND NOT SDL2";
    # define SDL2 so the desktop branch keeps the SDL backend.
    set(SDL2 ON)
    # Absolute resources path: golden_runner chdirs into a scratch sandbox.
    set(BRLS_RESOURCES_DIR ${CMAKE_BINARY_DIR})
else ()
    set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
    set(USE_DEKO3D ON CACHE BOOL "" FORCE)
    set(USE_GLFW OFF CACHE BOOL "" FORCE)
    set(USE_SDL2 OFF CACHE BOOL "" FORCE)
endif ()
set(USE_SYSTEM_FMT OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TWEENY OFF CACHE BOOL "" FORCE)
set(BRLS_UNITY_BUILD OFF CACHE BOOL "" FORCE)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

set(PIPENSX_VERSION "1.0.0" CACHE STRING "pipensx release version")
project(pipensx VERSION ${PIPENSX_VERSION} LANGUAGES C CXX)

# devkitA64 defaults Release to "-g -O2": the debug info balloons the ELF to
# ~90 MB and slows every compile and link, while elf2nro strips it from the
# NRO anyway. Keep Release lean.
if (PLATFORM_SWITCH)
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif ()

set(APP_TITLE "pipensx")
set(APP_AUTHOR "pipensx")
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg)
set(ROMFS_DIR ${CMAKE_BINARY_DIR}/romfs)

add_subdirectory(vendor/borealis/library)
target_include_directories(borealis PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/glm
)

file(GLOB ZSTD_COMMON_SOURCES CONFIGURE_DEPENDS
    vendor/zstd/lib/common/*.c
)
file(GLOB ZSTD_DECOMPRESS_SOURCES CONFIGURE_DEPENDS
    vendor/zstd/lib/decompress/*.c
)
add_library(pipensx_zstd STATIC
    ${ZSTD_COMMON_SOURCES}
    ${ZSTD_DECOMPRESS_SOURCES}
)
target_include_directories(pipensx_zstd PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zstd/lib
)
# ZSTD_DISABLE_ASM: the source glob only picks up *.c, so the x86-64 BMI2
# asm loops (huf_decompress_amd64.S) are never assembled; without this the
# PC golden build fails to link. No effect on the aarch64 Switch build.
target_compile_definitions(pipensx_zstd PRIVATE ZSTD_MULTITHREAD=0 ZSTD_DISABLE_ASM)

# Public-domain 7z ANSI-C decoder (LZMA SDK) for port archive extraction.
file(GLOB LZMA_7Z_SOURCES CONFIGURE_DEPENDS
    vendor/lzma/7zAlloc.c
    vendor/lzma/7zArcIn.c
    vendor/lzma/7zBuf.c
    vendor/lzma/7zBuf2.c
    vendor/lzma/7zCrc.c
    vendor/lzma/7zCrcOpt.c
    vendor/lzma/7zDec.c
    vendor/lzma/7zFile.c
    vendor/lzma/7zStream.c
    vendor/lzma/Bcj2.c
    vendor/lzma/Bra.c
    vendor/lzma/Bra86.c
    vendor/lzma/BraIA64.c
    vendor/lzma/CpuArch.c
    vendor/lzma/Delta.c
    vendor/lzma/LzmaDec.c
    vendor/lzma/Lzma2Dec.c
    vendor/lzma/Ppmd7.c
    vendor/lzma/Ppmd7Dec.c
)
add_library(pipensx_lzma STATIC ${LZMA_7Z_SOURCES})
target_include_directories(pipensx_lzma PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lzma
)
target_compile_definitions(pipensx_lzma PRIVATE Z7_EXTRACT_ONLY Z7_PPMD_SUPPORT)

# Vendored libutp (BitTorrent reference μTP/BEP-29). C++ with an extern "C"
# API (utp.h), linked into the C core for the TCP→μTP dial fallback. Same
# vendoring posture as pipensx_zstd and vendor/dht. POSIX selects the BSD
# sockets + inet_ntop paths (correct for both Linux and libnx/aarch64);
# -fno-exceptions/-fno-rtti/-fpermissive/-Wno-sign-compare mirror upstream.
add_library(pipensx_utp STATIC
    vendor/libutp/utp_internal.cpp
    vendor/libutp/utp_utils.cpp
    vendor/libutp/utp_hash.cpp
    vendor/libutp/utp_callbacks.cpp
    vendor/libutp/utp_api.cpp
    vendor/libutp/utp_packedsockaddr.cpp
)
target_include_directories(pipensx_utp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libutp
)
# NDEBUG: libutp guards internal invariants with assert(); a homebrew torrent
# client must never abort on one. Combined with the registered UTP_GET_UDP_MTU
# callback (torrent.c) this also sidesteps the mtu_reset ceiling assert.
target_compile_definitions(pipensx_utp PRIVATE POSIX NDEBUG)
target_compile_options(pipensx_utp PRIVATE
    -fpermissive -fno-exceptions -fno-rtti -Wno-sign-compare
)

# Platform-neutral sources (also compiled by Makefile.pc / golden runner).
set(CORE_SOURCES
    src/core/sha1.c
    src/core/sha256.c
    src/core/mse.c
    src/core/bencode.c
    src/core/metainfo.c
    src/core/util.c
    src/core/net.c
    src/core/peer.c
    src/core/piece.c
    src/core/tracker.c
    src/core/dht.c
    src/core/torrent.c
    src/platform/storage.c
    vendor/dht/dht.c
)

set(APP_SERVICE_SOURCES
    src/app/update_transaction.c
    src/app/app_settings.cpp
    src/app/favorites_service.cpp
    src/app/update_service.cpp
    src/app/catalog_batch_installer.cpp
    src/app/catalog_refresh.cpp
    src/app/catalog_service.cpp
    src/app/catalog_presentation.cpp
    src/app/snapshot_zstd.cpp
    src/app/download_manager.cpp
    src/app/task_files.cpp
    src/app/switch_deploy.cpp
    src/app/port_archive.cpp
    src/app/install_pacer.cpp
    src/app/web_seed_source.cpp
    src/app/request_gate.cpp
    src/app/stream_budget_arbiter.cpp
    src/app/stream_ram_budget.cpp
    src/app/game_metadata_service.cpp
    src/app/game_update_install.cpp
    src/app/game_update_service.cpp
    src/app/installed_title_service.cpp
    src/app/nacp_language.cpp
    src/app/storage_manager.cpp
    src/app/install_space.cpp
    src/app/magnet_resolver.cpp
    src/app/torrent_metainfo_fetch.cpp
    src/app/http_server.cpp
    src/app/web_add_queue.cpp
    src/app/web_server.cpp
    src/app/companion_settings.cpp
    src/app/bug_report.cpp
    src/app/torbox_client.cpp
    src/app/torbox_pairing_server.cpp
    src/app/torbox_provider.cpp
    src/app/torrserver_provider.cpp
    src/app/realdebrid_client.cpp
    src/app/realdebrid_provider.cpp
    src/app/debrid_transfer.cpp
    src/install/install_journal.cpp
    src/install/package_stream.cpp
)

set(UI_SOURCES
    src/ui/catalog/batch_install.cpp
    src/ui/catalog/catalog_view.cpp
    src/ui/detail/torrent_selection.cpp
    src/ui/downloads/downloads_view.cpp
    src/ui/downloads/file_picker.cpp
    src/ui/settings/about_view.cpp
    src/ui/settings/bug_report_view.cpp
)

if (NOT PIPENSX_GOLDEN)

program_target(pipensx_updater
    "src/update_helper.c;src/app/update_transaction.c;src/core/sha256.c")
set_target_properties(pipensx_updater PROPERTIES
    C_STANDARD 11
)
target_include_directories(pipensx_updater PRIVATE src)
target_compile_definitions(pipensx_updater PRIVATE __SWITCH__)
target_compile_options(pipensx_updater PRIVATE
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)

# Sysmodule probe (docs/plans/SYSMODULE_PLAN.md, ESHOP_APPLET_PLAN.md §5.4):
# a daemon that answers the background-download and half-awake hypotheses on
# hardware. It links neither borealis nor the torrent engine, so it needs its
# own libnx init - see src/probe/probe_main.c __appInit.
program_target(pipensx_probe
    "src/probe/probe_main.c;src/probe/probe_report.c")
set_target_properties(pipensx_probe PROPERTIES
    C_STANDARD 11
)
target_include_directories(pipensx_probe PRIVATE src)
target_compile_definitions(pipensx_probe PRIVATE __SWITCH__)
target_compile_options(pipensx_probe PRIVATE
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)

# The daemon (docs/plans/SYSMODULE_PHASES.md phase 2): the engine's future
# home. Skeleton for now - libnx init lifted from the probe plus the app's own
# HttpServer as the control plane, no engine and no borealis. Kept at C++17
# like Makefile.pc so the shared sources stay in the stricter dialect.
set(PIPENSX_DAEMON_HEAP_MB 32 CACHE STRING
    "Heap ceiling the daemon sysmodule asks the kernel for, in MiB")
program_target(pipensx_daemon
    "src/daemon/daemon_main.cpp;src/app/http_server.cpp;src/core/net.c;src/core/util.c")
set_target_properties(pipensx_daemon PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(pipensx_daemon PRIVATE src)
target_compile_definitions(pipensx_daemon PRIVATE
    __SWITCH__
    PIPENSX_VERSION="${PROJECT_VERSION}"
    PIPENSX_DAEMON_HEAP_MB=${PIPENSX_DAEMON_HEAP_MB}
)
target_compile_options(pipensx_daemon PRIVATE
    -Wall
    -Wextra
    -ffunction-sections
    -fdata-sections
)

# H8 client: a throwaway NRO launched from hbmenu while the probe runs.
program_target(pipensx_probe_client "tools/probe_client.c")
set_target_properties(pipensx_probe_client PROPERTIES
    C_STANDARD 11
)
target_compile_definitions(pipensx_probe_client PRIVATE __SWITCH__)
target_compile_options(pipensx_probe_client PRIVATE -Wall -Wextra)

set(APP_SOURCES
    ${CORE_SOURCES}
    src/platform/switch_crashlog.c
    src/platform/switch_backlight.cpp
    src/platform/switch_performance.cpp
    ${APP_SERVICE_SOURCES}
    src/install/install_backend_switch.cpp
    src/main_switch.cpp
    ${UI_SOURCES}
    vendor/qrcodegen/qrcodegen.cpp
    vendor/libnx-ext/libnx-ipcext/source/es.c
    vendor/libnx-ext/libnx-ipcext/source/ns-ext.c
    ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c
)

program_target(pipensx "${APP_SOURCES}")
set_target_properties(pipensx PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(pipensx PRIVATE
    src
    src/core
    src/platform
    vendor
    vendor/borealis/library/include
    vendor/zstd/lib
    vendor/libutp
    vendor/libnx-ext/libnx-ext/include
    vendor/libnx-ext/libnx-ipcext/include
)
target_compile_definitions(pipensx PRIVATE
    __SWITCH__
    USE_MBEDTLS
    PIPENSX_VERSION="${PROJECT_VERSION}"
)
target_compile_options(pipensx PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -ffunction-sections
    -fdata-sections
)
target_link_libraries(pipensx PRIVATE
    borealis
    pipensx_zstd
    pipensx_lzma
    pipensx_utp
    curl
    mbedtls
    mbedx509
    mbedcrypto
    z
    miniupnpc
)

# Probe packaging: ELF -> NSO -> exefs.nsp next to an NPDM, laid out the way
# Atmosphere expects under /atmosphere/contents/<title id>/.
#
# No flags/boot2.flag on purpose: the probe is launched at runtime (ovl-sysmodules
# or pm:shell), so a hung probe can never keep the console from booting. See
# src/probe/README.md.
find_program(NX_ELF2NSO_EXE NAMES elf2nso
    HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
find_program(NX_NPDMTOOL_EXE NAMES npdmtool
    HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
find_program(NX_BUILD_PFS0_EXE NAMES build_pfs0
    HINTS "$ENV{DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")

set(PROBE_TITLE_ID 00FF0000504E5358)
set(PROBE_EXEFS_DIR ${CMAKE_BINARY_DIR}/probe-exefs)
set(PROBE_OUT_DIR
    ${CMAKE_BINARY_DIR}/probe/atmosphere/contents/${PROBE_TITLE_ID})

add_custom_target(pipensx_probe_nsp ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROBE_EXEFS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROBE_OUT_DIR}"
    COMMAND ${NX_ELF2NSO_EXE} pipensx_probe.elf "${PROBE_EXEFS_DIR}/main"
    COMMAND ${NX_NPDMTOOL_EXE}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/probe.json"
        "${PROBE_EXEFS_DIR}/main.npdm"
    COMMAND ${NX_BUILD_PFS0_EXE} "${PROBE_EXEFS_DIR}" "${PROBE_OUT_DIR}/exefs.nsp"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/toolbox.json"
        "${PROBE_OUT_DIR}/toolbox.json"
    DEPENDS pipensx_probe "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/probe.json"
    BYPRODUCTS "${PROBE_OUT_DIR}/exefs.nsp" "${PROBE_EXEFS_DIR}/main"
        "${PROBE_EXEFS_DIR}/main.npdm"
    VERBATIM
)

add_custom_target(pipensx_probe_client_nro ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/probe/switch"
    COMMAND ${NX_NACPTOOL_EXE}
        --create "pipensx-probe-client" "${APP_AUTHOR}" "${PROJECT_VERSION}"
        pipensx_probe_client.nacp
    COMMAND ${NX_ELF2NRO_EXE}
        pipensx_probe_client.elf
        "${CMAKE_BINARY_DIR}/probe/switch/pipensx-probe-client.nro"
        --icon=${APP_ICON}
        --nacp=pipensx_probe_client.nacp
    DEPENDS pipensx_probe_client
    BYPRODUCTS "${CMAKE_BINARY_DIR}/probe/switch/pipensx-probe-client.nro"
        pipensx_probe_client.nacp
    VERBATIM
)

# Daemon packaging, same layout as the probe's. Deliberately not an ALL target:
# `make switch` builds the app, `make daemon` builds this. No flags/boot2.flag
# either - until the daemon has survived everything else, a hang has to be
# curable by switching it off in ovlSysmodules.
set(DAEMON_TITLE_ID 00FF0000504E5344)
set(DAEMON_EXEFS_DIR ${CMAKE_BINARY_DIR}/daemon-exefs)
set(DAEMON_OUT_DIR
    ${CMAKE_BINARY_DIR}/daemon/atmosphere/contents/${DAEMON_TITLE_ID})

add_custom_target(pipensx_daemon_nsp
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DAEMON_EXEFS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DAEMON_OUT_DIR}"
    COMMAND ${NX_ELF2NSO_EXE} pipensx_daemon.elf "${DAEMON_EXEFS_DIR}/main"
    COMMAND ${NX_NPDMTOOL_EXE}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/daemon.json"
        "${DAEMON_EXEFS_DIR}/main.npdm"
    COMMAND ${NX_BUILD_PFS0_EXE} "${DAEMON_EXEFS_DIR}" "${DAEMON_OUT_DIR}/exefs.nsp"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/toolbox.json"
        "${DAEMON_OUT_DIR}/toolbox.json"
    DEPENDS pipensx_daemon "${CMAKE_CURRENT_SOURCE_DIR}/src/daemon/daemon.json"
    BYPRODUCTS "${DAEMON_OUT_DIR}/exefs.nsp" "${DAEMON_EXEFS_DIR}/main"
        "${DAEMON_EXEFS_DIR}/main.npdm"
    VERBATIM
)

file(MAKE_DIRECTORY ${ROMFS_DIR})
# The helper is chain-loaded by hbloader via envSetNextLoad. hbloader (notably
# in application-override mode, which pipensx forces) can reject an NRO that
# lacks a NACP/asset section and abort with a User Break while loading it, so
# the helper is built as a complete homebrew NRO with a NACP and icon, exactly
# like the main app.
add_custom_target(sync_update_helper ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}"
    COMMAND ${NX_NACPTOOL_EXE}
        --create "pipensx-updater" "${APP_AUTHOR}" "${PROJECT_VERSION}"
        pipensx_updater.nacp
    COMMAND ${NX_ELF2NRO_EXE}
        pipensx_updater.elf "${ROMFS_DIR}/pipensx-updater.nro"
        --icon=${APP_ICON}
        --nacp=pipensx_updater.nacp
    DEPENDS pipensx_updater
    BYPRODUCTS "${ROMFS_DIR}/pipensx-updater.nro" pipensx_updater.nacp
    VERBATIM
)
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/resources/img/sys
    DESTINATION ${ROMFS_DIR}/img
)
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/resources/material
    DESTINATION ${ROMFS_DIR}
)
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/resources/i18n/en-US
    DESTINATION ${ROMFS_DIR}/i18n
)
# pipensx's own strings, plus a ru/hints.json overriding the borealis button
# hints (the vendored ru locale nests them under the wrong JSON pointer for
# this fork). A target rather than file(COPY) so editing a translation is
# picked up by a plain rebuild instead of needing a fresh configure.
add_custom_target(sync_i18n ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/i18n" "${ROMFS_DIR}/i18n"
    VERBATIM
)
# Web companion SPA, served from romfs:/web by src/app/web_server.cpp.
add_custom_target(sync_web ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/web" "${ROMFS_DIR}/web"
    VERBATIM
)
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
    DESTINATION ${ROMFS_DIR}
)
# TorBox (and other GTS/Cloudflare) HTTPS: switch-curl/libnx imports these
# roots via CURLOPT_CAINFO. See src/app/curl_https.hpp curlApplyTrustedSsl.
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/ssl
    DESTINATION ${ROMFS_DIR}
)
# Never reuse catalog data left in an existing build directory. Public builds
# ship both the Langegen torrent catalog and the metadata index snapshots for
# offline first launch.
file(REMOVE_RECURSE ${ROMFS_DIR}/catalog)
get_filename_component(PIPENSX_CATALOG_SNAPSHOT_ABS
    "${PIPENSX_CATALOG_SNAPSHOT}" ABSOLUTE)
if (NOT EXISTS "${PIPENSX_CATALOG_SNAPSHOT_ABS}")
    message(FATAL_ERROR
        "PIPENSX_CATALOG_SNAPSHOT does not exist: ${PIPENSX_CATALOG_SNAPSHOT_ABS}")
endif ()
set(PIPENSX_NRO_DEPENDS pipensx)
list(APPEND PIPENSX_NRO_DEPENDS sync_update_helper)
list(APPEND PIPENSX_NRO_DEPENDS sync_i18n)
list(APPEND PIPENSX_NRO_DEPENDS sync_web)
# The two JSON snapshots are ~19 MB — over 80% of the NRO uncompressed — and
# JSON compresses ~5x, so they ship as zstd frames that the load path
# decompresses (src/app/snapshot_zstd.cpp). Host zstd is required for the
# Switch build; scripts/build_switch.sh checks for it too.
find_program(ZSTD_EXE zstd)
if (NOT ZSTD_EXE)
    message(FATAL_ERROR
        "zstd not found on the host — required to compress the bundled "
        "catalog snapshots into romfs")
endif ()
add_custom_command(
    OUTPUT "${ROMFS_DIR}/catalog/switch_games.json.zst"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}/catalog"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${ROMFS_DIR}/catalog/switch_games.json"
    COMMAND ${ZSTD_EXE} -15 -q -f
        -o "${ROMFS_DIR}/catalog/switch_games.json.zst"
        "${PIPENSX_CATALOG_SNAPSHOT_ABS}"
    DEPENDS "${PIPENSX_CATALOG_SNAPSHOT_ABS}"
    VERBATIM
)
add_custom_target(sync_catalog_snapshot ALL
    DEPENDS "${ROMFS_DIR}/catalog/switch_games.json.zst"
)
list(APPEND PIPENSX_NRO_DEPENDS sync_catalog_snapshot)
if (PIPENSX_METADATA_INDEX)
    get_filename_component(PIPENSX_METADATA_INDEX_ABS
        "${PIPENSX_METADATA_INDEX}" ABSOLUTE)
    if (NOT EXISTS "${PIPENSX_METADATA_INDEX_ABS}")
        message(FATAL_ERROR
            "PIPENSX_METADATA_INDEX does not exist: ${PIPENSX_METADATA_INDEX_ABS}")
    endif ()
    add_custom_command(
        OUTPUT "${ROMFS_DIR}/catalog/game_metadata_index.json.zst"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}/catalog"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "${ROMFS_DIR}/catalog/game_metadata_index.json"
        COMMAND ${ZSTD_EXE} -15 -q -f
            -o "${ROMFS_DIR}/catalog/game_metadata_index.json.zst"
            "${PIPENSX_METADATA_INDEX_ABS}"
        DEPENDS "${PIPENSX_METADATA_INDEX_ABS}"
        VERBATIM
    )
    add_custom_target(sync_metadata_index ALL
        DEPENDS "${ROMFS_DIR}/catalog/game_metadata_index.json.zst"
    )
    list(APPEND PIPENSX_NRO_DEPENDS sync_metadata_index)
endif ()
gen_dksh(${ROMFS_DIR}/shaders)

add_custom_target(pipensx.nro ALL
    DEPENDS ${PIPENSX_NRO_DEPENDS}
    COMMAND ${NX_NACPTOOL_EXE}
        --create "${APP_TITLE}" "${APP_AUTHOR}" "${PROJECT_VERSION}"
        pipensx.nacp
    COMMAND ${NX_ELF2NRO_EXE}
        pipensx.elf pipensx.nro
        --icon=${APP_ICON}
        --nacp=pipensx.nacp
        --romfsdir=${ROMFS_DIR}
    BYPRODUCTS pipensx.nacp pipensx.nro
    VERBATIM
)

else () # PIPENSX_GOLDEN

find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_executable(golden_runner
    ${CORE_SOURCES}
    ${APP_SERVICE_SOURCES}
    ${UI_SOURCES}
    src/platform/pc/switch_shim.cpp
    src/install/install_backend_pc.cpp
    vendor/qrcodegen/qrcodegen.cpp
    tools/golden_runner.cpp
)
set_target_properties(golden_runner PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(golden_runner PRIVATE
    src/platform/pc # PC stand-in for <switch.h>, must come first
    src
    src/core
    src/platform
    vendor
    vendor/borealis/library/include
    vendor/zstd/lib
    vendor/libutp
)
target_compile_definitions(golden_runner PRIVATE
    _GNU_SOURCE
    PIPENSX_VERSION="${PROJECT_VERSION}"
)
target_compile_options(golden_runner PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
target_link_libraries(golden_runner PRIVATE
    borealis
    pipensx_zstd
    pipensx_lzma
    pipensx_utp
    CURL::libcurl
    ZLIB::ZLIB
    crypto
    Threads::Threads
    m
)

# Borealis loads fonts/i18n/material from ${BRLS_RESOURCES_DIR}/resources/.
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/resources
    DESTINATION ${CMAKE_BINARY_DIR}
)
# Layered on top: without en-US/pipensx.json the runner renders raw key names
# and every golden baseline diffs.
add_custom_target(sync_i18n ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/i18n"
        "${CMAKE_BINARY_DIR}/resources/i18n"
    VERBATIM
)
add_dependencies(golden_runner sync_i18n)

endif ()
