#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void switch_crashlog_install(void);
void switch_crashlog_stage(const char *stage);
/* Last stage observed on the UI thread. Safe to read from any thread
 * (watchdog ANR dumps); may lag by one stage under a torn-schedule race,
 * which is acceptable for triage output. */
const char *switch_crashlog_last_stage(void);

#ifdef __cplusplus
}
#endif
