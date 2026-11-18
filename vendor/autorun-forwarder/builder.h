#ifndef PIPENSX_FORWARDER_BUILDER_H
#define PIPENSX_FORWARDER_BUILDER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Switch-only: output is malloc-owned, caller frees it. Never writes NCM. */
uint32_t pipensxBuildForwarder(uint64_t tid, const char *name, const char *nro,
    const char *args, const void *main, size_t main_size,
    const void *npdm, size_t npdm_size, const void *icon, size_t icon_size,
    void **output, size_t *output_size);

uint32_t pipensxBuildForwarderEx(uint64_t tid, const char *name,
    const char *author, const char *display_version, const char *error_category,
    const char *nro, const char *args, const void *main, size_t main_size,
    const void *npdm, size_t npdm_size, const void *icon, size_t icon_size,
    void **output, size_t *output_size);

uint32_t pipensxBuildArchiveForwarder(uint64_t tid, const char *name,
    const char *author, const char *display_version,
    const char *nro, const char *args, const void *main, size_t main_size,
    const void *npdm, size_t npdm_size, const void *icon, size_t icon_size,
    void **output, size_t *output_size);
#ifdef __cplusplus
}
#endif
#endif
