/* PC stubs for Switch-only platform hooks (PC test builds only).
 *
 * The Switch build compiles src/platform/switch_crashlog.c instead; this
 * translation unit satisfies the same extern "C" interface declared in
 * src/platform/switch_crashlog.h so UI code links unmodified on PC.
 */

#include "platform/switch_crashlog.h"

extern "C" {

void switch_crashlog_install(void) {}

void switch_crashlog_stage(const char* stage) {
    (void)stage; /* startupStage() already mirrors stages into the app log */
}

const char* switch_crashlog_last_stage(void) {
    /* Mirrors the Switch build's "before main" default; PC builds never hit
     * the watchdog ANR path (main_switch.cpp is Switch-only), so a constant
     * keeps the interface honest without tracking UI stages. */
    return "before main";
}

} // extern "C"
