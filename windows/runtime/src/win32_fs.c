/* Win32 fs slice: files, directory search, Windows paths, console handles,
 * the registry values the game reads, shell folders and two shell/dialog
 * calls (notes/slice_owners.md, slice "fs"). Contract:
 * runtime/include/win32_thunk.h and docs/WIN32_THUNKS.md, "Windows paths".
 *
 * The virtual Windows layout; nothing else is reachable:
 *   C:\Game\          the game dir, never written to. Writes, creates and
 *                     deletes go to <save>/Game/<same path>; reads look there
 *                     first, then in the game dir (an overlay).
 *   C:\Users\Player\  <save>/Users/Player/
 *   C:\ProgramData\   <save>/ProgramData/
 * C:\ and C:\Users are folders that hold nothing reachable. The current
 * directory is C:\Game\ for the whole run: no import of the game changes it.
 * Each path component matches the host listing case-insensitively. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "diag.h"
#include "win32_thunk.h"
#include "win32_fs.h"

#ifndef GAME_EXE_NAME
#error "GAME_EXE_NAME is not set (windows/build/game.mk sets it from APP_GAME_FILE)"
#endif
#ifndef GAME_REG_KEY
#error "GAME_REG_KEY is not set (windows/build/game.mk sets it from GAME_REG_KEY in game.conf)"
#endif

#define FS_PATH_MAX 1024u /* a guest path, or a host folder from start-up */
#define FS_HOST_MAX 2304u /* host folder + mapped root + guest path */
#define FS_MAX_COMP 128
#define W32_MAX_PATH 260u

#define W32_ERROR_TOO_MANY_OPEN_FILES 4u
#define W32_ERROR_NOT_SAME_DEVICE 17u
#define W32_ERROR_GEN_FAILURE 31u
#define W32_ERROR_FILE_EXISTS 80u
#define W32_ERROR_DISK_FULL 112u
#define W32_ERROR_MOD_NOT_FOUND 126u
#define W32_ERROR_NEGATIVE_SEEK 131u
#define W32_ERROR_DIR_NOT_EMPTY 145u
#define W32_ERROR_FILENAME_EXCED_RANGE 206u
#define W32_ERROR_MORE_DATA 234u

#define W32_GENERIC_READ 0x80000000u
#define W32_GENERIC_WRITE 0x40000000u
#define W32_CREATE_NEW 1u
#define W32_CREATE_ALWAYS 2u
#define W32_OPEN_EXISTING 3u
#define W32_OPEN_ALWAYS 4u
#define W32_TRUNCATE_EXISTING 5u
#define W32_FILE_FLAG_DELETE_ON_CLOSE 0x04000000u

#define W32_FILE_ATTRIBUTE_READONLY 0x01u
#define W32_FILE_ATTRIBUTE_DIRECTORY 0x10u
#define W32_FILE_ATTRIBUTE_NORMAL 0x80u
#define W32_INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DATA_SIZE 36u /* WIN32_FILE_ATTRIBUTE_DATA */
#define W32_INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define W32_INVALID_FILE_SIZE 0xFFFFFFFFu
#define W32_FILE_TYPE_UNKNOWN 0u
#define W32_FILE_TYPE_DISK 1u
#define W32_FILE_TYPE_CHAR 2u

#define W32_FILE_BEGIN 0u
#define W32_FILE_CURRENT 1u
#define W32_FILE_END 2u

#define W32_IMAGE_BASE 0x00400000u
#define W32_EXE_PATH "C:\\Game\\" GAME_EXE_NAME
#define W32_VOLUME_SERIAL 0x4C485052u
#define W32_TEMP_DIR "C:\\Users\\Player\\AppData\\Local\\Temp"

/* WIN32_FIND_DATAA and BY_HANDLE_FILE_INFORMATION. */
#define FIND_DATA_SIZE 320u
#define FIND_DATA_NAME 44u
#define FILE_INFO_SIZE 52u

#define FILETIME_UNIX_EPOCH 116444736000000000ll /* 100 ns ticks, 1601 to 1970 */

static char g_game_dir[FS_PATH_MAX];
static char g_save_dir[FS_PATH_MAX];

static void set_dir(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= FS_PATH_MAX) {
        fprintf(stderr, "win32_fs: folder path too long: %s\n", src);
        abort();
    }
    memcpy(dst, src, n + 1);
    while (n > 1 && dst[n - 1] == '/')
        dst[--n] = 0;
}

void w32_fs_set_game_dir(const char *host_dir) { set_dir(g_game_dir, host_dir); }
void w32_fs_set_save_dir(const char *host_dir) { set_dir(g_save_dir, host_dir); }

/* ---- guest memory ------------------------------------------------------ */

static const char *guest_cstr(uint32_t a) { return a != 0 ? (const char *)guest_ptr(a) : NULL; }

static void put_cstr(uint32_t a, const char *s) { memcpy(guest_ptr(a), s, strlen(s) + 1); }

static void put64(uint32_t a, uint64_t v)
{
    MEM32(a) = (uint32_t)v;
    MEM32(a + 4) = (uint32_t)(v >> 32);
}

static uint64_t get64(uint32_t a) { return (uint64_t)MEM32(a) | ((uint64_t)MEM32(a + 4) << 32); }

static uint64_t filetime_from(struct timespec ts)
{
    return (uint64_t)((int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100 + FILETIME_UNIX_EPOCH);
}

static struct timespec timespec_from(uint64_t ft)
{
    int64_t t = (int64_t)ft - FILETIME_UNIX_EPOCH, rem = t % 10000000;
    struct timespec ts = {(time_t)(t / 10000000), 0};
    if (rem < 0) {
        rem += 10000000;
        ts.tv_sec--;
    }
    ts.tv_nsec = (long)rem * 100;
    return ts;
}

/* ---- host errors ------------------------------------------------------- */

static uint32_t w32_from_errno(int e)
{
    switch (e) {
    case ENOENT:
        return W32_ERROR_FILE_NOT_FOUND;
    case ENOTDIR:
        return W32_ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EISDIR:
    case EROFS:
    case EBUSY:
        return W32_ERROR_ACCESS_DENIED;
    case EEXIST:
        return W32_ERROR_ALREADY_EXISTS;
    case EBADF:
        return W32_ERROR_INVALID_HANDLE;
    case EMFILE:
    case ENFILE:
        return W32_ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM:
        return W32_ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC:
    case EDQUOT:
        return W32_ERROR_DISK_FULL;
    case ENAMETOOLONG:
        return W32_ERROR_FILENAME_EXCED_RANGE;
    case ENOTEMPTY:
        return W32_ERROR_DIR_NOT_EMPTY;
    case EXDEV:
        return W32_ERROR_NOT_SAME_DEVICE;
    case EINVAL:
        return W32_ERROR_INVALID_PARAMETER;
    default:
        return W32_ERROR_GEN_FAILURE;
    }
}

/* ---- ANSI names -------------------------------------------------------- */

/* CP1252 0x80-0x9F; the five unassigned bytes map to the C1 control of the
 * same value, as Windows does. 0xA0-0xFF are Latin-1. */
static const uint16_t k_cp1252_80[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* A guest (CP1252) string as UTF-8, the only bytes APFS takes in a name:
 * mkdir("LEGO\xae ...") fails with EILSEQ. Returns 0, or -1 when out is
 * too small. */
static int ansi_to_utf8(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)in; *s != 0; s++) {
        uint32_t u = *s < 0x80 ? *s : *s < 0xA0 ? k_cp1252_80[*s - 0x80] : *s;
        if (o + 4 > cap)
            return -1;
        if (u < 0x80) {
            out[o++] = (char)u;
        } else if (u < 0x800) {
            out[o++] = (char)(0xC0 | u >> 6);
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | u >> 12);
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[o] = 0;
    return 0;
}

/* The CP1252 byte for code point u, or '?' when CP1252 has none. */
static char ansi_of(uint32_t u)
{
    if (u < 0x80 || (u >= 0xA0 && u <= 0xFF))
        return (char)u;
    for (uint32_t i = 0; i < 32; i++)
        if (k_cp1252_80[i] == u)
            return (char)(0x80 + i);
    return '?';
}

/* A host (UTF-8) name as CP1252. A byte that does not start a valid
 * sequence becomes '?'. */
static void utf8_to_ansi(const char *in, char *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t o = 0;
    while (*s != 0 && o + 1 < cap) {
        uint32_t u = *s, len = u < 0x80 ? 1 : (u & 0xE0) == 0xC0 ? 2 : (u & 0xF0) == 0xE0 ? 3 : (u & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t k = 1;
        if (len > 1) {
            u &= 0x7Fu >> len;
            for (; k < len && (s[k] & 0xC0) == 0x80; k++)
                u = u << 6 | (s[k] & 0x3Fu);
        }
        out[o++] = len != 0 && k == len ? ansi_of(u) : '?';
        s += k;
    }
    out[o] = 0;
}

/* ---- Windows paths ----------------------------------------------------- */

enum fs_root {
    ROOT_DRIVE, /* C:\ or C:\Users */
    ROOT_GAME,
    ROOT_USER,
    ROOT_PROGDATA,
};

/* A guest path after `.` and `..` are collapsed: a mapped root and the
 * components below it, which point into buf. */
struct vpath {
    enum fs_root root;
    int n;
    char *comp[FS_MAX_COMP];
    char buf[FS_PATH_MAX + 8];
};

static int is_sep(char c) { return c == '\\' || c == '/'; }

/* Returns 0, or the Windows error for a path that is malformed, on another
 * drive, or outside the mapped roots. */
static uint32_t parse_path(const char *in, struct vpath *vp)
{
    char *stack[FS_MAX_COMP];
    int depth = 0, popped = 0, skip;
    const char *p = in;

    if (in == NULL || in[0] == 0)
        return W32_ERROR_PATH_NOT_FOUND;
    if (strlen(in) >= FS_PATH_MAX)
        return W32_ERROR_FILENAME_EXCED_RANGE;
    if (is_sep(in[0]) && is_sep(in[1])) {
        THUNK_UNSUPPORTED("UNC or device path %s", in);
        return W32_ERROR_PATH_NOT_FOUND;
    }
    if (in[1] == ':') {
        if (tolower((unsigned char)in[0]) != 'c') {
            THUNK_UNSUPPORTED("drive %c: in %s", in[0], in);
            return W32_ERROR_PATH_NOT_FOUND;
        }
        p = in + 2;
    }
    snprintf(vp->buf, sizeof vp->buf, "%s", is_sep(*p) ? "" : "Game\\");
    if (ansi_to_utf8(p, vp->buf + strlen(vp->buf), sizeof vp->buf - strlen(vp->buf)) != 0)
        return W32_ERROR_FILENAME_EXCED_RANGE;
    for (char *s = vp->buf; s != NULL;) {
        char *tok = s, *end = strpbrk(s, "\\/");
        if (end != NULL) {
            *end = 0;
            s = end + 1;
        } else {
            s = NULL;
        }
        if (tok[0] == 0 || strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (depth == 0)
                return W32_ERROR_ACCESS_DENIED;
            depth--;
            popped = 1;
            continue;
        }
        if (depth == FS_MAX_COMP)
            return W32_ERROR_FILENAME_EXCED_RANGE;
        stack[depth++] = tok;
    }

    if (depth >= 1 && strcasecmp(stack[0], "Game") == 0) {
        vp->root = ROOT_GAME;
        skip = 1;
    } else if (depth >= 2 && strcasecmp(stack[0], "Users") == 0 &&
               strcasecmp(stack[1], "Player") == 0) {
        vp->root = ROOT_USER;
        skip = 2;
    } else if (depth >= 1 && strcasecmp(stack[0], "ProgramData") == 0) {
        vp->root = ROOT_PROGDATA;
        skip = 1;
    } else if (!popped && (depth == 0 || (depth == 1 && strcasecmp(stack[0], "Users") == 0))) {
        vp->root = ROOT_DRIVE;
        skip = depth;
    } else {
        /* A `..` that leaves the mapped roots escapes; anything else is a
         * folder that does not exist here. */
        return popped ? W32_ERROR_ACCESS_DENIED : W32_ERROR_PATH_NOT_FOUND;
    }
    vp->n = depth - skip;
    for (int i = 0; i < vp->n; i++)
        vp->comp[i] = stack[skip + i];
    return 0;
}

/* Host folder of a mapped root; overlay selects <save>/Game for ROOT_GAME. */
static void root_base(enum fs_root root, int overlay, char *out)
{
    switch (root) {
    case ROOT_GAME:
        if (overlay)
            snprintf(out, FS_HOST_MAX, "%s/Game", g_save_dir);
        else
            snprintf(out, FS_HOST_MAX, "%s", g_game_dir);
        break;
    case ROOT_USER:
        snprintf(out, FS_HOST_MAX, "%s/Users/Player", g_save_dir);
        break;
    case ROOT_PROGDATA:
        snprintf(out, FS_HOST_MAX, "%s/ProgramData", g_save_dir);
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void append(char *out, const char *name)
{
    size_t n = strlen(out);
    snprintf(out + n, FS_HOST_MAX - n, "/%s", name);
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int w32_fs_mkdir_p(const char *path)
{
    char buf[FS_HOST_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1;; s++) {
        if (*s != '/' && *s != 0)
            continue;
        char c = *s;
        *s = 0;
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        *s = c;
        if (c == 0)
            break;
    }
    return is_dir(path) ? 0 : (errno = ENOTDIR, -1);
}

/* Find name in the host folder dir, ignoring case; an exact match wins.
 * Returns 1 and the real name in out, 0 when absent, -1 when dir is not a
 * readable folder. */
static int dir_match(const char *dir, const char *name, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, name) != 0)
            continue;
        snprintf(out, cap, "%s", e->d_name);
        found = 1;
        if (strcmp(e->d_name, name) == 0)
            break;
    }
    closedir(d);
    return found;
}

/* Follow vp's components down from base. out gets base, the real names
 * matched and the rest as given. Returns 1 when the whole path exists;
 * *parent is 1 when the folder holding the last component exists. */
static int walk(const char *base, const struct vpath *vp, char *out, int *parent)
{
    int k = 0, r = is_dir(base) ? 1 : -1;
    char name[256];

    snprintf(out, FS_HOST_MAX, "%s", base);
    *parent = 0;
    while (r == 1 && k < vp->n) {
        r = dir_match(out, vp->comp[k], name, sizeof name);
        if (k == vp->n - 1 && r >= 0)
            *parent = 1;
        if (r != 1)
            break;
        append(out, name);
        k++;
    }
    for (int i = k; i < vp->n; i++)
        append(out, vp->comp[i]);
    return r == 1;
}

/* Where a read finds vp. Not for ROOT_DRIVE. */
struct found {
    char path[FS_HOST_MAX];
    int exists;
    int parent;  /* the folder holding it exists */
    int shipped; /* found in the game dir, not overlaid */
};

static void lookup(const struct vpath *vp, struct found *f)
{
    char base[FS_HOST_MAX], path[FS_HOST_MAX];
    int parent;

    f->shipped = 0;
    root_base(vp->root, 1, base);
    if (vp->root != ROOT_GAME)
        w32_fs_mkdir_p(base); /* the mapped user folders always exist */
    f->exists = walk(base, vp, f->path, &f->parent);
    if (f->exists || vp->root != ROOT_GAME)
        return;
    root_base(ROOT_GAME, 0, base);
    if (walk(base, vp, path, &parent)) {
        memcpy(f->path, path, sizeof path);
        f->exists = 1;
        f->shipped = 1;
    }
    f->parent |= parent;
}

static uint32_t not_found(const struct found *f)
{
    return f->parent ? W32_ERROR_FILE_NOT_FOUND : W32_ERROR_PATH_NOT_FOUND;
}

/* Host path for a write, create or delete aimed at vp. Inside C:\Game\ that
 * is below <save>/Game, after creating there the parent folders the game dir
 * has (named as in the game dir). The last component keeps the case of an
 * existing file. Returns 0 or a Windows error. */
static uint32_t resolve_write(const struct vpath *vp, char *out)
{
    char base[FS_HOST_MAX], game[FS_HOST_MAX], sname[256], gname[256];
    int in_game = 1, parent;

    if (vp->root == ROOT_DRIVE || vp->n == 0)
        return W32_ERROR_ACCESS_DENIED;
    root_base(vp->root, 1, base);
    if (w32_fs_mkdir_p(base) != 0)
        return w32_from_errno(errno);
    if (vp->root != ROOT_GAME) {
        walk(base, vp, out, &parent);
        return parent ? 0 : W32_ERROR_PATH_NOT_FOUND;
    }
    root_base(ROOT_GAME, 0, game);
    snprintf(out, FS_HOST_MAX, "%s", base);
    for (int k = 0; k < vp->n; k++) {
        int last = k == vp->n - 1;
        in_game = in_game && dir_match(game, vp->comp[k], gname, sizeof gname) == 1;
        if (in_game)
            append(game, gname);
        int s = dir_match(out, vp->comp[k], sname, sizeof sname);
        if (s < 0)
            return W32_ERROR_PATH_NOT_FOUND;
        if (s == 1) {
            append(out, sname);
        } else if (last) {
            append(out, in_game ? gname : vp->comp[k]);
        } else {
            if (!in_game || !is_dir(game))
                return W32_ERROR_PATH_NOT_FOUND;
            append(out, gname);
            if (mkdir(out, 0755) != 0 && errno != EEXIST)
                return w32_from_errno(errno);
        }
    }
    return 0;
}

/* CreateDirectoryA on vp. */
static uint32_t make_dir(const struct vpath *vp)
{
    struct found f;
    char path[FS_HOST_MAX];
    uint32_t err;

    if (vp->root == ROOT_DRIVE || vp->n == 0)
        return W32_ERROR_ALREADY_EXISTS;
    lookup(vp, &f);
    if (f.exists)
        return W32_ERROR_ALREADY_EXISTS;
    if (!f.parent)
        return W32_ERROR_PATH_NOT_FOUND;
    err = resolve_write(vp, path);
    if (err != 0)
        return err;
    return mkdir(path, 0755) == 0 ? 0 : w32_from_errno(errno);
}

/* SHCreateDirectoryExA: vp and its missing parents. ERROR_ALREADY_EXISTS
 * when the last folder was there already. */
static uint32_t make_dirs(struct vpath *vp)
{
    int n = vp->n;
    uint32_t err = W32_ERROR_ALREADY_EXISTS;

    for (int k = 1; k <= n; k++) {
        vp->n = k;
        err = make_dir(vp);
        if (err != 0 && err != W32_ERROR_ALREADY_EXISTS)
            break;
    }
    vp->n = n;
    return err;
}

/* ---- File objects ------------------------------------------------------ */

struct fs_file {
    struct w32_object obj;
    int fd;
    int console;       /* a std handle */
    int can_read;      /* GENERIC_READ */
    int can_write;     /* GENERIC_WRITE */
    int shipped;       /* opened from the game dir: FILE_ATTRIBUTE_READONLY */
    char *delete_path; /* FILE_FLAG_DELETE_ON_CLOSE */
};

static void file_destroy(struct w32_object *self)
{
    struct fs_file *f = (struct fs_file *)self;
    close(f->fd);
    if (f->delete_path != NULL) {
        unlink(f->delete_path);
        free(f->delete_path);
    }
    free(f);
}

static struct fs_file *file_arg(uint32_t h)
{
    struct fs_file *f = (struct fs_file *)w32_handle_get(h, W32_KIND_FILE);
    if (f == NULL)
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return f;
}

static ssize_t write_all(int fd, const void *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)p + done, n - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/* A disk file reads until n bytes or end of file; a console returns what
 * one read gives. */
static ssize_t read_full(int fd, void *p, size_t n, int once)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)p + done, n - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        done += (size_t)r;
        if (r == 0 || once)
            break;
    }
    return (ssize_t)done;
}

static int copy_file(const char *src, const char *dst)
{
    char buf[16384];
    int in = open(src, O_RDONLY | O_CLOEXEC), out = -1, rc = -1, saved;
    ssize_t n = -1;

    if (in >= 0)
        out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out >= 0) {
        while ((n = read(in, buf, sizeof buf)) > 0)
            if (write_all(out, buf, (size_t)n) != n)
                break;
        rc = n == 0 ? 0 : -1;
    }
    saved = errno;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    if (rc != 0 && out >= 0)
        unlink(dst);
    errno = saved;
    return rc;
}

/* Open what CreateFileA names. Returns the value for GetLastError (0 or
 * ERROR_ALREADY_EXISTS when *fd is open), else the error with *fd = -1. A
 * write to a file that is only in the game dir first copies it below
 * <save>/Game when the content is kept. */
static uint32_t open_path(const char *name, uint32_t access, uint32_t disp, uint32_t flags,
                          int *fd, int *shipped, char *path)
{
    struct vpath vp;
    struct found f;
    uint32_t err = parse_path(name, &vp);
    int writes, oflags = O_RDONLY | O_CLOEXEC;

    *fd = -1;
    *shipped = 0;
    if (err != 0)
        return err;
    if (disp < W32_CREATE_NEW || disp > W32_TRUNCATE_EXISTING ||
        (disp == W32_TRUNCATE_EXISTING && !(access & W32_GENERIC_WRITE)))
        return W32_ERROR_INVALID_PARAMETER;
    if (vp.root == ROOT_DRIVE || vp.n == 0)
        return W32_ERROR_ACCESS_DENIED;
    lookup(&vp, &f);
    if (f.exists && is_dir(f.path))
        return W32_ERROR_ACCESS_DENIED;
    if (!f.exists && (disp == W32_OPEN_EXISTING || disp == W32_TRUNCATE_EXISTING))
        return not_found(&f);
    if (f.exists && disp == W32_CREATE_NEW)
        return W32_ERROR_FILE_EXISTS;
    if (!f.exists && !f.parent)
        return W32_ERROR_PATH_NOT_FOUND;

    writes = !f.exists || (access & W32_GENERIC_WRITE) || (flags & W32_FILE_FLAG_DELETE_ON_CLOSE) ||
             disp == W32_CREATE_ALWAYS || disp == W32_TRUNCATE_EXISTING;
    if (!writes) {
        memcpy(path, f.path, sizeof f.path);
        *shipped = f.shipped;
    } else {
        if (f.shipped && (flags & W32_FILE_FLAG_DELETE_ON_CLOSE))
            return W32_ERROR_ACCESS_DENIED; /* a read-only file */
        err = resolve_write(&vp, path);
        if (err != 0)
            return err;
        if (f.shipped && (disp == W32_OPEN_EXISTING || disp == W32_OPEN_ALWAYS) &&
            copy_file(f.path, path) != 0)
            return w32_from_errno(errno);
        /* Access is enforced per handle; the host descriptor can do both. */
        oflags = O_RDWR | O_CREAT | O_CLOEXEC;
        if (disp == W32_CREATE_NEW)
            oflags |= O_EXCL;
        if (disp == W32_CREATE_ALWAYS || disp == W32_TRUNCATE_EXISTING)
            oflags |= O_TRUNC;
    }
    *fd = open(path, oflags, 0644);
    if (*fd < 0)
        return errno == EEXIST ? W32_ERROR_FILE_EXISTS : w32_from_errno(errno);
    return f.exists && (disp == W32_CREATE_ALWAYS || disp == W32_OPEN_ALWAYS)
               ? W32_ERROR_ALREADY_EXISTS
               : 0;
}

/* ---- Std handles ------------------------------------------------------- */

static struct fs_file g_std[3];
static uint32_t g_std_handles[3];
static pthread_once_t g_std_once = PTHREAD_ONCE_INIT;

static void std_destroy(struct w32_object *self) { (void)self; }

static void std_init(void)
{
    for (int i = 0; i < 3; i++) {
        g_std[i].obj.kind = W32_KIND_FILE;
        g_std[i].obj.destroy = std_destroy;
        g_std[i].fd = i;
        g_std[i].console = 1;
        g_std[i].can_read = i == 0;
        g_std[i].can_write = i != 0;
        g_std_handles[i] = w32_handle_new(&g_std[i].obj);
    }
}

/* ---- Imports: files ---------------------------------------------------- */

/* HANDLE CreateFileA(LPCSTR, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES,
 *                    DWORD disposition, DWORD flags, HANDLE template) */
static void imp_CreateFileA(void)
{
    uint32_t access = W32_ARG(1), flags = W32_ARG(5), h = W32_INVALID_HANDLE_VALUE;
    char path[FS_HOST_MAX];
    int fd, shipped;
    uint32_t err = open_path(guest_cstr(W32_ARG(0)), access, W32_ARG(4), flags, &fd, &shipped, path);

    if (fd >= 0) {
        struct fs_file *f = calloc(1, sizeof *f);
        if (f == NULL)
            abort();
        f->obj.kind = W32_KIND_FILE;
        f->obj.destroy = file_destroy;
        f->fd = fd;
        f->can_read = (access & W32_GENERIC_READ) != 0;
        f->can_write = (access & W32_GENERIC_WRITE) != 0;
        f->shipped = shipped;
        if (flags & W32_FILE_FLAG_DELETE_ON_CLOSE)
            f->delete_path = strdup(path);
        h = w32_handle_new(&f->obj);
        if (h == 0) {
            file_destroy(&f->obj);
            h = W32_INVALID_HANDLE_VALUE;
            err = W32_ERROR_TOO_MANY_OPEN_FILES;
        }
    }
    w32_set_last_error(err);
    W32_RETURN(7, h);
}

/* A synchronous handle given an OVERLAPPED reads or writes at its Offset. */
static int seek_overlapped(struct fs_file *f, uint32_t ov)
{
    THUNK_UNSUPPORTED("OVERLAPPED I/O, done synchronously");
    return lseek(f->fd, (off_t)get64(ov + 8), SEEK_SET) < 0 ? -1 : 0;
}

/* BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD read, LPOVERLAPPED) */
static void imp_ReadFile(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t buf = W32_ARG(1), n = W32_ARG(2), done_p = W32_ARG(3), ov = W32_ARG(4);
    uint32_t done = 0, ok = W32_FALSE;

    if (f == NULL) {
    } else if (!f->can_read) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    } else if (ov != 0 && seek_overlapped(f, ov) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        ssize_t r = n != 0 ? read_full(f->fd, guest_ptr(buf), n, f->console) : 0;
        if (r < 0) {
            w32_set_last_error(w32_from_errno(errno));
        } else {
            done = (uint32_t)r;
            ok = W32_TRUE;
        }
    }
    if (done_p != 0)
        MEM32(done_p) = done;
    W32_RETURN(5, ok);
}

/* BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD written, LPOVERLAPPED) */
static void imp_WriteFile(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t buf = W32_ARG(1), n = W32_ARG(2), done_p = W32_ARG(3), ov = W32_ARG(4);
    uint32_t done = 0, ok = W32_FALSE;

    if (f == NULL) {
    } else if (!f->can_write) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    } else if (ov != 0 && seek_overlapped(f, ov) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        ssize_t r = n != 0 ? write_all(f->fd, guest_ptr(buf), n) : 0;
        if (r < 0) {
            w32_set_last_error(w32_from_errno(errno));
        } else {
            done = (uint32_t)r;
            ok = W32_TRUE;
        }
    }
    if (done_p != 0)
        MEM32(done_p) = done;
    W32_RETURN(5, ok);
}

/* Move f to dist from method. Fails, leaving the position as it was, when
 * the target is negative or above limit. */
static uint32_t seek_to(struct fs_file *f, int64_t dist, uint32_t method, int64_t limit,
                        int64_t *pos)
{
    struct stat st;
    int64_t base;

    switch (method) {
    case W32_FILE_BEGIN:
        base = 0;
        break;
    case W32_FILE_CURRENT:
        base = lseek(f->fd, 0, SEEK_CUR);
        break;
    case W32_FILE_END:
        base = fstat(f->fd, &st) == 0 ? st.st_size : -1;
        break;
    default:
        return W32_ERROR_INVALID_PARAMETER;
    }
    if (base < 0)
        return w32_from_errno(errno);
    if (dist > 0 && base > INT64_MAX - dist)
        return W32_ERROR_INVALID_PARAMETER;
    if (base + dist < 0)
        return W32_ERROR_NEGATIVE_SEEK;
    if (base + dist > limit)
        return W32_ERROR_INVALID_PARAMETER;
    if (lseek(f->fd, base + dist, SEEK_SET) < 0)
        return w32_from_errno(errno);
    *pos = base + dist;
    return 0;
}

/* DWORD SetFilePointer(HANDLE, LONG low, PLONG high, DWORD method) */
static void imp_SetFilePointer(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t low = W32_ARG(1), high_p = W32_ARG(2), method = W32_ARG(3);
    uint32_t result = W32_INVALID_SET_FILE_POINTER;

    if (f != NULL) {
        /* Without a high part the distance is a signed 32-bit value and the
         * new position must fit in 32 bits. */
        int64_t dist = high_p != 0 ? (int64_t)(((uint64_t)MEM32(high_p) << 32) | low)
                                   : (int64_t)(int32_t)low;
        int64_t pos;
        uint32_t err = seek_to(f, dist, method, high_p != 0 ? INT64_MAX : 0xFFFFFFFFll, &pos);
        if (err != 0) {
            w32_set_last_error(err);
        } else {
            if (high_p != 0)
                MEM32(high_p) = (uint32_t)((uint64_t)pos >> 32);
            result = (uint32_t)pos;
            /* The caller tells this from a failure by GetLastError. */
            if (result == W32_INVALID_SET_FILE_POINTER)
                w32_set_last_error(W32_ERROR_SUCCESS);
        }
    }
    W32_RETURN(4, result);
}

/* BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER dist, PLARGE_INTEGER new, DWORD method) */
static void imp_SetFilePointerEx(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    int64_t dist = (int64_t)((uint64_t)W32_ARG(1) | ((uint64_t)W32_ARG(2) << 32)), pos;
    uint32_t new_p = W32_ARG(3), method = W32_ARG(4), ok = W32_FALSE;

    if (f != NULL) {
        uint32_t err = seek_to(f, dist, method, INT64_MAX, &pos);
        if (err != 0) {
            w32_set_last_error(err);
        } else {
            if (new_p != 0)
                put64(new_p, (uint64_t)pos);
            ok = W32_TRUE;
        }
    }
    W32_RETURN(5, ok);
}

/* DWORD GetFileSize(HANDLE, LPDWORD high) */
static void imp_GetFileSize(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t high_p = W32_ARG(1), result = W32_INVALID_FILE_SIZE;
    struct stat st;

    if (f == NULL) {
    } else if (fstat(f->fd, &st) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        if (high_p != 0)
            MEM32(high_p) = (uint32_t)((uint64_t)st.st_size >> 32);
        result = (uint32_t)st.st_size;
        if (result == W32_INVALID_FILE_SIZE)
            w32_set_last_error(W32_ERROR_SUCCESS);
    }
    W32_RETURN(2, result);
}

/* BOOL GetFileSizeEx(HANDLE, PLARGE_INTEGER) */
static void imp_GetFileSizeEx(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t size_p = W32_ARG(1), ok = W32_FALSE;
    struct stat st;

    if (f == NULL) {
    } else if (fstat(f->fd, &st) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        put64(size_p, (uint64_t)st.st_size);
        ok = W32_TRUE;
    }
    W32_RETURN(2, ok);
}

/* BOOL SetEndOfFile(HANDLE) */
static void imp_SetEndOfFile(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t ok = W32_FALSE;
    off_t pos;

    if (f == NULL) {
    } else if (!f->can_write) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    } else if ((pos = lseek(f->fd, 0, SEEK_CUR)) < 0 || ftruncate(f->fd, pos) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        ok = W32_TRUE;
    }
    W32_RETURN(1, ok);
}

/* BOOL FlushFileBuffers(HANDLE) */
static void imp_FlushFileBuffers(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t ok = W32_FALSE;

    if (f == NULL) {
    } else if (!f->can_write) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    } else if (!f->console && fsync(f->fd) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        ok = W32_TRUE;
    }
    W32_RETURN(1, ok);
}

/* DWORD GetFileType(HANDLE) */
static void imp_GetFileType(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t type = W32_FILE_TYPE_UNKNOWN;
    if (f != NULL)
        type = f->console ? W32_FILE_TYPE_CHAR : W32_FILE_TYPE_DISK;
    W32_RETURN(1, type);
}

static uint32_t attributes_of(const struct stat *st, int shipped)
{
    if (S_ISDIR(st->st_mode))
        return W32_FILE_ATTRIBUTE_DIRECTORY;
    return shipped ? W32_FILE_ATTRIBUTE_READONLY : W32_FILE_ATTRIBUTE_NORMAL;
}

/* BOOL GetFileInformationByHandle(HANDLE, LPBY_HANDLE_FILE_INFORMATION) */
static void imp_GetFileInformationByHandle(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t info = W32_ARG(1), ok = W32_FALSE;
    struct stat st;

    if (f == NULL) {
    } else if (fstat(f->fd, &st) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        memset(guest_ptr(info), 0, FILE_INFO_SIZE);
        MEM32(info) = attributes_of(&st, f->shipped);
        put64(info + 4, filetime_from(st.st_birthtimespec));
        put64(info + 12, filetime_from(st.st_atimespec));
        put64(info + 20, filetime_from(st.st_mtimespec));
        MEM32(info + 28) = W32_VOLUME_SERIAL;
        MEM32(info + 32) = (uint32_t)((uint64_t)st.st_size >> 32);
        MEM32(info + 36) = (uint32_t)st.st_size;
        MEM32(info + 40) = (uint32_t)st.st_nlink;
        MEM32(info + 44) = (uint32_t)((uint64_t)st.st_ino >> 32);
        MEM32(info + 48) = (uint32_t)st.st_ino;
        ok = W32_TRUE;
    }
    W32_RETURN(2, ok);
}

/* BOOL SetFileTime(HANDLE, const FILETIME *creation, *access, *write).
 * The host keeps no settable creation time; that argument is ignored. */
static void imp_SetFileTime(void)
{
    struct fs_file *f = file_arg(W32_ARG(0));
    uint32_t atime_p = W32_ARG(2), mtime_p = W32_ARG(3), ok = W32_FALSE;
    struct timespec ts[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};

    if (atime_p != 0)
        ts[0] = timespec_from(get64(atime_p));
    if (mtime_p != 0)
        ts[1] = timespec_from(get64(mtime_p));
    if (f == NULL) {
    } else if (!f->can_write) {
        w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    } else if (futimens(f->fd, ts) != 0) {
        w32_set_last_error(w32_from_errno(errno));
    } else {
        ok = W32_TRUE;
    }
    W32_RETURN(4, ok);
}

/* Attributes and host stat of the guest path at name_va, through the same
 * path mapping as CreateFileA. 0, or a Win32 error code. The drive root is a
 * directory with a zeroed stat. */
static uint32_t path_attributes(uint32_t name_va, uint32_t *attr, struct stat *st)
{
    struct vpath vp;
    struct found f;
    uint32_t err = parse_path(guest_cstr(name_va), &vp);

    memset(st, 0, sizeof *st);
    if (err == 0 && vp.root == ROOT_DRIVE) {
        *attr = W32_FILE_ATTRIBUTE_DIRECTORY;
    } else if (err == 0) {
        lookup(&vp, &f);
        if (!f.exists)
            err = not_found(&f);
        else if (stat(f.path, st) != 0)
            err = w32_from_errno(errno);
        else
            *attr = attributes_of(st, f.shipped);
    }
    return err;
}

/* DWORD GetFileAttributesA(LPCSTR) */
static void imp_GetFileAttributesA(void)
{
    struct stat st;
    uint32_t attr = W32_INVALID_FILE_ATTRIBUTES;
    uint32_t err = path_attributes(W32_ARG(0), &attr, &st);
    if (err != 0)
        w32_set_last_error(err);
    W32_RETURN(1, err == 0 ? attr : W32_INVALID_FILE_ATTRIBUTES);
}

/* BOOL GetFileAttributesExA(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID).
 * Level 0 (GetFileExInfoStandard) is the only one: WIN32_FILE_ATTRIBUTE_DATA,
 * attributes +0, creation/access/write FILETIMEs +4/+12/+20, size high +28,
 * size low +32. */
static void imp_GetFileAttributesExA(void)
{
    uint32_t level = W32_ARG(1), data = W32_ARG(2), attr = 0;
    struct stat st;
    uint32_t err = level != 0 || data == 0 ? W32_ERROR_INVALID_PARAMETER
                                           : path_attributes(W32_ARG(0), &attr, &st);
    if (err == 0) {
        memset(guest_ptr(data), 0, FILE_ATTRIBUTE_DATA_SIZE);
        MEM32(data) = attr;
        if (st.st_mode != 0) {
            put64(data + 4, filetime_from(st.st_birthtimespec));
            put64(data + 12, filetime_from(st.st_atimespec));
            put64(data + 20, filetime_from(st.st_mtimespec));
        }
        if (!S_ISDIR(st.st_mode)) {
            MEM32(data + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
            MEM32(data + 32) = (uint32_t)st.st_size;
        }
    } else {
        w32_set_last_error(err);
    }
    W32_RETURN(3, err == 0 ? W32_TRUE : W32_FALSE);
}

/* BOOL CreateDirectoryA(LPCSTR, LPSECURITY_ATTRIBUTES) */
static void imp_CreateDirectoryA(void)
{
    struct vpath vp;
    uint32_t err = parse_path(guest_cstr(W32_ARG(0)), &vp);

    if (err == 0)
        err = make_dir(&vp);
    if (err != 0)
        w32_set_last_error(err);
    W32_RETURN(2, err == 0 ? W32_TRUE : W32_FALSE);
}

/* int SHCreateDirectoryExA(HWND, LPCSTR, const SECURITY_ATTRIBUTES *) */
static void imp_SHCreateDirectoryExA(void)
{
    struct vpath vp;
    uint32_t err = parse_path(guest_cstr(W32_ARG(1)), &vp);

    if (err == 0)
        err = make_dirs(&vp);
    W32_RETURN(3, err);
}

/* BOOL DeleteFileA(LPCSTR). A file the game dir ships, not overlaid, is
 * read-only; deleting an overlaid one shows the shipped one again. */
static void imp_DeleteFileA(void)
{
    struct vpath vp;
    struct found f;
    uint32_t err = parse_path(guest_cstr(W32_ARG(0)), &vp);

    if (err == 0 && (vp.root == ROOT_DRIVE || vp.n == 0)) {
        err = W32_ERROR_ACCESS_DENIED;
    } else if (err == 0) {
        lookup(&vp, &f);
        if (!f.exists)
            err = not_found(&f);
        else if (f.shipped || is_dir(f.path))
            err = W32_ERROR_ACCESS_DENIED;
        else if (unlink(f.path) != 0)
            err = w32_from_errno(errno);
    }
    if (err != 0)
        w32_set_last_error(err);
    W32_RETURN(1, err == 0 ? W32_TRUE : W32_FALSE);
}

/* BOOL MoveFileA(LPCSTR from, LPCSTR to) */
static void imp_MoveFileA(void)
{
    struct vpath src, dst;
    struct found from, to;
    char path[FS_HOST_MAX];
    uint32_t err = parse_path(guest_cstr(W32_ARG(0)), &src);

    if (err == 0)
        err = parse_path(guest_cstr(W32_ARG(1)), &dst);
    if (err == 0 && (src.root == ROOT_DRIVE || src.n == 0 || dst.root == ROOT_DRIVE))
        err = W32_ERROR_ACCESS_DENIED;
    if (err == 0) {
        lookup(&src, &from);
        if (!from.exists)
            err = not_found(&from);
        else if (from.shipped)
            err = W32_ERROR_ACCESS_DENIED;
    }
    if (err == 0) {
        lookup(&dst, &to);
        if (to.exists)
            err = W32_ERROR_ALREADY_EXISTS;
        else if (!to.parent)
            err = W32_ERROR_PATH_NOT_FOUND;
        else
            err = resolve_write(&dst, path);
    }
    if (err == 0 && rename(from.path, path) != 0)
        err = w32_from_errno(errno);
    if (err != 0)
        w32_set_last_error(err);
    W32_RETURN(2, err == 0 ? W32_TRUE : W32_FALSE);
}

/* UINT GetTempFileNameA(LPCSTR dir, LPCSTR prefix, UINT unique, LPSTR out).
 * With no dir, the mapped Temp folder. */
static void imp_GetTempFileNameA(void)
{
    const char *dir = guest_cstr(W32_ARG(0)), *prefix = guest_cstr(W32_ARG(1));
    uint32_t unique = W32_ARG(2), out = W32_ARG(3), result = 0, err = 0;
    uint32_t u = unique != 0 ? unique : ((uint32_t)getpid() ^ (uint32_t)time(NULL)) & 0xFFFFu;
    char name[FS_PATH_MAX + 32], path[FS_HOST_MAX];
    struct vpath vp;
    int fd, shipped;

    if (dir == NULL || dir[0] == 0) {
        dir = W32_TEMP_DIR;
        parse_path(dir, &vp);
        make_dirs(&vp);
    }
    for (uint32_t tries = 0; tries < 0x10000u; tries++) {
        if (u == 0)
            u = 1;
        size_t len = strlen(dir);
        snprintf(name, sizeof name, "%s%s%.3s%X.TMP", dir, len > 0 && is_sep(dir[len - 1]) ? "" : "\\",
                 prefix != NULL ? prefix : "", unique != 0 ? unique : u);
        if (strlen(name) >= W32_MAX_PATH) {
            err = W32_ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (unique != 0)
            break;
        err = open_path(name, W32_GENERIC_WRITE, W32_CREATE_NEW, 0, &fd, &shipped, path);
        if (fd >= 0)
            close(fd);
        if (err != W32_ERROR_FILE_EXISTS)
            break;
        u = (u + 1u) & 0xFFFFu;
    }
    if (err == 0) {
        put_cstr(out, name);
        result = unique != 0 ? unique : u;
    } else {
        w32_set_last_error(err);
    }
    W32_RETURN(4, result);
}

/* ---- Imports: directory search ----------------------------------------- */

struct fs_entry {
    char *name;
    char *path;
    int shipped;
};

struct fs_find {
    struct w32_object obj;
    struct fs_entry *e;
    size_t n, next;
};

static void find_destroy(struct w32_object *self)
{
    struct fs_find *f = (struct fs_find *)self;
    for (size_t i = 0; i < f->n; i++) {
        free(f->e[i].name);
        free(f->e[i].path);
    }
    free(f->e);
    free(f);
}

/* `*` and `?`, ignoring case. */
static int wild_match(const char *p, const char *s)
{
    const char *star = NULL, *back = NULL;
    while (*s != 0) {
        if (*p == '*') {
            star = ++p;
            back = s;
        } else if (*p != 0 && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))) {
            p++;
            s++;
        } else if (star != NULL) {
            p = star;
            s = ++back;
        } else {
            return 0;
        }
    }
    while (*p == '*')
        p++;
    return *p == 0;
}

/* Add the matching names of one host folder; a name already listed (from
 * the overlay) is skipped. */
static void find_scan(struct fs_find *f, const char *dir, const char *pattern, int shipped)
{
    DIR *d = opendir(dir);
    struct dirent *de;

    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        int dup = 0;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0 ||
            !wild_match(pattern, de->d_name))
            continue;
        for (size_t i = 0; i < f->n && !dup; i++)
            dup = strcasecmp(f->e[i].name, de->d_name) == 0;
        if (dup)
            continue;
        struct fs_entry *e = realloc(f->e, (f->n + 1) * sizeof *e);
        char path[FS_HOST_MAX];
        if (e == NULL)
            abort();
        f->e = e;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        e[f->n].name = strdup(de->d_name);
        e[f->n].path = strdup(path);
        e[f->n].shipped = shipped;
        f->n++;
    }
    closedir(d);
}

/* Merged listing for a FindFirstFileA spec: <folder>\<pattern>. */
static uint32_t find_open(const char *spec, struct fs_find *f)
{
    struct vpath vp;
    char base[FS_HOST_MAX], dir[FS_HOST_MAX];
    int parent, any = 0;
    uint32_t err = parse_path(spec, &vp);

    if (err != 0)
        return err;
    if (vp.root == ROOT_DRIVE || vp.n == 0)
        return W32_ERROR_FILE_NOT_FOUND;
    const char *pattern = vp.comp[--vp.n];
    if (strcmp(pattern, "*.*") == 0)
        pattern = "*";
    for (int overlay = 1; overlay >= 0; overlay--) {
        if (overlay == 0 && vp.root != ROOT_GAME)
            break;
        root_base(vp.root, overlay, base);
        if (vp.root != ROOT_GAME)
            w32_fs_mkdir_p(base);
        if (walk(base, &vp, dir, &parent) && is_dir(dir)) {
            any = 1;
            find_scan(f, dir, pattern, vp.root == ROOT_GAME && overlay == 0);
        }
    }
    if (!any)
        return W32_ERROR_PATH_NOT_FOUND;
    return f->n != 0 ? 0 : W32_ERROR_FILE_NOT_FOUND;
}

static void find_fill(const struct fs_entry *e, uint32_t data)
{
    struct stat st;
    memset(guest_ptr(data), 0, FIND_DATA_SIZE);
    if (stat(e->path, &st) != 0) {
        memset(&st, 0, sizeof st);
        st.st_mode = S_IFREG;
    }
    MEM32(data) = attributes_of(&st, e->shipped);
    put64(data + 4, filetime_from(st.st_birthtimespec));
    put64(data + 12, filetime_from(st.st_atimespec));
    put64(data + 20, filetime_from(st.st_mtimespec));
    MEM32(data + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
    MEM32(data + 32) = (uint32_t)st.st_size;
    utf8_to_ansi(e->name, (char *)guest_ptr(data + FIND_DATA_NAME), W32_MAX_PATH);
}

/* HANDLE FindFirstFileA(LPCSTR, LPWIN32_FIND_DATAA) */
static void imp_FindFirstFileA(void)
{
    uint32_t data = W32_ARG(1), h = W32_INVALID_HANDLE_VALUE;
    struct fs_find *f = calloc(1, sizeof *f);
    uint32_t err;

    if (f == NULL)
        abort();
    f->obj.kind = W32_KIND_FIND;
    f->obj.destroy = find_destroy;
    err = find_open(guest_cstr(W32_ARG(0)), f);
    if (err == 0) {
        find_fill(&f->e[0], data);
        f->next = 1;
        h = w32_handle_new(&f->obj);
        if (h == 0) {
            h = W32_INVALID_HANDLE_VALUE;
            err = W32_ERROR_TOO_MANY_OPEN_FILES;
        }
    }
    if (h == W32_INVALID_HANDLE_VALUE) {
        find_destroy(&f->obj);
        w32_set_last_error(err);
    }
    W32_RETURN(2, h);
}

/* BOOL FindNextFileA(HANDLE, LPWIN32_FIND_DATAA) */
static void imp_FindNextFileA(void)
{
    struct fs_find *f = (struct fs_find *)w32_handle_get(W32_ARG(0), W32_KIND_FIND);
    uint32_t ok = W32_FALSE;

    if (f == NULL) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    } else if (f->next >= f->n) {
        w32_set_last_error(W32_ERROR_NO_MORE_FILES);
    } else {
        find_fill(&f->e[f->next++], W32_ARG(1));
        ok = W32_TRUE;
    }
    W32_RETURN(2, ok);
}

/* BOOL FindClose(HANDLE) */
static void imp_FindClose(void)
{
    uint32_t h = W32_ARG(0), ok = W32_FALSE;
    if (w32_handle_get(h, W32_KIND_FIND) != NULL)
        ok = w32_handle_close(h);
    else
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    W32_RETURN(1, ok);
}

/* ---- Imports: console and std handles ---------------------------------- */

/* HANDLE GetStdHandle(DWORD): -10 input, -11 output, -12 error. */
static void imp_GetStdHandle(void)
{
    uint32_t which = W32_ARG(0), h = W32_INVALID_HANDLE_VALUE;
    pthread_once(&g_std_once, std_init);
    if (which >= 0xFFFFFFF4u && which <= 0xFFFFFFF6u)
        h = g_std_handles[0xFFFFFFF6u - which];
    else
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    W32_RETURN(1, h);
}

/* BOOL SetStdHandle(DWORD, HANDLE): accepted, nothing changes. */
static void imp_SetStdHandle(void) { W32_RETURN(2, W32_TRUE); }

/* UINT SetHandleCount(UINT) */
static void imp_SetHandleCount(void) { W32_RETURN(1, W32_ARG(0)); }

static struct fs_file *console_arg(uint32_t h)
{
    struct fs_file *f = (struct fs_file *)w32_handle_get(h, W32_KIND_FILE);
    if (f == NULL || !f->console || !f->can_write) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return NULL;
    }
    return f;
}

/* BOOL WriteConsoleA(HANDLE, const VOID *, DWORD, LPDWORD written, LPVOID) */
static void imp_WriteConsoleA(void)
{
    struct fs_file *f = console_arg(W32_ARG(0));
    uint32_t n = W32_ARG(2), done_p = W32_ARG(3), ok = W32_FALSE;

    if (f != NULL && write_all(f->fd, guest_ptr(W32_ARG(1)), n) == (ssize_t)n) {
        if (done_p != 0)
            MEM32(done_p) = n;
        ok = W32_TRUE;
    } else if (f != NULL) {
        w32_set_last_error(w32_from_errno(errno));
    }
    W32_RETURN(5, ok);
}

/* n UTF-16 units to UTF-8; out holds at least 3 * n bytes. */
static size_t utf8_from_utf16(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = in[2 * i] | (uint32_t)in[2 * i + 1] << 8;
        uint32_t lo = i + 1 < n ? (in[2 * i + 2] | (uint32_t)in[2 * i + 3] << 8) : 0;
        if (c >= 0xD800 && c < 0xDC00 && lo >= 0xDC00 && lo < 0xE000) {
            c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
            i++;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | c >> 18);
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return o;
}

/* BOOL WriteConsoleW(HANDLE, const VOID *, DWORD chars, LPDWORD written, LPVOID) */
static void imp_WriteConsoleW(void)
{
    struct fs_file *f = console_arg(W32_ARG(0));
    uint32_t n = W32_ARG(2), done_p = W32_ARG(3), ok = W32_FALSE;

    if (f != NULL) {
        char *utf8 = malloc(3u * (size_t)n + 1u);
        if (utf8 == NULL)
            abort();
        size_t len = utf8_from_utf16(guest_ptr(W32_ARG(1)), n, utf8);
        if (write_all(f->fd, utf8, len) == (ssize_t)len) {
            if (done_p != 0)
                MEM32(done_p) = n;
            ok = W32_TRUE;
        } else {
            w32_set_last_error(w32_from_errno(errno));
        }
        free(utf8);
    }
    W32_RETURN(5, ok);
}

/* UINT GetConsoleCP(void), UINT GetConsoleOutputCP(void) */
static void imp_GetConsoleCP(void) { W32_RETURN(0, 437); }
static void imp_GetConsoleOutputCP(void) { W32_RETURN(0, 437); }

/* BOOL GetConsoleMode(HANDLE, LPDWORD): there is no console. */
static void imp_GetConsoleMode(void)
{
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    W32_RETURN(2, W32_FALSE);
}

/* BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE, BOOL) */
static void imp_SetConsoleCtrlHandler(void) { W32_RETURN(2, W32_TRUE); }

/* ---- Imports: drives, volume, module name ------------------------------ */

/* "C:", "C:\" or "C:/", or NULL for the current directory's drive. */
static int is_c_root(const char *root)
{
    if (root == NULL)
        return 1;
    return tolower((unsigned char)root[0]) == 'c' && root[1] == ':' &&
           (root[2] == 0 || (is_sep(root[2]) && root[3] == 0));
}

/* DWORD GetLogicalDrives(void) */
static void imp_GetLogicalDrives(void) { W32_RETURN(0, 0x4u); }

/* UINT GetDriveTypeA(LPCSTR): DRIVE_FIXED for C:\, else DRIVE_NO_ROOT_DIR. */
static void imp_GetDriveTypeA(void)
{
    W32_RETURN(1, is_c_root(guest_cstr(W32_ARG(0))) ? 3u : 1u);
}

/* Copy s into a guest buffer of size cap, truncating as Wine does. */
static void put_truncated(uint32_t buf, uint32_t cap, const char *s)
{
    if (buf != 0 && cap != 0)
        snprintf((char *)guest_ptr(buf), cap, "%s", s);
}

/* BOOL GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD, LPDWORD serial,
 *                            LPDWORD max_component, LPDWORD flags, LPSTR fs, DWORD) */
static void imp_GetVolumeInformationA(void)
{
    uint32_t ok = W32_FALSE;
    if (is_c_root(guest_cstr(W32_ARG(0)))) {
        put_truncated(W32_ARG(1), W32_ARG(2), "MAC");
        if (W32_ARG(3) != 0)
            MEM32(W32_ARG(3)) = W32_VOLUME_SERIAL;
        if (W32_ARG(4) != 0)
            MEM32(W32_ARG(4)) = 255;
        if (W32_ARG(5) != 0)
            MEM32(W32_ARG(5)) = 0x6u; /* FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK */
        put_truncated(W32_ARG(6), W32_ARG(7), "NTFS");
        ok = W32_TRUE;
    } else {
        w32_set_last_error(W32_ERROR_PATH_NOT_FOUND);
    }
    W32_RETURN(8, ok);
}

/* DWORD GetModuleFileNameA(HMODULE, LPSTR, DWORD size) */
static void imp_GetModuleFileNameA(void)
{
    uint32_t module = W32_ARG(0), buf = W32_ARG(1), size = W32_ARG(2), result;
    uint32_t len = (uint32_t)strlen(W32_EXE_PATH);

    if (module != 0 && module != W32_IMAGE_BASE) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        result = 0;
    } else if (size > len) {
        put_cstr(buf, W32_EXE_PATH);
        result = len;
    } else {
        /* Truncated to size - 1 characters and a NUL; returns size. */
        if (size != 0) {
            memcpy(guest_ptr(buf), W32_EXE_PATH, size - 1u);
            MEM8(buf + size - 1u) = 0;
        }
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        result = size;
    }
    W32_RETURN(3, result);
}

/* ---- Imports: registry ------------------------------------------------- */

/* One key exists, HKLM\<GAME_REG_KEY>, in memory only.
 * Reg* return their error; they do not set the last error. */
#define W32_HKEY_CLASSES_ROOT 0x80000000u
#define W32_HKEY_CURRENT_USER 0x80000001u
#define W32_HKEY_LOCAL_MACHINE 0x80000002u
#define W32_HKEY_USERS 0x80000003u
#define W32_HKEY_PREDEFINED_LAST 0x80000006u
#define W32_REG_SZ 1u
#define W32_REG_DWORD 4u

struct reg_value {
    char *name;
    uint32_t type, size;
    uint8_t *data;
};

static struct reg_value *g_reg;
static size_t g_reg_n;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_reg_once = PTHREAD_ONCE_INIT;

/* Caller holds g_reg_lock. */
static struct reg_value *reg_find(const char *name, int create)
{
    for (size_t i = 0; i < g_reg_n; i++)
        if (strcasecmp(g_reg[i].name, name) == 0)
            return &g_reg[i];
    if (!create)
        return NULL;
    struct reg_value *v = realloc(g_reg, (g_reg_n + 1) * sizeof *v);
    if (v == NULL)
        abort();
    g_reg = v;
    v = &g_reg[g_reg_n++];
    v->name = strdup(name);
    v->type = 0;
    v->size = 0;
    v->data = NULL;
    return v;
}

static void reg_store(const char *name, uint32_t type, const void *data, uint32_t size)
{
    struct reg_value *v = reg_find(name, 1);
    free(v->data);
    v->data = malloc(size != 0 ? size : 1);
    if (v->data == NULL)
        abort();
    memcpy(v->data, data, size);
    v->type = type;
    v->size = size;
}

/* LanguageVal is 1033 (en-US), or HPRECOMP_LANGID as a decimal LANGID. */
static void reg_init(void)
{
    const char *lang = getenv("HPRECOMP_LANGID");
    uint32_t langid = lang != NULL && lang[0] != 0 ? (uint32_t)strtoul(lang, NULL, 10) : 1033u;
    pthread_mutex_lock(&g_reg_lock);
    reg_store("PathVal", W32_REG_SZ, "C:\\Game\\", sizeof "C:\\Game\\");
    reg_store("LanguageVal", W32_REG_DWORD, &langid, 4);
    pthread_mutex_unlock(&g_reg_lock);
}

struct reg_key {
    struct w32_object obj;
};

static void reg_key_destroy(struct w32_object *self) { free(self); }

static const char *hkey_name(uint32_t h, char *buf, size_t cap)
{
    switch (h) {
    case W32_HKEY_CLASSES_ROOT:
        return "HKCR";
    case W32_HKEY_CURRENT_USER:
        return "HKCU";
    case W32_HKEY_LOCAL_MACHINE:
        return "HKLM";
    case W32_HKEY_USERS:
        return "HKU";
    default:
        snprintf(buf, cap, "0x%08X", h);
        return buf;
    }
}

/* LONG RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY) */
static void imp_RegOpenKeyExA(void)
{
    uint32_t root = W32_ARG(0), out = W32_ARG(4), err = W32_ERROR_FILE_NOT_FOUND;
    const char *sub = guest_cstr(W32_ARG(1));

    pthread_once(&g_reg_once, reg_init);
    if (root == W32_HKEY_LOCAL_MACHINE && sub != NULL && strcasecmp(sub, GAME_REG_KEY) == 0) {
        struct reg_key *k = calloc(1, sizeof *k);
        if (k == NULL)
            abort();
        k->obj.kind = W32_KIND_REGKEY;
        k->obj.destroy = reg_key_destroy;
        uint32_t h = w32_handle_new(&k->obj);
        if (h == 0) {
            free(k);
            err = W32_ERROR_NOT_ENOUGH_MEMORY;
        } else {
            MEM32(out) = h;
            err = W32_ERROR_SUCCESS;
        }
    } else if (getenv("RECOMP_STARTUP_VERBOSE") != NULL) {
        char buf[16];
        fprintf(stderr, "RegOpenKeyExA %s\\%s -> not found\n", hkey_name(root, buf, sizeof buf),
                sub != NULL ? sub : "");
    }
    W32_RETURN(5, err);
}

/* LONG RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD type, LPBYTE, LPDWORD size) */
static void imp_RegQueryValueExA(void)
{
    const char *name = guest_cstr(W32_ARG(1));
    uint32_t type_p = W32_ARG(3), data = W32_ARG(4), size_p = W32_ARG(5), err;

    pthread_once(&g_reg_once, reg_init);
    if (w32_handle_get(W32_ARG(0), W32_KIND_REGKEY) == NULL) {
        err = W32_ERROR_INVALID_HANDLE;
    } else if (data != 0 && size_p == 0) {
        err = W32_ERROR_INVALID_PARAMETER;
    } else {
        pthread_mutex_lock(&g_reg_lock);
        struct reg_value *v = reg_find(name != NULL ? name : "", 0);
        if (v == NULL) {
            err = W32_ERROR_FILE_NOT_FOUND;
        } else {
            err = W32_ERROR_SUCCESS;
            if (type_p != 0)
                MEM32(type_p) = v->type;
            if (data != 0 && MEM32(size_p) < v->size)
                err = W32_ERROR_MORE_DATA;
            else if (data != 0)
                memcpy(guest_ptr(data), v->data, v->size);
            if (size_p != 0)
                MEM32(size_p) = v->size;
        }
        pthread_mutex_unlock(&g_reg_lock);
    }
    W32_RETURN(6, err);
}

/* LONG RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD type, const BYTE *, DWORD size).
 * Kept for the rest of the run, never written to disk. */
static void imp_RegSetValueExA(void)
{
    const char *name = guest_cstr(W32_ARG(1));
    uint32_t type = W32_ARG(3), data = W32_ARG(4), size = W32_ARG(5), err = W32_ERROR_SUCCESS;

    pthread_once(&g_reg_once, reg_init);
    if (w32_handle_get(W32_ARG(0), W32_KIND_REGKEY) == NULL) {
        err = W32_ERROR_INVALID_HANDLE;
    } else {
        pthread_mutex_lock(&g_reg_lock);
        reg_store(name != NULL ? name : "", type, size != 0 ? guest_ptr(data) : "", size);
        pthread_mutex_unlock(&g_reg_lock);
    }
    W32_RETURN(6, err);
}

/* LONG RegCloseKey(HKEY) */
static void imp_RegCloseKey(void)
{
    uint32_t h = W32_ARG(0), err = W32_ERROR_SUCCESS;
    if (h >= W32_HKEY_CLASSES_ROOT && h <= W32_HKEY_PREDEFINED_LAST)
        err = W32_ERROR_SUCCESS;
    else if (w32_handle_get(h, W32_KIND_REGKEY) != NULL)
        w32_handle_close(h);
    else
        err = W32_ERROR_INVALID_HANDLE;
    W32_RETURN(1, err);
}

/* ---- Imports: shell ---------------------------------------------------- */

/* HRESULT SHGetFolderPathA(HWND, int csidl, HANDLE, DWORD, LPSTR) */
static void imp_SHGetFolderPathA(void)
{
    uint32_t csidl = W32_ARG(1), out = W32_ARG(4), hr = 0;
    const char *path;
    struct vpath vp;

    switch (csidl & 0xFFu) {
    case 0x05: /* CSIDL_PERSONAL */
        path = "C:\\Users\\Player\\Documents";
        break;
    case 0x1A: /* CSIDL_APPDATA */
        path = "C:\\Users\\Player\\AppData\\Roaming";
        break;
    case 0x1C: /* CSIDL_LOCAL_APPDATA */
        path = "C:\\Users\\Player\\AppData\\Local";
        break;
    case 0x23: /* CSIDL_COMMON_APPDATA */
        path = "C:\\ProgramData";
        break;
    default:
        path = NULL;
        break;
    }
    if (path == NULL) {
        THUNK_UNSUPPORTED("CSIDL 0x%X", csidl);
        MEM8(out) = 0;
        hr = 0x80004005u; /* E_FAIL */
    } else {
        if (csidl & 0x8000u) { /* CSIDL_FLAG_CREATE */
            uint32_t err = W32_ERROR_SUCCESS;
            parse_path(path, &vp);
            if (vp.n == 0) {
                char base[FS_HOST_MAX];
                root_base(vp.root, 1, base);
                if (w32_fs_mkdir_p(base) != 0)
                    err = w32_from_errno(errno);
            } else {
                err = make_dirs(&vp);
            }
            if (err != W32_ERROR_SUCCESS && err != W32_ERROR_ALREADY_EXISTS)
                hr = 0x80070000u | err; /* HRESULT_FROM_WIN32 */
        }
        put_cstr(out, hr == 0 ? path : "");
    }
    W32_RETURN(5, hr);
}

/* HINSTANCE ShellExecuteA(HWND, LPCSTR verb, LPCSTR file, LPCSTR, LPCSTR, INT) */
static void imp_ShellExecuteA(void)
{
    const char *verb = guest_cstr(W32_ARG(1)), *file = guest_cstr(W32_ARG(2));
    THUNK_UNSUPPORTED("verb %s file %s", verb != NULL ? verb : "(default)",
                      file != NULL ? file : "(null)");
    W32_RETURN(6, 31u); /* SE_ERR_NOASSOC */
}

/* BOOL GetOpenFileNameA(LPOPENFILENAMEA) */
static void imp_GetOpenFileNameA(void) { W32_RETURN(1, W32_FALSE); }

/* ---- Export table ------------------------------------------------------ */

const win32_export_t win32_exports_fs[] = {
    {"ADVAPI32.dll", "RegCloseKey", 0, imp_RegCloseKey},
    {"ADVAPI32.dll", "RegOpenKeyExA", 0, imp_RegOpenKeyExA},
    {"ADVAPI32.dll", "RegQueryValueExA", 0, imp_RegQueryValueExA},
    {"ADVAPI32.dll", "RegSetValueExA", 0, imp_RegSetValueExA},
    {"KERNEL32.dll", "CreateDirectoryA", 0, imp_CreateDirectoryA},
    {"KERNEL32.dll", "CreateFileA", 0, imp_CreateFileA},
    {"KERNEL32.dll", "DeleteFileA", 0, imp_DeleteFileA},
    {"KERNEL32.dll", "FindClose", 0, imp_FindClose},
    {"KERNEL32.dll", "FindFirstFileA", 0, imp_FindFirstFileA},
    {"KERNEL32.dll", "FindNextFileA", 0, imp_FindNextFileA},
    {"KERNEL32.dll", "FlushFileBuffers", 0, imp_FlushFileBuffers},
    {"KERNEL32.dll", "GetConsoleCP", 0, imp_GetConsoleCP},
    {"KERNEL32.dll", "GetConsoleMode", 0, imp_GetConsoleMode},
    {"KERNEL32.dll", "GetConsoleOutputCP", 0, imp_GetConsoleOutputCP},
    {"KERNEL32.dll", "GetDriveTypeA", 0, imp_GetDriveTypeA},
    {"KERNEL32.dll", "GetFileAttributesA", 0, imp_GetFileAttributesA},
    {"KERNEL32.dll", "GetFileAttributesExA", 0, imp_GetFileAttributesExA},
    {"KERNEL32.dll", "GetFileInformationByHandle", 0, imp_GetFileInformationByHandle},
    {"KERNEL32.dll", "GetFileSize", 0, imp_GetFileSize},
    {"KERNEL32.dll", "GetFileSizeEx", 0, imp_GetFileSizeEx},
    {"KERNEL32.dll", "GetFileType", 0, imp_GetFileType},
    {"KERNEL32.dll", "GetLogicalDrives", 0, imp_GetLogicalDrives},
    {"KERNEL32.dll", "GetModuleFileNameA", 0, imp_GetModuleFileNameA},
    {"KERNEL32.dll", "GetStdHandle", 0, imp_GetStdHandle},
    {"KERNEL32.dll", "GetTempFileNameA", 0, imp_GetTempFileNameA},
    {"KERNEL32.dll", "GetVolumeInformationA", 0, imp_GetVolumeInformationA},
    {"KERNEL32.dll", "MoveFileA", 0, imp_MoveFileA},
    {"KERNEL32.dll", "ReadFile", 0, imp_ReadFile},
    {"KERNEL32.dll", "SetConsoleCtrlHandler", 0, imp_SetConsoleCtrlHandler},
    {"KERNEL32.dll", "SetEndOfFile", 0, imp_SetEndOfFile},
    {"KERNEL32.dll", "SetFilePointer", 0, imp_SetFilePointer},
    {"KERNEL32.dll", "SetFilePointerEx", 0, imp_SetFilePointerEx},
    {"KERNEL32.dll", "SetFileTime", 0, imp_SetFileTime},
    {"KERNEL32.dll", "SetHandleCount", 0, imp_SetHandleCount},
    {"KERNEL32.dll", "SetStdHandle", 0, imp_SetStdHandle},
    {"KERNEL32.dll", "WriteConsoleA", 0, imp_WriteConsoleA},
    {"KERNEL32.dll", "WriteConsoleW", 0, imp_WriteConsoleW},
    {"KERNEL32.dll", "WriteFile", 0, imp_WriteFile},
    {"SHELL32.dll", "SHCreateDirectoryExA", 0, imp_SHCreateDirectoryExA},
    {"SHELL32.dll", "SHGetFolderPathA", 0, imp_SHGetFolderPathA},
    {"SHELL32.dll", "ShellExecuteA", 0, imp_ShellExecuteA},
    {"comdlg32.dll", "GetOpenFileNameA", 0, imp_GetOpenFileNameA},
    {0},
};
