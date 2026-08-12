#pragma once
/*
 * Lightweight read-only bencode parser.
 * Works on a flat buffer; no allocations except for list/dict traversal
 * which uses the caller's stack via recursive descent.
 */
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BE_INT  = 'i',
    BE_STR  = 's',
    BE_LIST = 'l',
    BE_DICT = 'd',
    BE_ERR  = 0
} be_type_t;

typedef struct {
    be_type_t   type;
    const char *buf;   /* start of this value in the source buffer */
    size_t      raw_len; /* byte length of the bencoded value (for SHA-1 of info dict) */

    /* For BE_INT */
    int64_t     ival;

    /* For BE_STR */
    const char *sval;
    size_t      slen;
} be_node_t;

/*
 * Parse one bencode value starting at *p.
 * On success returns 1 and advances *p past the value.
 * On error returns 0.
 */
int be_decode(const char **p, const char *end, be_node_t *out);

/*
 * Lookup a key in a bencoded dictionary.
 * dict_start / dict_end delimit the raw bencoded dict (including 'd' and 'e').
 * Returns 1 and fills *val if found, 0 otherwise.
 */
int be_dict_get(const char *dict_start, const char *dict_end,
                const char *key, size_t klen, be_node_t *val);

/*
 * Iterator for dict: call with *p pointing just after 'd';
 * fills key_* and val on each call, advances *p.
 * Returns 1 while items remain, 0 at 'e' or on error.
 */
int be_dict_next(const char **p, const char *end,
                 const char **key, size_t *klen, be_node_t *val);

/*
 * Iterator for list: call with *p pointing just after 'l';
 * fills item, advances *p.
 * Returns 1 while items remain, 0 at 'e' or on error.
 */
int be_list_next(const char **p, const char *end, be_node_t *item);
