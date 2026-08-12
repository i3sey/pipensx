/* PC stubs for Switch-only platform hooks (desktop/golden builds only).
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

} // extern "C"
