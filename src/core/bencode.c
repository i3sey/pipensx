#include "bencode.h"
#include <string.h>

/* Parse integer: iNNNe */
static int parse_int(const char **p, const char *end, int64_t *out) {
    const char *s = *p;
    if (s >= end || *s != 'i') return 0;
    s++;
    int neg = 0;
    if (s < end && *s == '-') { neg = 1; s++; }
    int64_t v = 0;
    int digits = 0;
    while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; digits++; }
    if (!digits || s >= end || *s != 'e') return 0;
    s++;
    *out = neg ? -v : v;
    *p = s;
    return 1;
}

/* Parse string: NNN:data */
static int parse_str(const char **p, const char *end,
                     const char **sval, size_t *slen) {
    const char *s = *p;
    if (s >= end || *s < '0' || *s > '9') return 0;
    size_t len = 0;
    while (s < end && *s >= '0' && *s <= '9') { len = len * 10 + (*s - '0'); s++; }
    if (s >= end || *s != ':') return 0;
    s++;
    if ((size_t)(end - s) < len) return 0;
    *sval = s;
    *slen = len;
    *p = s + len;
    return 1;
}

/* Skip one value without saving */
static int be_skip(const char **p, const char *end) {
    if (*p >= end) return 0;
    char c = **p;
    if (c == 'i') {
        int64_t dummy;
        return parse_int(p, end, &dummy);
    } else if (c >= '0' && c <= '9') {
        const char *sv; size_t sl;
        return parse_str(p, end, &sv, &sl);
    } else if (c == 'l') {
        (*p)++;
        while (*p < end && **p != 'e') {
            if (!be_skip(p, end)) return 0;
        }
        if (*p >= end) return 0;
        (*p)++;
        return 1;
    } else if (c == 'd') {
        (*p)++;
        while (*p < end && **p != 'e') {
            const char *kv; size_t kl;
            if (!parse_str(p, end, &kv, &kl)) return 0;
            if (!be_skip(p, end)) return 0;
        }
        if (*p >= end) return 0;
        (*p)++;
        return 1;
    }
    return 0;
}

int be_decode(const char **p, const char *end, be_node_t *out) {
    if (*p >= end) return 0;
    const char *start = *p;
    out->buf = start;
    char c = **p;
    if (c == 'i') {
        out->type = BE_INT;
        if (!parse_int(p, end, &out->ival)) return 0;
    } else if (c >= '0' && c <= '9') {
        out->type = BE_STR;
        if (!parse_str(p, end, &out->sval, &out->slen)) return 0;
    } else if (c == 'l') {
        out->type = BE_LIST;
        /* We don't recurse here; caller uses be_list_next */
        const char *tmp = *p;
        if (!be_skip(&tmp, end)) return 0;
        *p = tmp;
    } else if (c == 'd') {
        out->type = BE_DICT;
        const char *tmp = *p;
        if (!be_skip(&tmp, end)) return 0;
        *p = tmp;
    } else {
        return 0;
    }
    out->raw_len = (size_t)(*p - start);
    return 1;
}

int be_dict_next(const char **p, const char *end,
                 const char **key, size_t *klen, be_node_t *val) {
    if (*p >= end || **p == 'e') return 0;
    if (!parse_str(p, end, key, klen)) return 0;
    if (!be_decode(p, end, val)) return 0;
    return 1;
}

int be_list_next(const char **p, const char *end, be_node_t *item) {
    if (*p >= end || **p == 'e') return 0;
    return be_decode(p, end, item);
}

int be_dict_get(const char *dict_start, const char *dict_end,
                const char *key, size_t klen, be_node_t *val) {
    if (!dict_start || dict_start >= dict_end || *dict_start != 'd') return 0;
    const char *p = dict_start + 1;
    const char *k; size_t kl;
    be_node_t v;
    while (be_dict_next(&p, dict_end, &k, &kl, &v)) {
        if (kl == klen && memcmp(k, key, klen) == 0) {
            *val = v;
            return 1;
        }
    }
    return 0;
}
