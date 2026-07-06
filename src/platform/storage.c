#include "storage.h"
#include "../core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#ifndef _WIN32
#  include <unistd.h>
#endif

struct file_handle {
    char  path[512];
    FILE *fp;
    int64_t offset; /* start in torrent flat space */
    int64_t length;
    storage_file_config_t config;
};

struct storage {
    struct file_handle *files;
    uint32_t num_files;
    char error[256];
};

/* mkdir -p equivalent (portable) */
static void mkdirs(const char *path) {
    char tmp[512];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len < 0 || (size_t)len >= sizeof(tmp))
        return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *basename_component(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void sanitize_component(const char *input, char *out, size_t out_size) {
    size_t pos = 0;
    if (out_size == 0)
        return;

    for (const char *p = input; *p && pos + 1 < out_size; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '.' || ch == '-' || ch == '_') {
            out[pos++] = (char)ch;
        } else if ((ch == ' ' || ch == '[' || ch == ']' || ch == '(' || ch == ')') &&
                   pos > 0 && out[pos - 1] != '_') {
            out[pos++] = '_';
        }
    }

    while (pos > 0 && (out[pos - 1] == '_' || out[pos - 1] == '.'))
        pos--;
    if (pos == 0) {
        snprintf(out, out_size, "file");
    } else {
        out[pos] = '\0';
    }
}

static int build_original_path(char *fullpath, size_t size, const metainfo_t *mi,
                               const char *outdir, const mi_file_t *mf) {
    int len = mi->is_multi
        ? snprintf(fullpath, size, "%s/%s/%s", outdir, mi->name, mf->path)
        : snprintf(fullpath, size, "%s/%s", outdir, mf->path);
    return (len >= 0 && (size_t)len < size);
}

static int build_fallback_path(char *fullpath, size_t size, const char *outdir,
                               uint32_t index, const mi_file_t *mf) {
    char name[128];
    sanitize_component(basename_component(mf->path), name, sizeof(name));
    int len = snprintf(fullpath, size, "%s/_files/%06u_%s",
                       outdir, index, name);
    return (len >= 0 && (size_t)len < size);
}

static int open_disk_file(struct file_handle *fh) {
    fh->fp = fopen(fh->path, "r+b");
    if (fh->fp)
        return 1;

    fh->fp = fopen(fh->path, "w+b");
    if (!fh->fp)
        return 0;

    if (fh->length > 0) {
        fseek(fh->fp, (long)(fh->length - 1), SEEK_SET);
        fputc(0, fh->fp);
        fseek(fh->fp, 0, SEEK_SET);
    }
    return 1;
}

storage_t *storage_open_ex(const metainfo_t *mi, const char *outdir,
                           const storage_file_config_t *configs) {
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
        fh->config.mode = STORAGE_FILE_DISK;
        if (configs)
            fh->config = configs[i];

        if (fh->config.mode != STORAGE_FILE_DISK)
            continue;

        char fullpath[512];
        int using_fallback = !build_original_path(fullpath, sizeof(fullpath),
                                                  mi, outdir, mf);
        if (using_fallback &&
            !build_fallback_path(fullpath, sizeof(fullpath), outdir, i, mf)) {
            log_msg("[storage] output path is too long, fallback failed\n");
            storage_close(s);
            return NULL;
        }

        memcpy(fh->path, fullpath, sizeof(fh->path));
        fh->path[sizeof(fh->path)-1] = '\0';

        char *slash = strrchr(fullpath, '/');
        if (slash) { *slash = 0; mkdirs(fullpath); *slash = '/'; }

        if (!open_disk_file(fh)) {
            int saved_errno = errno;
            if (!using_fallback && saved_errno == ENAMETOOLONG &&
                build_fallback_path(fullpath, sizeof(fullpath), outdir, i, mf)) {
                memcpy(fh->path, fullpath, sizeof(fh->path));
                fh->path[sizeof(fh->path)-1] = '\0';
                slash = strrchr(fullpath, '/');
                if (slash) { *slash = 0; mkdirs(fullpath); *slash = '/'; }
                using_fallback = 1;
                if (!open_disk_file(fh))
                    saved_errno = errno;
            }
            if (!fh->fp) {
                log_msg("[storage] cannot open '%s': %s\n",
                        fh->path, strerror(saved_errno));
                storage_close(s);
                return NULL;
            }
        }

        if (using_fallback)
            log_msg("[storage] long output path remapped to '%s'\n", fh->path);
    }
    return s;
}

storage_t *storage_open(const metainfo_t *mi, const char *outdir) {
    return storage_open_ex(mi, outdir, NULL);
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
        size_t can_write = (size_t)(fh->length - local_off);
        if (can_write > len - written) can_write = len - written;
        if (fh->config.mode == STORAGE_FILE_SKIP) {
            written += can_write;
            continue;
        }
        if (fh->config.mode == STORAGE_FILE_SINK) {
            const uint8_t *src = data + written;
            int64_t sink_off = local_off;
            size_t deliver = can_write;
            /*
             * F-B resume: the prefix below ready_bytes was consumed by a
             * previous session; drop re-downloaded bytes and hand the sink
             * only the tail at/after the mark.
             */
            if ((uint64_t)local_off < fh->config.ready_bytes) {
                uint64_t skip = fh->config.ready_bytes - (uint64_t)local_off;
                if (skip >= (uint64_t)deliver) {
                    written += can_write;
                    continue;
                }
                src += skip;
                sink_off += (int64_t)skip;
                deliver -= (size_t)skip;
            }
            if (!fh->config.sink ||
                !fh->config.sink(fh->config.user,
                                 (uint32_t)(fh - s->files), sink_off,
                                 src, deliver)) {
                if (!s->error[0])
                    snprintf(s->error, sizeof(s->error),
                             "stream sink rejected file %u",
                             (unsigned)(fh - s->files));
                return 0;
            }
            written += can_write;
            continue;
        }
        if (!fh->fp) return 0;
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
        if (s->files[i].config.mode == STORAGE_FILE_DISK &&
            (!s->files[i].fp || fflush(s->files[i].fp) != 0))
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
        if (fh->config.mode != STORAGE_FILE_DISK || !fh->fp) return -1;
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

static int range_has_mode(storage_t *s, int64_t offset, size_t len,
                          storage_file_mode_t mode) {
    size_t done = 0;
    while (done < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)done,
                       (int64_t)(len - done), &fh, &local_off))
            return 0;
        if (fh->config.mode != mode)
            return 0;
        size_t count = (size_t)(fh->length - local_off);
        if (count > len - done)
            count = len - done;
        done += count;
    }
    return 1;
}

int storage_range_readable(storage_t *s, int64_t offset, size_t len) {
    return range_has_mode(s, offset, len, STORAGE_FILE_DISK);
}

int storage_range_skipped(storage_t *s, int64_t offset, size_t len) {
    size_t done = 0;
    while (done < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)done,
                       (int64_t)(len - done), &fh, &local_off))
            return 0;
        size_t count = (size_t)(fh->length - local_off);
        if (count > len - done)
            count = len - done;
        if (fh->config.mode == STORAGE_FILE_SKIP) {
            done += count;
            continue;
        }
        /* F-B: the consumed prefix of a resumed SINK file counts as done. */
        if (fh->config.mode == STORAGE_FILE_SINK &&
            (uint64_t)local_off + count <= fh->config.ready_bytes) {
            done += count;
            continue;
        }
        return 0;
    }
    return 1;
}

const char *storage_error(storage_t *s) {
    return s && s->error[0] ? s->error : "";
}

void storage_close(storage_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->num_files; i++)
        if (s->files[i].fp) fclose(s->files[i].fp);
    free(s->files);
    free(s);
}
