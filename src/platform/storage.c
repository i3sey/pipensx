#include "storage.h"
#include "../core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef _WIN32
#  include <unistd.h>
#endif

struct file_handle {
    char  path[512];
    FILE *fp;
    int64_t offset; /* start in torrent flat space */
    int64_t length;
};

struct storage {
    struct file_handle *files;
    uint32_t num_files;
};

/* mkdir -p equivalent (portable) */
static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

storage_t *storage_open(const metainfo_t *mi, const char *outdir) {
    storage_t *s = (storage_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->num_files = mi->num_files;
    s->files = (struct file_handle*)calloc(mi->num_files, sizeof(struct file_handle));
    if (!s->files) { free(s); return NULL; }

    for (uint32_t i = 0; i < mi->num_files; i++) {
        const mi_file_t *mf = &mi->files[i];
        struct file_handle *fh = &s->files[i];
        fh->offset = mf->offset;
        fh->length = mf->length;

        /* Build full path — use snprintf with guaranteed NUL */
        char fullpath[512];
        int path_len = mi->is_multi
            ? snprintf(fullpath, sizeof(fullpath), "%s/%s/%s",
                       outdir, mi->name, mf->path)
            : snprintf(fullpath, sizeof(fullpath), "%s/%s",
                       outdir, mf->path);
        if (path_len < 0 || (size_t)path_len >= sizeof(fullpath)) {
            log_msg("[storage] output path is too long\n");
            storage_close(s);
            return NULL;
        }
        memcpy(fh->path, fullpath, sizeof(fh->path));
        fh->path[sizeof(fh->path)-1] = '\0';

        /* Create parent directories */
        char *slash = strrchr(fullpath, '/');
        if (slash) { *slash = 0; mkdirs(fullpath); *slash = '/'; }

        /* Open for r+w; create if absent */
        fh->fp = fopen(fh->path, "r+b");
        if (!fh->fp) {
            fh->fp = fopen(fh->path, "w+b");
            if (!fh->fp) {
                log_msg("[storage] cannot open '%s': %s\n", fh->path, strerror(errno));
                storage_close(s);
                return NULL;
            } else {
                /* Pre-allocate / seek to size */
                if (fh->length > 0) {
                    fseek(fh->fp, (long)(fh->length - 1), SEEK_SET);
                    fputc(0, fh->fp);
                    fseek(fh->fp, 0, SEEK_SET);
                }
            }
        }
    }
    return s;
}

static int find_file(storage_t *s, int64_t off,
                     int64_t len __attribute__((unused)),
                     struct file_handle **fh_out, int64_t *local_off) {
    for (uint32_t i = 0; i < s->num_files; i++) {
        struct file_handle *fh = &s->files[i];
        if (off >= fh->offset && off < fh->offset + fh->length) {
            *fh_out = fh;
            *local_off = off - fh->offset;
            return 1;
        }
    }
    return 0;
}

int storage_write(storage_t *s, int64_t offset, const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)written, (int64_t)(len - written), &fh, &local_off))
            return 0;
        if (!fh->fp) return 0;
        size_t can_write = (size_t)(fh->length - local_off);
        if (can_write > len - written) can_write = len - written;
        if (fseek(fh->fp, (long)local_off, SEEK_SET) != 0) return 0;
        size_t w = fwrite(data + written, 1, can_write, fh->fp);
        if (w != can_write) return 0;
        written += w;
    }
    return 1;
}

int storage_flush(storage_t *s) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->num_files; i++) {
        if (!s->files[i].fp || fflush(s->files[i].fp) != 0)
            return 0;
    }
    return 1;
}

int storage_read(storage_t *s, int64_t offset, uint8_t *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)done, (int64_t)(len - done), &fh, &local_off))
            return -1;
        if (!fh->fp) return -1;
        size_t can_read = (size_t)(fh->length - local_off);
        if (can_read > len - done) can_read = len - done;
        clearerr(fh->fp);
        if (fseek(fh->fp, (long)local_off, SEEK_SET) != 0) return -1;
        size_t r = fread(data + done, 1, can_read, fh->fp);
        done += r;
        if (r != can_read) return -1;
    }
    return (int)done;
}

void storage_close(storage_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->num_files; i++)
        if (s->files[i].fp) fclose(s->files[i].fp);
    free(s->files);
    free(s);
}
