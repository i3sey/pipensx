#ifndef PIPENSX_UPDATE_TRANSACTION_H
#define PIPENSX_UPDATE_TRANSACTION_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *target;
    const char *staged;
    const char *marker;
    const char *backup;
} update_paths_t;

bool update_transaction_ready(const update_paths_t *paths,
                              char *error, size_t error_size);
bool update_transaction_apply(const update_paths_t *paths,
                              char *error, size_t error_size);
bool update_transaction_rollback(const update_paths_t *paths,
                                 char *error, size_t error_size);
bool update_transaction_confirm(const update_paths_t *paths,
                                char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
