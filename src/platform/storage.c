#include "storage.h"
#include "../core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#ifdef __SWITCH__
#  include <switch.h>
#endif

#ifndef _WIN32
#  include <unistd.h>
#endif

/* DBI split-archive format: a folder holding parts of at most
   0xFFFF0000 bytes, named 00..NN so alphabetical order is data order. */
#define SPLIT_PART_SIZE ((int64_t)0xFFFF0000u)
#define FAT32_FILE_MAX   ((int64_t)0xFFFFFFFFu) /* 4 GiB - 1 */
/* Port torrents (e.g. Lain, 8352 files) need headroom for outdir/name/rel. */
#define STORAGE_PATH_MAX 768
/* Above this file count, store flat under outdir/_files/ — nested torrent
   names with brackets hit Switch path limits and lazy-open exhausts fds. */
#define FLAT_FILE_LAYOUT_THRESHOLD 1024
/* ponytail: 64 files/dir so FAT32 LFN (~3 entries/name) stays under a
   512-entry cluster if the dir cannot grow. */
#define FLAT_FILE_SUBDIR_SIZE 64

static int use_flat_file_layout(const metainfo_t *mi);

struct file_handle {
    char  path[STORAGE_PATH_MAX];
    FILE *fp;
    int64_t offset; /* start in torrent flat space */
    int64_t length;
    storage_file_config_t config;
    /* Split mode: fp stays NULL, parts are opened lazily into part_fp. */
    int      split;
    uint32_t num_parts;
    uint32_t part_size;
    FILE    *part_fp;
    uint32_t part_fp_index;
};

struct storage {
    struct file_handle *files;
    uint32_t num_files;
    const metainfo_t *mi;
    const char *outdir;
    char error[256];
};

static char g_open_error[256];

static void copy_open_error(const storage_t *s) {
    if (s && s->error[0])
        snprintf(g_open_error, sizeof(g_open_error), "%s", s->error);
    else
        g_open_error[0] = '\0';
}

void storage_set_error(storage_t *s, const char *msg) {
    if (!s || !msg || s->error[0])
        return;
    snprintf(s->error, sizeof(s->error), "%s", msg);
}

const char *storage_open_error(void) {
    return g_open_error;
}

static void set_path_errno(storage_t *s, const char *path, const char *what) {
    if (!s || s->error[0])
        return;
    snprintf(s->error, sizeof(s->error), "%s '%.200s': %s", what, path,
             strerror(errno));
}

/* One mkdir level where EEXIST only counts when the name is already a
   directory. A regular file holding a directory's name used to pass as
   success here and fail deeper with a cryptic ENOENT (B2: Tomodachi Life
   port, `.../contents/английская озв`) — report the blocker as ENOTDIR. */
static int mkdir_one(char *path) {
    struct stat st;
    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        errno = ENOTDIR;
    return S_ISDIR(st.st_mode) != 0;
}

/* mkdir -p. Skip a "device:" prefix — mkdir("sdmc:") fails with a
   non-EEXIST errno on libnx (see game_metadata_service makeDirectories). */
static int mkdirs(const char *path) {
    char tmp[STORAGE_PATH_MAX];
    int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len < 0 || (size_t)len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    char *start = tmp + 1;
    char *colon = strchr(tmp, ':');
    if (colon && colon[1] == '/')
        start = colon + 2;
    for (char *p = start; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (!mkdir_one(tmp)) {
            *p = '/';
            return 0;
        }
        *p = '/';
    }
    return mkdir_one(tmp);
}

static int mkdirs_parent(const char *file_path) {
    char parent[STORAGE_PATH_MAX];
    const char *slash = strrchr(file_path, '/');
    if (!slash)
        return 1;
    size_t len = (size_t)(slash - file_path);
    if (len >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    memcpy(parent, file_path, len);
    parent[len] = '\0';
    return mkdirs(parent);
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
    uint32_t subdir = index / FLAT_FILE_SUBDIR_SIZE;
    int len = snprintf(fullpath, size, "%s/_files/%04u/%06u_%s",
                       outdir, subdir, index, name);
    return (len >= 0 && (size_t)len < size);
}

int storage_expected_file_path(const metainfo_t *mi, const char *outdir,
                               uint32_t file_index, char *out, size_t out_size) {
    if (!mi || !outdir || !out || out_size == 0 ||
        file_index >= mi->num_files)
        return 0;
    const mi_file_t *file = &mi->files[file_index];
    char candidate[STORAGE_PATH_MAX];
    int ok;
    if (use_flat_file_layout(mi))
        ok = build_fallback_path(candidate, sizeof(candidate), outdir,
                                 file_index, file);
    else
        ok = build_original_path(candidate, sizeof(candidate), mi, outdir,
                                 file) ||
             build_fallback_path(candidate, sizeof(candidate), outdir,
                                 file_index, file);
    if (!ok)
        return 0;
    int len = snprintf(out, out_size, "%s", candidate);
    return len >= 0 && (size_t)len < out_size;
}

int storage_locate_file_path(const metainfo_t *mi, const char *outdir,
                             uint32_t file_index, char *out, size_t out_size) {
    if (!mi || !outdir || !out || out_size == 0 ||
        file_index >= mi->num_files)
        return 0;
    const mi_file_t *file = &mi->files[file_index];
    char candidate[STORAGE_PATH_MAX];
    if (use_flat_file_layout(mi)) {
        if (!storage_expected_file_path(mi, outdir, file_index, out, out_size))
            return 0;
        return access(out, F_OK) == 0;
    }
    if (build_original_path(candidate, sizeof(candidate), mi, outdir, file) &&
        access(candidate, F_OK) == 0) {
        int len = snprintf(out, out_size, "%s", candidate);
        return len >= 0 && (size_t)len < out_size;
    }
    if (build_fallback_path(candidate, sizeof(candidate), outdir, file_index,
                            file) && access(candidate, F_OK) == 0) {
        int len = snprintf(out, out_size, "%s", candidate);
        return len >= 0 && (size_t)len < out_size;
    }
    return 0;
}

/* Preallocate size bytes (sparse on PC) so later writes cannot fail with
   ENOSPC/EFBIG unexpectedly. Returns 0 when the final byte cannot be
   written — the FAT32 probe relies on that. */
static int prealloc(FILE *fp, int64_t size) {
    if (size <= 0)
        return 1;
    /* ponytail: FAT32 fseek+fflush per file dominates 8k-file ports; the
       4 GiB probe only matters above 1 MiB. ENOSPC on tiny files surfaces
       at write. */
    if (size < 1024 * 1024)
        return 1;
    if (fseek(fp, (long)(size - 1), SEEK_SET) != 0)
        return 0;
    if (fputc(0, fp) == EOF)
        return 0;
    if (fflush(fp) != 0)
        return 0;
    fseek(fp, 0, SEEK_SET);
    return 1;
}

static int64_t split_part_size_at(const struct file_handle *fh, uint32_t idx) {
    int64_t left = fh->length - (int64_t)idx * fh->part_size;
    return left < fh->part_size ? left : fh->part_size;
}

static int split_part_path(const struct file_handle *fh, uint32_t idx,
                           char *out, size_t out_size) {
    /* Zero-padded decimal so alphabetical order = data order. Width grows
       beyond 99 parts, keeping "100" after "99". */
    int width = 2;
    for (uint32_t n = fh->num_parts - 1; n >= 100; n /= 10)
        width++;
    int len = snprintf(out, out_size, "%s/%0*u", fh->path, width, idx);
    return len >= 0 && (size_t)len < out_size;
}

static int split_setup(struct file_handle *fh) {
    fh->split = 1;
    fh->part_size = (uint32_t)SPLIT_PART_SIZE;
    fh->num_parts =
        (uint32_t)((fh->length + SPLIT_PART_SIZE - 1) / SPLIT_PART_SIZE);
    fh->part_fp = NULL;
    fh->part_fp_index = (uint32_t)-1;
    return 1;
}

/* Resume: the path is already a split folder from a previous session. */
static int split_open(struct file_handle *fh) {
    return split_setup(fh);
}

/* Fresh session: the >4 GiB probe failed, store as a DBI split folder. */
static int split_create(struct file_handle *fh, storage_t *s) {
    if (fh->fp) {
        fclose(fh->fp);
        fh->fp = NULL;
    }
    remove(fh->path);
    if (mkdir(fh->path, 0755) != 0) {
        if (s)
            set_path_errno(s, fh->path, "cannot create split folder");
        return 0;
    }
    if (!split_setup(fh))
        return 0;
    for (uint32_t i = 0; i < fh->num_parts; i++) {
        char ppath[STORAGE_PATH_MAX + 16];
        if (!split_part_path(fh, i, ppath, sizeof(ppath)))
            return 0;
        FILE *p = fopen(ppath, "w+b");
        if (!p) {
            if (s)
                set_path_errno(s, ppath, "cannot create split part");
            return 0;
        }
        int ok = prealloc(p, split_part_size_at(fh, i));
        if (!ok) {
            if (s) {
                if (errno == ENOSPC)
                    snprintf(s->error, sizeof(s->error),
                             "not enough space for split part '%.200s'",
                             ppath);
                else
                    snprintf(s->error, sizeof(s->error),
                             "cannot allocate split part '%.200s'", ppath);
            }
        }
        fclose(p);
        if (!ok)
            return 0;
    }
    log_msg("[storage] split '%s' into %u parts\n", fh->path, fh->num_parts);
    return 1;
}

static FILE *split_part_open(struct file_handle *fh, uint32_t idx) {
    if (idx >= fh->num_parts)
        return NULL;
    if (fh->part_fp && fh->part_fp_index == idx)
        return fh->part_fp;
    if (fh->part_fp) {
        fclose(fh->part_fp);
        fh->part_fp = NULL;
    }
    char ppath[STORAGE_PATH_MAX + 16];
    if (!split_part_path(fh, idx, ppath, sizeof(ppath)))
        return NULL;
    FILE *p = fopen(ppath, "r+b");
    if (!p) {
        /* Missing part (deleted mid-resume): recreate preallocated; piece
           hashes re-download whatever it held. */
        p = fopen(ppath, "w+b");
        if (p && !prealloc(p, split_part_size_at(fh, idx))) {
            fclose(p);
            p = NULL;
        }
    }
    if (!p)
        return NULL;
    fh->part_fp = p;
    fh->part_fp_index = idx;
    return p;
}

static int open_disk_file(struct file_handle *fh, storage_t *s) {
    fh->fp = fopen(fh->path, "r+b");
    if (fh->fp)
        return 1;

    /* Previous session's split folder? */
    struct stat st;
    if (stat(fh->path, &st) == 0 && S_ISDIR(st.st_mode))
        return split_open(fh);

    fh->fp = fopen(fh->path, "w+b");
    if (!fh->fp) {
        if (s)
            set_path_errno(s, fh->path, "cannot open output file");
        return 0;
    }

    /* Files above the FAT32 ceiling: preallocating the final byte probes
       whether they can exist as one file. If it cannot land (FAT32) or the
       caller forced it, store as a DBI split folder instead. exFAT and PC
       filesystems pass the probe and stay a plain file.
       ponytail: the probe can also fail for other reasons (full SD on
       exFAT) and then splits too — harmless, parts are still correct. */
    int probe_ok = prealloc(fh->fp, fh->length);
    if (!probe_ok && fh->length <= FAT32_FILE_MAX && s) {
        if (errno == ENOSPC)
            snprintf(s->error, sizeof(s->error),
                     "not enough space for '%.200s'", fh->path);
        else
            snprintf(s->error, sizeof(s->error),
                     "cannot allocate '%.200s'", fh->path);
    }
    if (fh->length > FAT32_FILE_MAX &&
        (fh->config.force_split || !probe_ok))
        return split_create(fh, s);
    if (!probe_ok)
        return 0;
    return 1;
}

static int use_flat_file_layout(const metainfo_t *mi) {
    return mi && mi->num_files > FLAT_FILE_LAYOUT_THRESHOLD;
}

static void close_disk_handle(struct file_handle *fh) {
    if (!fh)
        return;
    if (fh->part_fp) {
        fflush(fh->part_fp);
        fclose(fh->part_fp);
        fh->part_fp = NULL;
        fh->part_fp_index = (uint32_t)-1;
    }
    if (fh->fp) {
        fflush(fh->fp);
        fclose(fh->fp);
        fh->fp = NULL;
    }
}

static void release_disk_handles(storage_t *s) {
    if (!s)
        return;
    for (uint32_t i = 0; i < s->num_files; i++) {
        if (s->files[i].config.mode != STORAGE_FILE_DISK)
            continue;
        close_disk_handle(&s->files[i]);
    }
}

static int assign_disk_path(struct file_handle *fh, const metainfo_t *mi,
                            const char *outdir, uint32_t index,
                            const mi_file_t *mf, storage_t *s) {
    char fullpath[STORAGE_PATH_MAX];
    if (use_flat_file_layout(mi)) {
        if (!build_fallback_path(fullpath, sizeof(fullpath), outdir, index,
                                 mf)) {
            log_msg("[storage] flat output path failed index=%u\n", index);
            if (s)
                storage_set_error(s, "output path is too long");
            return 0;
        }
    } else if (!build_original_path(fullpath, sizeof(fullpath), mi, outdir,
                                     mf) &&
               !build_fallback_path(fullpath, sizeof(fullpath), outdir, index,
                                    mf)) {
        log_msg("[storage] output path is too long, fallback failed\n");
        if (s)
            storage_set_error(s, "output path is too long");
        return 0;
    }
    memcpy(fh->path, fullpath, sizeof(fh->path));
    fh->path[sizeof(fh->path) - 1] = '\0';
    return 1;
}

static int ensure_disk_file_open(struct file_handle *fh, storage_t *s) {
    if (fh->fp || fh->split)
        return 1;

    // A nested original path can fail mkdirs on the target FS (Switch path
    // limits, a file holding a directory's name) while the short sanitized
    // _files/ fallback below still fits — try it before giving up (B2).
    if (mkdirs_parent(fh->path)) {
        if (open_disk_file(fh, s))
            return 1;
    } else if (s) {
        set_path_errno(s, fh->path, "cannot mkdir for");
    }

    if (strstr(fh->path, "/_files/") || !s || !s->mi || !s->outdir ||
        use_flat_file_layout(s->mi))
        return 0;

    const uint32_t index = (uint32_t)(fh - s->files);
    char fallback[STORAGE_PATH_MAX];
    if (!build_fallback_path(fallback, sizeof(fallback), s->outdir, index,
                             &s->mi->files[index]))
        return 0;

    memcpy(fh->path, fallback, sizeof(fh->path));
    fh->path[sizeof(fh->path) - 1] = '\0';
    s->error[0] = '\0';
    if (!mkdirs_parent(fh->path)) {
        if (s)
            set_path_errno(s, fh->path, "cannot mkdir for");
        return 0;
    }
    if (!open_disk_file(fh, s))
        return 0;

    log_msg("[storage] open failed, remapped to '%s'\n", fh->path);
    return 1;
}

storage_t *storage_open_ex(const metainfo_t *mi, const char *outdir,
                           const storage_file_config_t *configs) {
    g_open_error[0] = '\0';
    storage_t *s = (storage_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mi = mi;
    s->outdir = outdir;
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

        if (!assign_disk_path(fh, mi, outdir, i, mf, s)) {
            copy_open_error(s);
            storage_close(s);
            return NULL;
        }
        /* Lazy open for most files; split handles and resume folders must
           exist before the first write (tests + split resume). */
        if (fh->config.force_split) {
            if (!ensure_disk_file_open(fh, s)) {
                if (!s->error[0])
                    set_path_errno(s, fh->path, "cannot open output file");
                log_msg("[storage] cannot open '%s': %s\n",
                        fh->path, s->error[0] ? s->error : strerror(errno));
                copy_open_error(s);
                storage_close(s);
                return NULL;
            }
            continue;
        }
        struct stat existing;
        if (stat(fh->path, &existing) == 0 && S_ISDIR(existing.st_mode)) {
            if (!ensure_disk_file_open(fh, s)) {
                if (!s->error[0])
                    set_path_errno(s, fh->path, "cannot open output file");
                log_msg("[storage] cannot open '%s': %s\n",
                        fh->path, s->error[0] ? s->error : strerror(errno));
                copy_open_error(s);
                storage_close(s);
                return NULL;
            }
        }
        /* Otherwise open on first read/write — see ensure_disk_file_open. */
    }
    return s;
}

storage_t *storage_open(const metainfo_t *mi, const char *outdir) {
    return storage_open_ex(mi, outdir, NULL);
}

static int find_file(storage_t *s, int64_t off,
                     int64_t len __attribute__((unused)),
                     struct file_handle **fh_out, int64_t *local_off) {
    /* Files are laid out contiguously in ascending flat-offset order (the
       cumulative sum from the metainfo), so the owning file is the last one
       starting at or before `off`. Binary search — this runs per segment of
       every read and write. */
    uint32_t lo = 0, hi = s->num_files;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->files[mid].offset <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return 0;
    struct file_handle *fh = &s->files[lo - 1];
    if (off >= fh->offset + fh->length)
        return 0;
    *fh_out = fh;
    *local_off = off - fh->offset;
    return 1;
}

int storage_write(storage_t *s, int64_t offset, const uint8_t *data, size_t len) {
    size_t written = 0;
    const struct file_handle *err_fh = NULL;
    while (written < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)written, (int64_t)(len - written), &fh, &local_off)) {
            release_disk_handles(s);
            return 0;
        }
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
                release_disk_handles(s);
                return 0;
            }
            written += can_write;
            continue;
        }
        if (fh->split) {
            int64_t pos = local_off;
            int64_t end = local_off + can_write;
            while (pos < end) {
                uint32_t part_idx = (uint32_t)(pos / fh->part_size);
                FILE *fp = split_part_open(fh, part_idx);
                if (!fp) {
                    err_fh = fh;
                    goto write_fail;
                }
                int64_t seek = pos - (int64_t)part_idx * fh->part_size;
                int64_t chunk = end - pos;
                int64_t part_end = (int64_t)(part_idx + 1) * fh->part_size;
                if (part_end - pos < chunk)
                    chunk = part_end - pos;
                if (fseek(fp, (long)seek, SEEK_SET) != 0) {
                    err_fh = fh;
                    goto write_fail;
                }
                size_t w = fwrite(data + written, 1, (size_t)chunk, fp);
                if (w != (size_t)chunk) {
                    err_fh = fh;
                    goto write_fail;
                }
                pos += (int64_t)w;
                written += w;
            }
            close_disk_handle(fh);
            continue;
        }
        if (!ensure_disk_file_open(fh, s)) {
            if (!s->error[0])
                set_path_errno(s, fh->path, "cannot open output file");
            release_disk_handles(s);
            return 0;
        }
        if (fseek(fh->fp, (long)local_off, SEEK_SET) != 0) {
            err_fh = fh;
            goto write_fail;
        }
        size_t w = fwrite(data + written, 1, can_write, fh->fp);
        if (w != can_write) {
            err_fh = fh;
            goto write_fail;
        }
        written += w;
        close_disk_handle(fh);
    }
    return 1;

write_fail:
    {
        int saved_errno = errno;
        if (!s->error[0] && err_fh)
            snprintf(s->error, sizeof(s->error), "write '%.200s': %s",
                     err_fh->path, strerror(saved_errno));
        release_disk_handles(s);
        return 0;
    }
}

int storage_flush(storage_t *s) {
    if (!s) return 0;
    for (uint32_t i = 0; i < s->num_files; i++) {
        struct file_handle *fh = &s->files[i];
        if (fh->config.mode != STORAGE_FILE_DISK)
            continue;
        if (fh->split) {
            /* Parts closed by a switch were flushed on close; only the
               cached part can hold buffered data. */
            if (fh->part_fp && fflush(fh->part_fp) != 0)
                return 0;
        } else if (fh->fp && fflush(fh->fp) != 0) {
            return 0;
        }
    }
    return 1;
}

int storage_read(storage_t *s, int64_t offset, uint8_t *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        struct file_handle *fh;
        int64_t local_off;
        if (!find_file(s, offset + (int64_t)done, (int64_t)(len - done), &fh, &local_off)) {
            release_disk_handles(s);
            return -1;
        }
        if (fh->config.mode != STORAGE_FILE_DISK) {
            release_disk_handles(s);
            return -1;
        }
        if (!ensure_disk_file_open(fh, s)) {
            release_disk_handles(s);
            return -1;
        }
        if (!fh->split && !fh->fp) {
            release_disk_handles(s);
            return -1;
        }
        size_t can_read = (size_t)(fh->length - local_off);
        if (can_read > len - done) can_read = len - done;
        int64_t pos = local_off;
        int64_t end = local_off + can_read;
        while (pos < end) {
            FILE *fp = fh->fp;
            int64_t seek = pos;
            int64_t chunk = end - pos;
            if (fh->split) {
                uint32_t part_idx = (uint32_t)(pos / fh->part_size);
                fp = split_part_open(fh, part_idx);
                if (!fp) {
                    release_disk_handles(s);
                    return -1;
                }
                seek = pos - (int64_t)part_idx * fh->part_size;
                int64_t part_end = (int64_t)(part_idx + 1) * fh->part_size;
                if (part_end - pos < chunk)
                    chunk = part_end - pos;
            }
            clearerr(fp);
            if (fseek(fp, (long)seek, SEEK_SET) != 0) {
                release_disk_handles(s);
                return -1;
            }
            size_t r = fread(data + done, 1, (size_t)chunk, fp);
            done += r;
            if (r != (size_t)chunk) {
                release_disk_handles(s);
                return -1;
            }
            pos += (int64_t)r;
        }
        close_disk_handle(fh);
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
    for (uint32_t i = 0; i < s->num_files; i++) {
        if (s->files[i].fp) fclose(s->files[i].fp);
        if (s->files[i].part_fp) fclose(s->files[i].part_fp);
    }
    free(s->files);
    free(s);
}

void storage_finalize(storage_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->num_files; i++) {
        const struct file_handle *fh = &s->files[i];
        if (fh->config.mode != STORAGE_FILE_DISK || !fh->split)
            continue;
#ifdef __SWITCH__
        /* DBI archive trick: HOS then reads the whole folder as one file
           (concatenating 00..NN in name order). */
        Result rc = fsdevSetConcatenationFileAttribute(fh->path);
        log_msg("[storage] concat attribute '%s': 0x%08x\n", fh->path, rc);
#else
        (void)fh;
#endif
    }
}
