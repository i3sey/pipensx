#ifndef PIPENSX_NRO_RELOCATION_H
#define PIPENSX_NRO_RELOCATION_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nro_relocation_paths {
    const char *source;
    const char *target;
    const char *backup;
} nro_relocation_paths_t;

/* Returns the sdmc: path when argv[0] names an NRO directly under /switch. */
bool nro_relocation_source(const char *launch_path, char *source,
                           size_t source_size);

bool nro_relocation_apply(const nro_relocation_paths_t *paths,
                          char *error, size_t error_size);
bool nro_relocation_rollback(const nro_relocation_paths_t *paths,
                             char *error, size_t error_size);
bool nro_relocation_confirm(const nro_relocation_paths_t *paths,
                            char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
