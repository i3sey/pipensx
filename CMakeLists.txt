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

project(pipensx VERSION 0.2.0 LANGUAGES C CXX)

set(APP_TITLE "pipensx")
set(APP_AUTHOR "pipensx")
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg)
set(ROMFS_DIR ${CMAKE_BINARY_DIR}/romfs)

add_subdirectory(vendor/borealis/library)
target_include_directories(borealis PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/glm
)

set(CORE_SOURCES
    src/core/sha1.c
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
    src/platform/switch_crashlog.c
    vendor/dht/dht.c
)

set(APP_SOURCES
    ${CORE_SOURCES}
    src/app/download_manager.cpp
    src/main_switch.cpp
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
)
target_compile_definitions(pipensx PRIVATE
    __SWITCH__
    USE_MBEDTLS
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
gen_dksh(${ROMFS_DIR}/shaders)

add_custom_target(pipensx.nro ALL
    DEPENDS pipensx
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
