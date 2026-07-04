cmake_minimum_required(VERSION 3.13)

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/vendor/borealis/library)
set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(USE_DEKO3D ON CACHE BOOL "" FORCE)
set(USE_GLFW OFF CACHE BOOL "" FORCE)
set(USE_SDL2 OFF CACHE BOOL "" FORCE)
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
target_compile_definitions(pipensx_zstd PRIVATE ZSTD_MULTITHREAD=0)

set(CORE_SOURCES
    src/core/sha1.c
    src/core/bencode.c
    src/core/metainfo.c
    src/core/util.c
    src/core/antizapret.c
    src/core/net.c
    src/core/peer.c
    src/core/piece.c
    src/core/tracker.c
    src/core/dht.c
    src/core/torrent.c
    src/platform/storage.c
    src/platform/switch_crashlog.c
    src/platform/switch_performance.cpp
    vendor/dht/dht.c
)

set(APP_SOURCES
    ${CORE_SOURCES}
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
    src/install/package_stream.cpp
    src/install/install_backend_switch.cpp
    src/main_switch.cpp
    src/ui/catalog/batch_install.cpp
    src/ui/catalog/catalog_view.cpp
    src/ui/detail/torrent_selection.cpp
    src/ui/downloads/downloads_view.cpp
    src/ui/downloads/file_picker.cpp
    src/ui/settings/about_view.cpp
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
