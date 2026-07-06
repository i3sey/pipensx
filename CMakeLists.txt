cmake_minimum_required(VERSION 3.13)

# PIPENSX_GOLDEN=ON builds the PC golden-screenshot runner (SDL2 desktop,
# UI_PLAN.md F4) instead of the Switch homebrew. Use a separate build dir.
# SDL2 (not GLFW): the vendored borealis GLFWInputManager misses the 5-arg
# sendRumble override and does not compile; the SDL backend is complete.
option(PIPENSX_GOLDEN
    "Build the PC golden-screenshot runner instead of the Switch app" OFF)

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

project(pipensx VERSION 0.4.0 LANGUAGES C CXX)

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

# Platform-neutral sources (also compiled by Makefile.pc / golden runner).
set(CORE_SOURCES
    src/core/sha1.c
    src/core/bencode.c
    src/core/metainfo.c
    src/core/util.c
    src/core/antizapret.c
    src/core/catalog_sig.c
    vendor/tweetnacl/tweetnacl.c
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
    src/app/app_settings.cpp
    src/app/catalog_batch_installer.cpp
    src/app/catalog_service.cpp
    src/app/catalog_presentation.cpp
    src/app/download_manager.cpp
    src/app/request_gate.cpp
    src/app/stream_ram_budget.cpp
    src/app/game_metadata_service.cpp
    src/app/installed_title_service.cpp
    src/app/install_space.cpp
    src/app/magnet_resolver.cpp
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
)

if (NOT PIPENSX_GOLDEN)

set(APP_SOURCES
    ${CORE_SOURCES}
    src/platform/switch_crashlog.c
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
    curl
    mbedtls
    mbedx509
    mbedcrypto
    z
    miniupnpc
)

file(MAKE_DIRECTORY ${ROMFS_DIR})
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
file(COPY
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
    DESTINATION ${ROMFS_DIR}
)
file(GLOB_RECURSE CATALOG_ROMFS_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/catalog/*
)
add_custom_target(sync_catalog_romfs ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/catalog
        ${ROMFS_DIR}/catalog
    DEPENDS ${CATALOG_ROMFS_FILES}
    VERBATIM
)
gen_dksh(${ROMFS_DIR}/shaders)

add_custom_target(pipensx.nro ALL
    DEPENDS pipensx sync_catalog_romfs
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

endif ()
