/* isofs.c -- the game's disc read straight out of its ISO.
 *
 * The runtime's two file layers (libs/filesystem/cellFs.c and the port's
 * ppu_fs.cpp / sys_fs.c) resolve guest paths such as
 * /dev_bdvd/PS3_GAME/USRDIR/DATA.CPK to host paths under PS3_VFS_ROOT and then
 * use the ordinary host API: fopen/fread/fseeko, open/fdopen, stat, opendir/
 * readdir. Instead of extracting the disc, this file sits under that API: the
 * link wraps those functions (hackversus/CMakeLists.txt, -Wl,--wrap=...), and
 * every call whose path lies under the mount prefix is served from an ISO9660
 * image through pread() on one file descriptor -- the descriptor Android's
 * document picker hands the app for the user's own copy of the disc, which
 * needs no storage permission at all. Anything outside the prefix goes to the
 * real function untouched.
 *
 * Only reads: the disc is read-only on the console too. Files are looked up
 * case-insensitively (the title asks in upper case, ISO9660 stores upper
 * case, but nothing is lost by being lenient). Files over 4 GB come as several
 * extents with the same name (the >4 GB rule of ISO9660), handled as one. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ISO_SECTOR 2048u

typedef struct iso_ext { uint32_t lba, len; } iso_ext;
typedef struct iso_ent {
    char*           name;
    int             is_dir;
    uint64_t        size;
    int             n_ext;
    iso_ext         ext[8];
    struct iso_ent* children;
    int             n_children, cap_children;
    int             loaded;          /* directory listing parsed */
    uint32_t        dir_lba, dir_len;
} iso_ent;

static int      s_fd = -1;
static iso_ent  s_root;
static char     s_prefix[512];
static size_t   s_prefix_len;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- */
static int iso_pread(void* buf, size_t n, uint64_t off)
{
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        ssize_t r = pread64(s_fd, p, n, (off64_t)off);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r; off += (uint64_t)r;
    }
    return 0;
}

static iso_ent* ent_add(iso_ent* dir)
{
    if (dir->n_children == dir->cap_children) {
        int cap = dir->cap_children ? dir->cap_children * 2 : 16;
        iso_ent* n = (iso_ent*)realloc(dir->children, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        dir->children = n; dir->cap_children = cap;
    }
    iso_ent* e = &dir->children[dir->n_children++];
    memset(e, 0, sizeof *e);
    return e;
}

/* One directory extent: ISO9660 directory records (ECMA-119 9.1), little-endian
 * halves of the both-byte-order fields. */
static int dir_load(iso_ent* dir)
{
    if (dir->loaded) return 0;
    uint8_t* data = (uint8_t*)malloc(dir->dir_len);
    if (!data || iso_pread(data, dir->dir_len, (uint64_t)dir->dir_lba * ISO_SECTOR) != 0) { free(data); return -1; }
    uint32_t off = 0;
    while (off < dir->dir_len) {
        uint8_t ln = data[off];
        if (ln == 0) { off = (off / ISO_SECTOR + 1) * ISO_SECTOR; continue; }   /* rest of the sector unused */
        if (off + ln > dir->dir_len || ln < 34) break;
        const uint8_t* rec = data + off;
        uint32_t ext_lba, ext_len; memcpy(&ext_lba, rec + 2, 4); memcpy(&ext_len, rec + 10, 4);
        uint8_t flags = rec[25], name_len = rec[32];
        const uint8_t* name = rec + 33;
        off += ln;
        if (name_len == 1 && (name[0] == 0 || name[0] == 1)) continue;   /* . and .. */
        char nm[256]; size_t k = 0;
        for (size_t i = 0; i < name_len && k < sizeof nm - 1; i++) { if (name[i] == ';') break; nm[k++] = (char)name[i]; }
        nm[k] = 0;
        if (k && nm[k - 1] == '.') nm[k - 1] = 0;      /* "NAME." for an extensionless file */
        iso_ent* last = dir->n_children ? &dir->children[dir->n_children - 1] : NULL;
        if (last && !last->is_dir && last->n_ext < 8 && strcmp(last->name, nm) == 0 && (last->ext[last->n_ext - 1].len % ISO_SECTOR) == 0) {
            /* continuation extent of a >4 GB file (the previous record had the multi-extent flag) */
            last->ext[last->n_ext++] = (iso_ext){ ext_lba, ext_len };
            last->size += ext_len;
            continue;
        }
        iso_ent* e = ent_add(dir);
        if (!e) { free(data); return -1; }
        e->name = strdup(nm);
        e->is_dir = (flags & 2) != 0;
        e->n_ext = 1; e->ext[0] = (iso_ext){ ext_lba, ext_len };
        e->size = ext_len;
        e->dir_lba = ext_lba; e->dir_len = ext_len;
        (void)flags;
    }
    free(data);
    dir->loaded = 1;
    return 0;
}

int hv_isofs_mount(int fd, const char* prefix)
{
    uint8_t pvd[ISO_SECTOR];
    s_fd = fd;
    if (iso_pread(pvd, sizeof pvd, 16u * ISO_SECTOR) != 0 || memcmp(pvd + 1, "CD001", 5) != 0) {
        fprintf(stderr, "[isofs] fd %d is not an ISO9660 image\n", fd);
        s_fd = -1; return -1;
    }
    memset(&s_root, 0, sizeof s_root);
    s_root.is_dir = 1; s_root.name = "";
    memcpy(&s_root.dir_lba, pvd + 156 + 2, 4);
    memcpy(&s_root.dir_len, pvd + 156 + 10, 4);
    snprintf(s_prefix, sizeof s_prefix, "%s", prefix);
    s_prefix_len = strlen(s_prefix);
    while (s_prefix_len > 1 && s_prefix[s_prefix_len - 1] == '/') s_prefix[--s_prefix_len] = 0;
    if (dir_load(&s_root) != 0) { fprintf(stderr, "[isofs] cannot read the root directory\n"); s_fd = -1; return -1; }
    char label[33]; memcpy(label, pvd + 40, 32); label[32] = 0;
    for (int i = 31; i >= 0 && label[i] == ' '; i--) label[i] = 0;
    fprintf(stderr, "[isofs] mounted '%s' at %s (%d root entries)\n", label, s_prefix, s_root.n_children);
    return 0;
}

/* The entry for a host path under the prefix, NULL if it is not there; *under
 * tells whether the path was under the prefix at all. */
static iso_ent* iso_lookup(const char* path, int* under)
{
    *under = 0;
    if (s_fd < 0 || !path) return NULL;
    if (strncmp(path, s_prefix, s_prefix_len) != 0) return NULL;
    const char* rel = path + s_prefix_len;
    if (*rel && *rel != '/') return NULL;
    *under = 1;
    pthread_mutex_lock(&s_mu);
    iso_ent* cur = &s_root;
    while (*rel) {
        while (*rel == '/') rel++;
        if (!*rel) break;
        const char* end = strchr(rel, '/'); size_t n = end ? (size_t)(end - rel) : strlen(rel);
        if (n == 1 && rel[0] == '.') { rel += n; continue; }
        if (!cur->is_dir || dir_load(cur) != 0) { cur = NULL; break; }
        iso_ent* next = NULL;
        for (int i = 0; i < cur->n_children; i++)
            if (strlen(cur->children[i].name) == n && strncasecmp(cur->children[i].name, rel, n) == 0) { next = &cur->children[i]; break; }
        cur = next;
        if (!cur) break;
        rel += n;
    }
    if (cur && cur->is_dir) dir_load(cur);
    pthread_mutex_unlock(&s_mu);
    return cur;
}

/* ------------------------------------------------------------------------- */
/* Open files: one iso_stream per open, reached either as a fake fd (open())
 * or as a FILE* (fopen(), or fdopen() over the fake fd). */
typedef struct iso_stream { iso_ent* e; uint64_t pos; int fd; FILE* fp; } iso_stream;
#define ISO_FD_BASE 0x40000000
#define ISO_MAX_OPEN 256
static iso_stream* s_open[ISO_MAX_OPEN];

static ssize_t stream_read_at(iso_stream* s, void* buf, size_t n, uint64_t pos)
{
    if (pos >= s->e->size) return 0;
    if (n > s->e->size - pos) n = (size_t)(s->e->size - pos);
    uint8_t* p = (uint8_t*)buf; size_t done = 0; uint64_t skip = pos;
    for (int i = 0; i < s->e->n_ext && done < n; i++) {
        uint64_t len = s->e->ext[i].len;
        if (skip >= len) { skip -= len; continue; }
        size_t take = (size_t)(len - skip); if (take > n - done) take = n - done;
        if (iso_pread(p + done, take, (uint64_t)s->e->ext[i].lba * ISO_SECTOR + skip) != 0) return done ? (ssize_t)done : -1;
        done += take; skip = 0;
    }
    return (ssize_t)done;
}

static iso_stream* stream_new(iso_ent* e)
{
    iso_stream* s = (iso_stream*)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->e = e; s->fd = -1;
    return s;
}
static int stream_fd_alloc(iso_stream* s)
{
    pthread_mutex_lock(&s_mu);
    for (int i = 0; i < ISO_MAX_OPEN; i++)
        if (!s_open[i]) { s_open[i] = s; s->fd = ISO_FD_BASE + i; pthread_mutex_unlock(&s_mu); return s->fd; }
    pthread_mutex_unlock(&s_mu);
    return -1;
}
static iso_stream* stream_of_fd(int fd)
{
    if (fd < ISO_FD_BASE || fd >= ISO_FD_BASE + ISO_MAX_OPEN) return NULL;
    pthread_mutex_lock(&s_mu); iso_stream* s = s_open[fd - ISO_FD_BASE]; pthread_mutex_unlock(&s_mu);
    return s;
}
static iso_stream* stream_of_fp(FILE* fp)
{
    if (!fp) return NULL;
    pthread_mutex_lock(&s_mu);
    iso_stream* r = NULL;
    for (int i = 0; i < ISO_MAX_OPEN; i++) if (s_open[i] && s_open[i]->fp == fp) { r = s_open[i]; break; }
    pthread_mutex_unlock(&s_mu);
    return r;
}
static void stream_free(iso_stream* s)
{
    pthread_mutex_lock(&s_mu);
    if (s->fd >= ISO_FD_BASE) s_open[s->fd - ISO_FD_BASE] = NULL;
    pthread_mutex_unlock(&s_mu);
    free(s);
}

/* funopen64 (bionic has the BSD form, not fopencookie): the stdio side of a
 * stream -- fread/fseeko/ftello/fclose all land here. */
static int cookie_read(void* c, char* buf, int n)
{
    iso_stream* s = (iso_stream*)c;
    ssize_t r = stream_read_at(s, buf, (size_t)n, s->pos);
    if (r > 0) s->pos += (uint64_t)r;
    return (int)r;
}
static int cookie_write(void* c, const char* buf, int n) { (void)c; (void)buf; (void)n; errno = EROFS; return -1; }
static fpos64_t cookie_seek(void* c, fpos64_t off, int whence)
{
    iso_stream* s = (iso_stream*)c;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)s->pos : (int64_t)s->e->size;
    int64_t np = base + (int64_t)off;
    if (np < 0) { errno = EINVAL; return (fpos64_t)-1; }
    s->pos = (uint64_t)np;
    return (fpos64_t)np;
}
static int cookie_close(void* c) { stream_free((iso_stream*)c); return 0; }

static FILE* stream_open_fp(iso_stream* s)
{
    FILE* fp = funopen64(s, cookie_read, cookie_write, cookie_seek, cookie_close);
    if (fp) { s->fp = fp; if (s->fd < 0) stream_fd_alloc(s); }   /* registered, so fileno() can find it */
    return fp;
}

static int mode_writes(const char* mode) { return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')); }

static void fill_stat(const iso_ent* e, struct stat* st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = e->is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (off_t)e->size;
    st->st_blksize = ISO_SECTOR;
    st->st_blocks = (blkcnt_t)((e->size + 511) / 512);
    st->st_ino = (ino_t)(uintptr_t)e;
}

/* ------------------------------------------------------------------------- */
/* The wrapped host API. __real_* are the libc functions. */
FILE* __real_fopen(const char*, const char*);
FILE* __real_fopen64(const char*, const char*);
int   __real_open(const char*, int, ...);
int   __real_open64(const char*, int, ...);
int   __real_openat(int, const char*, int, ...);
FILE* __real_fdopen(int, const char*);
int   __real_fileno(FILE*);
int   __real_fstat(int, struct stat*);
int   __real_fstat64(int, struct stat64*);
int   __real_stat(const char*, struct stat*);
int   __real_stat64(const char*, struct stat64*);
int   __real_lstat(const char*, struct stat*);
int   __real_access(const char*, int);
int   __real_close(int);
ssize_t __real_read(int, void*, size_t);
ssize_t __real_pread(int, void*, size_t, off_t);
ssize_t __real_pread64(int, void*, size_t, off64_t);
off_t   __real_lseek(int, off_t, int);
off64_t __real_lseek64(int, off64_t, int);
DIR*  __real_opendir(const char*);
struct dirent* __real_readdir(DIR*);
struct dirent64* __real_readdir64(DIR*);
int   __real_closedir(DIR*);
void  __real_rewinddir(DIR*);

FILE* __wrap_fopen(const char* path, const char* mode)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return __real_fopen(path, mode);
    if (!e) { errno = ENOENT; return NULL; }
    if (e->is_dir) { errno = EISDIR; return NULL; }
    if (mode_writes(mode)) { errno = EROFS; return NULL; }
    iso_stream* s = stream_new(e); if (!s) { errno = ENOMEM; return NULL; }
    FILE* fp = stream_open_fp(s); if (!fp) { free(s); errno = ENOMEM; }
    return fp;
}
FILE* __wrap_fopen64(const char* path, const char* mode) { return __wrap_fopen(path, mode); }

static int iso_open_common(const char* path, int flags)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return -2;
    if (!e) { errno = ENOENT; return -1; }
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) { errno = EROFS; return -1; }
    if (e->is_dir && !(flags & O_DIRECTORY)) { errno = EISDIR; return -1; }
    iso_stream* s = stream_new(e); if (!s) { errno = ENOMEM; return -1; }
    int fd = stream_fd_alloc(s);
    if (fd < 0) { free(s); errno = EMFILE; return -1; }
    return fd;
}
int __wrap_open(const char* path, int flags, ...)
{
    int r = iso_open_common(path, flags);
    if (r != -2) return r;
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_open(path, flags, mode);
}
int __wrap_open64(const char* path, int flags, ...)
{
    int r = iso_open_common(path, flags);
    if (r != -2) return r;
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_open64(path, flags, mode);
}
int __wrap_openat(int dirfd, const char* path, int flags, ...)
{
    if (path && path[0] == '/') { int r = iso_open_common(path, flags); if (r != -2) return r; }
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    return __real_openat(dirfd, path, flags, mode);
}
FILE* __wrap_fdopen(int fd, const char* mode)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_fdopen(fd, mode);
    if (mode_writes(mode)) { errno = EROFS; return NULL; }
    if (s->fp) return s->fp;
    return stream_open_fp(s);
}
int __wrap_fileno(FILE* fp)
{
    iso_stream* s = stream_of_fp(fp);
    return s ? s->fd : __real_fileno(fp);
}
int __wrap_fstat(int fd, struct stat* st)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_fstat(fd, st);
    fill_stat(s->e, st); return 0;
}
int __wrap_fstat64(int fd, struct stat64* st)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_fstat64(fd, st);
    fill_stat(s->e, (struct stat*)st); return 0;
}
int __wrap_stat(const char* path, struct stat* st)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return __real_stat(path, st);
    if (!e) { errno = ENOENT; return -1; }
    fill_stat(e, st); return 0;
}
int __wrap_stat64(const char* path, struct stat64* st) { return __wrap_stat(path, (struct stat*)st); }
int __wrap_lstat(const char* path, struct stat* st)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return __real_lstat(path, st);
    if (!e) { errno = ENOENT; return -1; }
    fill_stat(e, st); return 0;
}
int __wrap_access(const char* path, int amode)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return __real_access(path, amode);
    if (!e) { errno = ENOENT; return -1; }
    if (amode & W_OK) { errno = EROFS; return -1; }
    return 0;
}
int __wrap_close(int fd)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_close(fd);
    if (s->fp) { FILE* fp = s->fp; s->fp = NULL; return fclose(fp); }   /* the cookie close frees it */
    stream_free(s); return 0;
}
ssize_t __wrap_read(int fd, void* buf, size_t n)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_read(fd, buf, n);
    ssize_t r = stream_read_at(s, buf, n, s->pos);
    if (r > 0) s->pos += (uint64_t)r;
    return r;
}
ssize_t __wrap_pread64(int fd, void* buf, size_t n, off64_t off)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_pread64(fd, buf, n, off);
    return stream_read_at(s, buf, n, (uint64_t)off);
}
ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_pread(fd, buf, n, off);
    return stream_read_at(s, buf, n, (uint64_t)off);
}
off64_t __wrap_lseek64(int fd, off64_t off, int whence)
{
    iso_stream* s = stream_of_fd(fd);
    if (!s) return __real_lseek64(fd, off, whence);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)s->pos : (int64_t)s->e->size;
    int64_t np = base + off;
    if (np < 0) { errno = EINVAL; return -1; }
    s->pos = (uint64_t)np; return np;
}
off_t __wrap_lseek(int fd, off_t off, int whence) { return (off_t)__wrap_lseek64(fd, off, whence); }

/* Directories: a fake DIR* is a pointer into this table, recognised by identity. */
typedef struct iso_dir { iso_ent* e; int idx; struct dirent64 de; } iso_dir;
static iso_dir* s_dirs[64];
static iso_dir* dir_of(DIR* d)
{
    pthread_mutex_lock(&s_mu);
    iso_dir* r = NULL;
    for (int i = 0; i < 64; i++) if (s_dirs[i] && (DIR*)s_dirs[i] == d) { r = s_dirs[i]; break; }
    pthread_mutex_unlock(&s_mu);
    return r;
}
DIR* __wrap_opendir(const char* path)
{
    int under; iso_ent* e = iso_lookup(path, &under);
    if (!under) return __real_opendir(path);
    if (!e) { errno = ENOENT; return NULL; }
    if (!e->is_dir) { errno = ENOTDIR; return NULL; }
    iso_dir* d = (iso_dir*)calloc(1, sizeof *d); if (!d) { errno = ENOMEM; return NULL; }
    d->e = e;
    pthread_mutex_lock(&s_mu);
    int slot = -1; for (int i = 0; i < 64; i++) if (!s_dirs[i]) { s_dirs[i] = d; slot = i; break; }
    pthread_mutex_unlock(&s_mu);
    if (slot < 0) { free(d); errno = EMFILE; return NULL; }
    return (DIR*)d;
}
struct dirent64* __wrap_readdir64(DIR* dp)
{
    iso_dir* d = dir_of(dp);
    if (!d) return __real_readdir64(dp);
    if (d->idx >= d->e->n_children) return NULL;
    iso_ent* c = &d->e->children[d->idx++];
    memset(&d->de, 0, sizeof d->de);
    snprintf(d->de.d_name, sizeof d->de.d_name, "%s", c->name);
    d->de.d_type = c->is_dir ? DT_DIR : DT_REG;
    d->de.d_ino = (ino64_t)(uintptr_t)c;
    d->de.d_reclen = sizeof d->de;
    return &d->de;
}
struct dirent* __wrap_readdir(DIR* dp)
{
    iso_dir* d = dir_of(dp);
    if (!d) return __real_readdir(dp);
    return (struct dirent*)__wrap_readdir64(dp);   /* same layout on 64-bit bionic */
}
int __wrap_closedir(DIR* dp)
{
    iso_dir* d = dir_of(dp);
    if (!d) return __real_closedir(dp);
    pthread_mutex_lock(&s_mu);
    for (int i = 0; i < 64; i++) if (s_dirs[i] == d) s_dirs[i] = NULL;
    pthread_mutex_unlock(&s_mu);
    free(d); return 0;
}
void __wrap_rewinddir(DIR* dp)
{
    iso_dir* d = dir_of(dp);
    if (!d) { __real_rewinddir(dp); return; }
    d->idx = 0;
}
