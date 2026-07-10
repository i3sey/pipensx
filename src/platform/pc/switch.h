#pragma once

/* PC stand-in for libnx <switch.h> — desktop/golden builds only.
 *
 * This header is picked up instead of the real libnx header when the build
 * adds -Isrc/platform/pc (see PIPENSX_GOLDEN in CMakeLists.txt). It provides
 * the minimal surface consumed by src/ui and src/app so the UI compiles
 * unmodified off-console:
 *   - ui/common/ui_helpers.hpp: applet type/main loop, console, pad input
 *   - ui/settings/settings_view.hpp: hosversionGet + HOSVER_* macros,
 *     appletGetOperationMode
 *   - app/installed_title_service.cpp: ns application records + NACP
 *
 * Behaviour is chosen for deterministic golden rendering:
 *   - applet type reports Application (isApplicationMode() passes)
 *   - nsListApplicationRecord succeeds with zero records (empty library)
 *   - padGetButtonsDown reports Plus so any blocking console loop exits
 * The Switch build never sees this file.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef u32 Result;

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)

/* ---- applet ---- */

typedef enum {
    AppletType_None = -2,
    AppletType_Default = -1,
    AppletType_Application = 0,
    AppletType_SystemApplet = 1,
    AppletType_LibraryApplet = 2,
    AppletType_OverlayApplet = 3,
    AppletType_SystemApplication = 4,
} AppletType;

typedef enum {
    AppletOperationMode_Handheld = 0,
    AppletOperationMode_Console = 1,
} AppletOperationMode;

static inline AppletType appletGetAppletType(void) {
    return AppletType_Application;
}

static inline AppletOperationMode appletGetOperationMode(void) {
    return AppletOperationMode_Handheld;
}

static inline bool appletMainLoop(void) {
    return true;
}

static inline void svcSleepThread(int64_t nanoseconds) {
    (void)nanoseconds;
}

/* ---- hosversion ---- */

#define HOSVER_MAJOR(v) (((v) >> 16) & 0xFF)
#define HOSVER_MINOR(v) (((v) >> 8) & 0xFF)
#define HOSVER_MICRO(v) ((v) & 0xFF)

static inline u32 hosversionGet(void) {
    /* Fixed fake firmware version so diagnostics render deterministically. */
    return (20u << 16) | (1u << 8) | 0u;
}

/* ---- console ---- */

typedef struct PrintConsole PrintConsole;

static inline PrintConsole* consoleInit(PrintConsole* console) {
    return console;
}

static inline void consoleUpdate(PrintConsole* console) {
    (void)console;
}

static inline void consoleExit(PrintConsole* console) {
    (void)console;
}

/* ---- hid / pad ---- */

#define HidNpadStyleSet_NpadStandard 0x7u
#define HidNpadButton_Plus (1ULL << 10)

typedef struct {
    u64 buttons_cur;
    u64 buttons_old;
} PadState;

static inline void padConfigureInput(u32 max_players, u32 style_set) {
    (void)max_players;
    (void)style_set;
}

static inline void padInitializeDefault(PadState* pad) {
    memset(pad, 0, sizeof(*pad));
}

static inline void padUpdate(PadState* pad) {
    (void)pad;
}

static inline u64 padGetButtonsDown(const PadState* pad) {
    (void)pad;
    /* Report Plus so any "press + to exit" console loop terminates. */
    return HidNpadButton_Plus;
}

/* ---- ns / nacp ---- */

typedef struct {
    char name[0x200];
    char author[0x100];
} NacpLanguageEntry;

typedef struct {
    NacpLanguageEntry lang[16];
    u8 reserved[0x4000 - 16 * sizeof(NacpLanguageEntry)];
} NacpStruct; /* sizeof == 0x4000, same as libnx */

typedef struct {
    u64 application_id;
    u8 type;
    u8 reserved[0x0f];
} NsApplicationRecord;

typedef struct {
    NacpStruct nacp;
    u8 icon[0x20000];
} NsApplicationControlData;

typedef enum {
    NsApplicationControlSource_CacheOnly = 0,
    NsApplicationControlSource_Storage = 1,
    NsApplicationControlSource_StorageOnly = 2,
} NsApplicationControlSource;

static inline Result nsListApplicationRecord(NsApplicationRecord* records,
                                             s32 count, s32 offset,
                                             s32* out_count) {
    (void)records;
    (void)count;
    (void)offset;
    if (out_count)
        *out_count = 0;
    return 0; /* success, empty library */
}

static inline Result nsGetApplicationControlData(
    NsApplicationControlSource source, u64 application_id,
    NsApplicationControlData* data, size_t size, u64* actual_size) {
    (void)source;
    (void)application_id;
    (void)data;
    (void)size;
    if (actual_size)
        *actual_size = 0;
    return 0x236; /* generic libnx-style failure */
}

static inline Result nacpGetLanguageEntry(NacpStruct* nacp,
                                          NacpLanguageEntry** out) {
    (void)nacp;
    if (out)
        *out = NULL;
    return 0x236;
}
