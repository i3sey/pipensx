#include "metainfo.h"
#include "bencode.h"
#include "sha1.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void strncpy_safe(char *dst, size_t dsz, const char *src, size_t slen) {
    size_t n = (slen < dsz - 1) ? slen : dsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int metainfo_path_is_safe(const char *path) {
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\')
        return 0;
    if (strlen(path) >= MAX_NAME_LEN)
        return 0;

    const char *component = path;
    for (const char *p = path;; p++) {
        if (*p == '\\')
            return 0;
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - component);
            if (len == 0 ||
                (len == 1 && component[0] == '.') ||
                (len == 2 && component[0] == '.' && component[1] == '.'))
                return 0;
            if (*p == '\0')
                break;
            component = p + 1;
        }
    }
    return 1;
}

int metainfo_parse(const uint8_t *data, size_t len, metainfo_t *mi) {
    memset(mi, 0, sizeof(*mi));
    const char *buf = (const char*)data;
    const char *end = buf + len;
    const char *p   = buf;

    be_node_t root;
    if (!be_decode(&p, end, &root) || root.type != BE_DICT) {
        log_msg("[meta] not a bencode dict\n");
        return 0;
    }

    /* ---- info dict ---- */
    be_node_t info_node;
    if (!be_dict_get(root.buf, root.buf + root.raw_len, "info", 4, &info_node)
        || info_node.type != BE_DICT) {
        log_msg("[meta] no info dict\n");
        return 0;
    }

    /* info_hash = SHA-1 of raw bencoded info dict */
    sha1((const uint8_t*)info_node.buf, info_node.raw_len, mi->info_hash);

    be_node_t v;
    /* name */
    if (be_dict_get(info_node.buf, info_node.buf + info_node.raw_len, "name", 4, &v) && v.type == BE_STR)
        strncpy_safe(mi->name, MAX_NAME_LEN, v.sval, v.slen);
    else
        strncpy_safe(mi->name, MAX_NAME_LEN, "unknown", 7);
    if (!metainfo_path_is_safe(mi->name)) {
        log_msg("[meta] unsafe torrent name\n");
        return 0;
    }

    /* piece length */
    if (!be_dict_get(info_node.buf, info_node.buf + info_node.raw_len, "piece length", 12, &v) || v.type != BE_INT) {
        log_msg("[meta] no piece length\n");
        return 0;
    }
    mi->piece_length = v.ival;

    /* pieces */
    if (!be_dict_get(info_node.buf, info_node.buf + info_node.raw_len, "pieces", 6, &v) || v.type != BE_STR
        || v.slen % 20 != 0) {
        log_msg("[meta] invalid pieces\n");
        return 0;
    }
    mi->num_pieces = (uint32_t)(v.slen / 20);
    mi->piece_hashes = (uint8_t*)malloc(v.slen);
    if (!mi->piece_hashes) return 0;
    memcpy(mi->piece_hashes, v.sval, v.slen);

    /* files */
    be_node_t files_node;
    if (be_dict_get(info_node.buf, info_node.buf + info_node.raw_len, "files", 5, &files_node)
        && files_node.type == BE_LIST) {
        /* Multi-file torrent */
        mi->is_multi = 1;

        /* Count files first */
        uint32_t fc = 0;
        {
            const char *fp = files_node.buf + 1;
            const char *fe = files_node.buf + files_node.raw_len - 1;
            be_node_t item;
            while (be_list_next(&fp, fe, &item)) fc++;
        }
        mi->num_files = fc;
        mi->files = (mi_file_t*)calloc(fc, sizeof(mi_file_t));
        if (!mi->files) { free(mi->piece_hashes); return 0; }

        int64_t offset = 0;
        uint32_t fi = 0;
        const char *fp = files_node.buf + 1;
        const char *fe = files_node.buf + files_node.raw_len - 1;
        be_node_t item;
        while (fi < fc && be_list_next(&fp, fe, &item)) {
            if (item.type != BE_DICT) { fi++; continue; }
            mi_file_t *f = &mi->files[fi];
            /* length */
            be_node_t fv;
            if (be_dict_get(item.buf, item.buf + item.raw_len, "length", 6, &fv) && fv.type == BE_INT)
                f->length = fv.ival;
            /* path list */
            be_node_t path_node;
            if (be_dict_get(item.buf, item.buf + item.raw_len, "path", 4, &path_node)
                && path_node.type == BE_LIST) {
                char tmp[MAX_NAME_LEN] = "";
                const char *pp = path_node.buf + 1;
                const char *pe = path_node.buf + path_node.raw_len - 1;
                be_node_t part;
                int first = 1;
                while (be_list_next(&pp, pe, &part)) {
                    if (part.type != BE_STR) continue;
                    if (!first) strncat(tmp, "/", sizeof(tmp)-1);
                    strncat(tmp, part.sval, part.slen < sizeof(tmp)-1 ? part.slen : sizeof(tmp)-1);
                    first = 0;
                }
                strncpy_safe(f->path, MAX_NAME_LEN, tmp, strlen(tmp));
            }
            if (!metainfo_path_is_safe(f->path)) {
                log_msg("[meta] unsafe file path '%s'\n", f->path);
                metainfo_free(mi);
                return 0;
            }
            f->offset = offset;
            offset += f->length;
            mi->total_length += f->length;
            fi++;
        }
    } else {
        /* Single-file torrent */
        mi->is_multi = 0;
        mi->num_files = 1;
        mi->files = (mi_file_t*)calloc(1, sizeof(mi_file_t));
        if (!mi->files) { free(mi->piece_hashes); return 0; }
        be_node_t lv;
        if (be_dict_get(info_node.buf, info_node.buf + info_node.raw_len, "length", 6, &lv) && lv.type == BE_INT)
            mi->files[0].length = lv.ival;
        strncpy_safe(mi->files[0].path, MAX_NAME_LEN, mi->name, strlen(mi->name));
        mi->files[0].offset = 0;
        mi->total_length = mi->files[0].length;
    }

    /* Warn about FAT32 limit */
    for (uint32_t i = 0; i < mi->num_files; i++) {
        if (mi->files[i].length > (int64_t)4*1024*1024*1024LL - 1)
            log_msg("[meta] WARNING: file '%s' exceeds 4 GB FAT32 limit\n", mi->files[i].path);
    }

    /* ---- trackers ---- */
    /* Try announce-list first */
    be_node_t al;
    if (be_dict_get(root.buf, root.buf + root.raw_len, "announce-list", 13, &al) && al.type == BE_LIST) {
        const char *lp = al.buf + 1;
        const char *le = al.buf + al.raw_len - 1;
        be_node_t tier;
        while (mi->num_trackers < MAX_TRACKERS && be_list_next(&lp, le, &tier)) {
            if (tier.type != BE_LIST) continue;
            const char *tp = tier.buf + 1;
            const char *te = tier.buf + tier.raw_len - 1;
            be_node_t url;
            while (mi->num_trackers < MAX_TRACKERS && be_list_next(&tp, te, &url)) {
                if (url.type == BE_STR && url.slen < 512) {
                    memcpy(mi->trackers[mi->num_trackers], url.sval, url.slen);
                    mi->trackers[mi->num_trackers][url.slen] = 0;
                    mi->num_trackers++;
                }
            }
        }
    }
    /* Fallback to announce */
    if (mi->num_trackers == 0) {
        be_node_t an;
        if (be_dict_get(root.buf, root.buf + root.raw_len, "announce", 8, &an) && an.type == BE_STR
            && an.slen < 512) {
            memcpy(mi->trackers[0], an.sval, an.slen);
            mi->trackers[0][an.slen] = 0;
            mi->num_trackers = 1;
        }
    }

    char ih_hex[41];
    hex20(ih_hex, mi->info_hash);
    log_msg("[meta] name='%s' hash=%s pieces=%u piece_len=%lld total=%lld files=%u trackers=%u\n",
            mi->name, ih_hex, mi->num_pieces, (long long)mi->piece_length,
            (long long)mi->total_length, mi->num_files, mi->num_trackers);
    return 1;
}

void metainfo_free(metainfo_t *mi) {
    free(mi->piece_hashes);
    free(mi->files);
    memset(mi, 0, sizeof(*mi));
}

int metainfo_load(const char *path, metainfo_t *mi) {
    FILE *f = fopen(path, "rb");
    if (!f) { log_msg("[meta] cannot open '%s'\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || sz > 10*1024*1024) { fclose(f); log_msg("[meta] file too large or empty\n"); return 0; }
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f);
    fclose(f);
    int r = metainfo_parse(buf, sz, mi);
    free(buf);
    return r;
}
