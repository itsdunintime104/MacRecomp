/* Win32 fs slice test: all 41 imports of runtime/src/win32_fs.c. Every call
 * checks the stdcall pop (g_esp + 4 + 4 * nargs) and that ebx/esi/edi/ebp
 * hold. The game dir and save dir are fresh folders next to this binary; the
 * game dir is made read-only for the run, so any write into it fails.
 * Guest stack as in the engine's THUNK_SLICES.md. Links win32_fs.c,
 * guest_mem.c and recomp_glue.c, not win32_core.c: the core functions the
 * slice calls are doubles below. */
#define _DARWIN_C_SOURCE
#include <ftw.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "win32_thunk.h"
#include "win32_fs.h"

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

/* ---- Core doubles ------------------------------------------------------ */

static struct w32_object *g_objs[256];
static uint32_t g_err;

uint32_t w32_last_error(void) { return g_err; }
void w32_set_last_error(uint32_t code) { g_err = code; }

uint32_t w32_handle_new(struct w32_object *obj)
{
    for (uint32_t i = 1; i < 256; i++) {
        if (g_objs[i] == NULL) {
            g_objs[i] = obj;
            obj->refs = 1;
            return W32_HANDLE_BASE + 4u * i;
        }
    }
    return 0;
}

static int handle_slot(uint32_t h)
{
    uint32_t off = h - W32_HANDLE_BASE;
    return h >= W32_HANDLE_BASE && off % 4u == 0 && off / 4u < 256 ? (int)(off / 4u) : -1;
}

struct w32_object *w32_handle_get(uint32_t h, enum w32_kind kind)
{
    int i = handle_slot(h);
    struct w32_object *o = i > 0 ? g_objs[i] : NULL;
    return o != NULL && (kind == W32_KIND_ANY || o->kind == kind) ? o : NULL;
}

uint32_t w32_handle_close(uint32_t h)
{
    struct w32_object *o = w32_handle_get(h, W32_KIND_ANY);
    if (o == NULL) {
        g_err = W32_ERROR_INVALID_HANDLE;
        return W32_FALSE;
    }
    g_objs[handle_slot(h)] = NULL;
    if (--o->refs == 0)
        o->destroy(o);
    return W32_TRUE;
}

/* ---- Guest stack ------------------------------------------------------- */

#define GUEST_LO 0x00F00000u
#define GUEST_SIZE 0x00100000u
#define SCRATCH 0x00F10000u
#define STACK_TOP 0x00FE0000u
#define RET_ADDR 0xDEAD0001u

static uint32_t g_next = SCRATCH;

static uint32_t galloc(uint32_t n)
{
    uint32_t a = g_next;
    memset(guest_ptr(a), 0, n);
    g_next += (n + 15u) & ~15u;
    return a;
}

static uint32_t gstr(const char *s)
{
    uint32_t a = galloc((uint32_t)strlen(s) + 1u);
    strcpy((char *)guest_ptr(a), s);
    return a;
}

static uint32_t rd32(uint32_t a) { return MEM32(a); }
static void wr32(uint32_t a, uint32_t v) { MEM32(a) = v; }
static const char *gs(uint32_t a) { return (const char *)guest_ptr(a); }

static int g_called[64];

/* Call the slice's export `name` with nargs arguments the way lifted code
 * does: arguments at esp+4, the return address at esp. */
static uint32_t call(const char *name, int nargs, ...)
{
    recomp_func_t fn = NULL;
    va_list ap;

    for (int i = 0; win32_exports_fs[i].dll != NULL; i++) {
        if (strcmp(win32_exports_fs[i].name, name) == 0) {
            fn = win32_exports_fs[i].fn;
            g_called[i] = 1;
        }
    }
    if (fn == NULL) {
        fprintf(stderr, "FAIL: %s is not in win32_exports_fs\n", name);
        g_fail = 1;
        return 0;
    }
    g_esp = STACK_TOP;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        wr32(STACK_TOP + 4u + 4u * (uint32_t)i, va_arg(ap, uint32_t));
    va_end(ap);
    wr32(STACK_TOP, RET_ADDR);
    g_ebx = 0x11111111u;
    g_esi = 0x22222222u;
    g_edi = 0x33333333u;
    g_ebp = 0x44444444u;
    fn();
    if (g_esp != STACK_TOP + 4u + 4u * (uint32_t)nargs) {
        fprintf(stderr, "FAIL: %s popped %d bytes, expected %d\n", name,
                (int)(g_esp - STACK_TOP), 4 + 4 * nargs);
        g_fail = 1;
    }
    CHECK(g_ebx == 0x11111111u && g_esi == 0x22222222u);
    CHECK(g_edi == 0x33333333u && g_ebp == 0x44444444u);
    return g_eax;
}

/* ---- Host folders ------------------------------------------------------ */

static char g_root[1024], g_game[1100], g_save[1100];

static void host_write(const char *dir, const char *rel, const char *text)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

/* Contents of dir/rel, or "" when missing. */
static const char *host_read(const char *dir, const char *rel)
{
    static char buf[256];
    char path[1300];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (f != NULL) {
        buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
        fclose(f);
    }
    return buf;
}

static int host_kind(const char *dir, const char *rel) /* 0 none, 1 file, 2 folder */
{
    char path[1300];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

static int chmod_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)flag;
    (void)ftw;
    chmod(path, S_ISDIR(st->st_mode) ? 0755 : 0644);
    return 0;
}

static int lock_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)flag;
    (void)ftw;
    chmod(path, S_ISDIR(st->st_mode) ? 0555 : 0444);
    return 0;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static int g_game_entries;
static int count_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)path;
    (void)st;
    (void)flag;
    (void)ftw;
    g_game_entries++;
    return 0;
}

static void make_folders(const char *argv0)
{
    char exe[1024];
    snprintf(exe, sizeof exe, "%s", argv0);
    snprintf(g_root, sizeof g_root, "%s/test_win32_fs.XXXXXX", dirname(exe));
    if (mkdtemp(g_root) == NULL) {
        perror(g_root);
        exit(1);
    }
    snprintf(g_game, sizeof g_game, "%s/game", g_root);
    snprintf(g_save, sizeof g_save, "%s/save", g_root);
    char data[1200];
    snprintf(data, sizeof data, "%s/DATA", g_game);
    if (mkdir(g_game, 0755) != 0 || mkdir(g_save, 0755) != 0 || mkdir(data, 0755) != 0) {
        perror("mkdir");
        exit(1);
    }
    host_write(g_game, "GAME.DAT", "shipped data");
    host_write(g_game, "CONFIG.TXT", "old config");
    host_write(g_game, "README.TXT", "readme");
    host_write(g_game, "DATA/LEVEL1.TXT", "level one");
    nftw(g_game, lock_entry, 16, FTW_PHYS);
    w32_fs_set_game_dir(g_game);
    w32_fs_set_save_dir(g_save);
}

/* ---- Tests ------------------------------------------------------------- */

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define DELETE_ON_CLOSE 0x04000000u
#define INVALID 0xFFFFFFFFu

static uint32_t open_file(const char *path, uint32_t access, uint32_t disp, uint32_t flags)
{
    return call("CreateFileA", 7, gstr(path), access, 1u, 0u, disp, flags | 0x80u, 0u);
}

/* Read up to n bytes from h into a fresh guest buffer; returns it. */
static const char *read_text(uint32_t h, uint32_t n)
{
    uint32_t buf = galloc(n + 1u), done = galloc(4);
    CHECK(call("ReadFile", 5, h, buf, n, done, 0u) == W32_TRUE);
    MEM8(buf + rd32(done)) = 0;
    return gs(buf);
}

static uint32_t write_text(uint32_t h, const char *s)
{
    uint32_t done = galloc(4);
    uint32_t ok = call("WriteFile", 5, h, gstr(s), (uint32_t)strlen(s), done, 0u);
    return ok == W32_TRUE ? rd32(done) : INVALID;
}

static void expect_open_error(const char *path, uint32_t err)
{
    g_err = 0;
    uint32_t h = open_file(path, GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h == INVALID);
    if (g_err != err)
        fprintf(stderr, "FAIL: CreateFileA %s: error %u, expected %u\n", path, g_err, err);
    CHECK(g_err == err);
}

static void test_paths(void)
{
    /* Case-insensitive open of the shipped GAME.DAT. */
    uint32_t h = open_file("c:\\game\\game.dat", GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h != INVALID && w32_handle_get(h, W32_KIND_FILE) != NULL);
    CHECK(g_err == 0);
    CHECK(strcmp(read_text(h, 64), "shipped data") == 0);
    uint32_t hi = galloc(4), li = galloc(8), info = galloc(52);
    wr32(hi, 0x55u);
    CHECK(call("GetFileSize", 2, h, hi) == 12 && rd32(hi) == 0);
    CHECK(call("GetFileSizeEx", 2, h, li) == W32_TRUE && rd32(li) == 12 && rd32(li + 4) == 0);
    CHECK(call("GetFileType", 1, h) == 1);
    CHECK(call("GetFileInformationByHandle", 2, h, info) == W32_TRUE);
    CHECK(rd32(info) == 0x01u);           /* READONLY: shipped, not overlaid */
    CHECK(rd32(info + 28) == 0x4C485052u); /* volume serial */
    CHECK(rd32(info + 36) == 12);
    CHECK(write_text(h, "x") == INVALID && g_err == W32_ERROR_ACCESS_DENIED);
    uint32_t ft = galloc(8);
    CHECK(call("SetFileTime", 4, h, 0u, 0u, ft) == W32_FALSE && g_err == W32_ERROR_ACCESS_DENIED);
    CHECK(w32_handle_close(h) == W32_TRUE);

    /* Forward slashes, relative paths, `.` and `..` inside the root. */
    h = open_file("C:/GAME/data/level1.txt", GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h != INVALID && strcmp(read_text(h, 64), "level one") == 0);
    w32_handle_close(h);
    h = open_file("DATA\\.\\..\\Game.dat", GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h != INVALID);
    w32_handle_close(h);
    h = open_file("\\Game\\Data\\Level1.txt", GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h != INVALID);
    w32_handle_close(h);

    /* `..` cannot leave the mapped roots. */
    expect_open_error("C:\\Game\\..\\..\\etc\\passwd", W32_ERROR_ACCESS_DENIED);
    expect_open_error("..\\..\\..\\etc\\passwd", W32_ERROR_ACCESS_DENIED);
    expect_open_error("..\\secret.txt", W32_ERROR_ACCESS_DENIED);
    expect_open_error("C:\\Game\\..\\Windows\\win.ini", W32_ERROR_ACCESS_DENIED);
    /* Other drives, UNC and device paths, unmapped folders, missing files. */
    expect_open_error("D:\\GAME.DAT", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("\\\\server\\share\\GAME.DAT", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("\\\\.\\PhysicalDrive0", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("C:\\Windows\\win.ini", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("C:\\Game\\nope.dat", W32_ERROR_FILE_NOT_FOUND);
    expect_open_error("C:\\Game\\nodir\\nope.dat", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("C:\\Game\\GAME.DAT\\x", W32_ERROR_PATH_NOT_FOUND);
    expect_open_error("C:\\Game\\DATA", W32_ERROR_ACCESS_DENIED); /* a folder */

    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game\\DATA\\")) == 0x10u);
    CHECK(call("GetFileAttributesA", 1, gstr("c:\\")) == 0x10u);
    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game")) == 0x10u);
    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game\\readme.txt")) == 0x01u);
    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game\\none.txt")) == INVALID);
    CHECK(g_err == W32_ERROR_FILE_NOT_FOUND);

    /* GetFileAttributesExA: WIN32_FILE_ATTRIBUTE_DATA for the shipped
     * GAME.DAT (12 bytes), its host mtime, and FILE_NOT_FOUND for none.txt. */
    uint32_t fad = galloc(40);
    memset(guest_ptr(fad), 0xAB, 40);
    CHECK(call("GetFileAttributesExA", 3, gstr("C:\\Game\\game.dat"), 0u, fad) == W32_TRUE);
    CHECK(rd32(fad) == 0x01u && rd32(fad + 28) == 0 && rd32(fad + 32) == 12);
    CHECK(rd32(fad + 36) == 0xABABABABu); /* 36 bytes written, no more */
    struct stat hst;
    char hpath[1024];
    snprintf(hpath, sizeof hpath, "%s/GAME.DAT", g_game);
    CHECK(stat(hpath, &hst) == 0);
    uint64_t want_ft = (uint64_t)((int64_t)hst.st_mtimespec.tv_sec * 10000000 +
                                  hst.st_mtimespec.tv_nsec / 100 + 116444736000000000ll);
    CHECK(rd32(fad + 20) == (uint32_t)want_ft && rd32(fad + 24) == (uint32_t)(want_ft >> 32));
    CHECK(call("GetFileAttributesExA", 3, gstr("C:\\Game\\DATA"), 0u, fad) == W32_TRUE);
    CHECK(rd32(fad) == 0x10u);
    g_err = 0;
    CHECK(call("GetFileAttributesExA", 3, gstr("C:\\Game\\none.txt"), 0u, fad) == W32_FALSE);
    CHECK(g_err == W32_ERROR_FILE_NOT_FOUND);
    g_err = 0;
    CHECK(call("GetFileAttributesExA", 3, gstr("C:\\Game\\game.dat"), 1u, fad) == W32_FALSE);
    CHECK(g_err == W32_ERROR_INVALID_PARAMETER);
}

static void test_writes(void)
{
    /* A write to C:\Game\ lands in the save dir, never in the game dir. */
    uint32_t h = open_file("C:\\Game\\x.txt", GENERIC_WRITE, CREATE_ALWAYS, 0);
    CHECK(h != INVALID && g_err == 0);
    CHECK(write_text(h, "hello world") == 11);
    CHECK(call("ReadFile", 5, h, galloc(4), 4u, galloc(4), 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ACCESS_DENIED);
    w32_handle_close(h);
    CHECK(strcmp(host_read(g_save, "Game/x.txt"), "hello world") == 0);
    CHECK(host_kind(g_game, "x.txt") == 0);
    h = open_file("C:\\Game\\X.TXT", GENERIC_WRITE, CREATE_ALWAYS, 0);
    CHECK(h != INVALID && g_err == W32_ERROR_ALREADY_EXISTS);
    CHECK(write_text(h, "hello world") == 11);
    w32_handle_close(h);
    CHECK(open_file("C:\\Game\\x.txt", GENERIC_WRITE, CREATE_NEW, 0) == INVALID);
    CHECK(g_err == 80); /* ERROR_FILE_EXISTS */

    /* Overlay: a rewritten shipped file shadows it; the shipped copy stays. */
    h = open_file("C:\\Game\\config.txt", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0);
    CHECK(h != INVALID);
    CHECK(strcmp(read_text(h, 3), "old") == 0);
    CHECK(call("SetFilePointer", 4, h, 0u, 0u, 0u) == 0);
    CHECK(write_text(h, "new") == 3);
    w32_handle_close(h);
    CHECK(strcmp(host_read(g_save, "Game/CONFIG.TXT"), "new config") == 0);
    CHECK(strcmp(host_read(g_game, "CONFIG.TXT"), "old config") == 0);
    h = open_file("C:\\GAME\\Config.Txt", GENERIC_READ, OPEN_EXISTING, 0);
    CHECK(h != INVALID && strcmp(read_text(h, 64), "new config") == 0);
    w32_handle_close(h);
    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game\\CONFIG.TXT")) == 0x80u);

    /* A new file in a shipped folder: the folder is mirrored with its case. */
    h = open_file("C:\\Game\\data\\new.bin", GENERIC_WRITE, CREATE_NEW, 0);
    CHECK(h != INVALID);
    w32_handle_close(h);
    CHECK(host_kind(g_save, "Game/DATA/new.bin") == 1);
    CHECK(host_kind(g_game, "DATA/new.bin") == 0);

    /* FILE_FLAG_DELETE_ON_CLOSE removes the file; a shipped file refuses it. */
    h = open_file("C:\\Game\\tmp.del", GENERIC_READ | GENERIC_WRITE, CREATE_NEW, DELETE_ON_CLOSE);
    CHECK(h != INVALID && host_kind(g_save, "Game/tmp.del") == 1);
    w32_handle_close(h);
    CHECK(host_kind(g_save, "Game/tmp.del") == 0);
    CHECK(open_file("C:\\Game\\GAME.DAT", GENERIC_READ, OPEN_EXISTING, DELETE_ON_CLOSE) == INVALID);
    CHECK(g_err == W32_ERROR_ACCESS_DENIED);
}

static void test_seek(void)
{
    uint32_t h = open_file("C:\\Game\\x.txt", GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0);
    uint32_t hi = galloc(4), np = galloc(8);
    CHECK(h != INVALID);

    CHECK(call("SetFilePointer", 4, h, 0u, 0u, 2u) == 11);           /* FILE_END */
    CHECK(call("SetFilePointer", 4, h, (uint32_t)-5, 0u, 1u) == 6);  /* FILE_CURRENT */
    CHECK(strcmp(read_text(h, 5), "world") == 0);
    /* No high part: the distance is signed 32-bit, so 0xFFFFFFFF is -1. */
    CHECK(call("SetFilePointer", 4, h, 0xFFFFFFFFu, 0u, 0u) == INVALID);
    CHECK(g_err == 131); /* ERROR_NEGATIVE_SEEK */
    CHECK(call("SetFilePointer", 4, h, 0u, 0u, 1u) == 11);           /* unchanged */
    /* With a high part: 64-bit positions. */
    wr32(hi, 1);
    CHECK(call("SetFilePointer", 4, h, 0u, hi, 0u) == 0 && rd32(hi) == 1);
    wr32(hi, 0);
    CHECK(call("SetFilePointer", 4, h, 0u, hi, 1u) == 0 && rd32(hi) == 1);
    /* No high part and a position above 4 GB fails. */
    CHECK(call("SetFilePointer", 4, h, 0u, 0u, 1u) == INVALID && g_err == W32_ERROR_INVALID_PARAMETER);
    /* Position 0xFFFFFFFF is a success that GetLastError tells apart. */
    wr32(hi, 0);
    g_err = 99;
    CHECK(call("SetFilePointer", 4, h, 0xFFFFFFFFu, hi, 0u) == INVALID && rd32(hi) == 0 && g_err == 0);
    wr32(hi, 0xFFFFFFFFu);
    CHECK(call("SetFilePointer", 4, h, 0xFFFFFFFEu, hi, 2u) == 9 && rd32(hi) == 0); /* END - 2 */
    CHECK(call("SetFilePointer", 4, h, 0u, 0u, 7u) == INVALID && g_err == W32_ERROR_INVALID_PARAMETER);

    CHECK(call("SetFilePointerEx", 5, h, 5u, 1u, np, 0u) == W32_TRUE);
    CHECK(rd32(np) == 5 && rd32(np + 4) == 1);
    /* A size above 4 GB (sparse) through the high part, then back to 5. */
    CHECK(call("SetFilePointerEx", 5, h, 2u, 1u, 0u, 0u) == W32_TRUE);
    CHECK(call("SetEndOfFile", 1, h) == W32_TRUE);
    wr32(hi, 0);
    CHECK(call("GetFileSize", 2, h, hi) == 2 && rd32(hi) == 1);
    CHECK(call("SetFilePointerEx", 5, h, 5u, 0u, 0u, 0u) == W32_TRUE);
    CHECK(call("SetEndOfFile", 1, h) == W32_TRUE);
    CHECK(call("GetFileSize", 2, h, 0u) == 5);
    CHECK(call("FlushFileBuffers", 1, h) == W32_TRUE);

    /* SetFileTime: 2020-01-01T00:00:00Z as a FILETIME. */
    uint32_t ft = galloc(8);
    uint64_t t = 132223104000000000ull;
    wr32(ft, (uint32_t)t);
    wr32(ft + 4, (uint32_t)(t >> 32));
    CHECK(call("SetFileTime", 4, h, 0u, 0u, ft) == W32_TRUE);
    w32_handle_close(h);
    char path[1300];
    struct stat st;
    snprintf(path, sizeof path, "%s/Game/x.txt", g_save);
    CHECK(stat(path, &st) == 0 && st.st_mtime == 1577836800 && st.st_size == 5);

    CHECK(call("ReadFile", 5, 0x1234u, galloc(4), 4u, 0u, 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_INVALID_HANDLE);
}

/* Names FindFirstFileA/FindNextFileA return for spec, joined by '|' in the
 * order given; *attrs gets the attributes of the entry named attr_of. */
static int find_all(const char *spec, char *names, size_t cap, const char *attr_of, uint32_t *attrs)
{
    uint32_t data = galloc(320);
    int n = 0;
    names[0] = 0;
    uint32_t h = call("FindFirstFileA", 2, gstr(spec), data);
    if (h == INVALID)
        return -1;
    do {
        const char *name = gs(data + 44);
        if (attr_of != NULL && strcmp(name, attr_of) == 0)
            *attrs = rd32(data);
        strlcat(names, "|", cap);
        strlcat(names, name, cap);
        n++;
    } while (call("FindNextFileA", 2, h, data) == W32_TRUE);
    CHECK(g_err == W32_ERROR_NO_MORE_FILES);
    CHECK(call("FindClose", 1, h) == W32_TRUE);
    CHECK(call("FindClose", 1, h) == W32_FALSE && g_err == W32_ERROR_INVALID_HANDLE);
    strlcat(names, "|", cap);
    return n;
}

static void test_find(void)
{
    char names[512];
    uint32_t attrs = 0;

    /* Merged overlay listing: CONFIG.TXT is in both folders, listed once. */
    CHECK(find_all("C:\\Game\\*.txt", names, sizeof names, "README.TXT", &attrs) == 3);
    CHECK(strstr(names, "|x.txt|") && strstr(names, "|CONFIG.TXT|") && strstr(names, "|README.TXT|"));
    CHECK(attrs == 0x01u);
    CHECK(find_all("c:\\game\\*.*", names, sizeof names, "DATA", &attrs) == 5);
    CHECK(attrs == 0x10u);
    CHECK(strstr(names, "|GAME.DAT|") && strstr(names, "|DATA|"));
    CHECK(find_all("C:\\Game\\g?me*", names, sizeof names, "GAME.DAT", &attrs) == 1);
    CHECK(strcmp(names, "|GAME.DAT|") == 0);
    CHECK(find_all("C:\\Game\\DATA\\*", names, sizeof names, NULL, NULL) == 2);
    CHECK(find_all("C:\\Game\\*.none", names, sizeof names, NULL, NULL) == -1);
    CHECK(g_err == W32_ERROR_FILE_NOT_FOUND);
    CHECK(find_all("C:\\Game\\nodir\\*", names, sizeof names, NULL, NULL) == -1);
    CHECK(g_err == W32_ERROR_PATH_NOT_FOUND);
}

static void test_dirs(void)
{
    CHECK(call("CreateDirectoryA", 2, gstr("C:\\Game\\SAVES"), 0u) == W32_TRUE);
    CHECK(host_kind(g_save, "Game/SAVES") == 2 && host_kind(g_game, "SAVES") == 0);
    CHECK(call("CreateDirectoryA", 2, gstr("c:\\game\\saves"), 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ALREADY_EXISTS);
    CHECK(call("CreateDirectoryA", 2, gstr("C:\\Game\\DATA"), 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ALREADY_EXISTS);
    CHECK(call("CreateDirectoryA", 2, gstr("C:\\Game\\a\\b"), 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_PATH_NOT_FOUND);

    CHECK(call("SHCreateDirectoryExA", 3, 0u, gstr("C:\\Users\\Player\\AppData\\Roaming\\WB\\HP"), 0u) == 0);
    CHECK(host_kind(g_save, "Users/Player/AppData/Roaming/WB/HP") == 2);
    CHECK(call("SHCreateDirectoryExA", 3, 0u, gstr("C:\\Users\\Player\\AppData\\Roaming\\WB\\HP\\"), 0u) == 183);

    /* The game's profile folder: its CP1252 bytes (0xAE, 0x99) are UTF-8 on
     * the host, which APFS needs, and CP1252 again in a listing. */
    const char *prof = "C:\\Users\\Player\\AppData\\Roaming\\WB Games\\LEGO\xae Harry Potter\x99";
    char names[256];
    CHECK(call("SHCreateDirectoryExA", 3, 0u, gstr(prof), 0u) == 0);
    CHECK(host_kind(g_save, "Users/Player/AppData/Roaming/WB Games/LEGO\xc2\xae Harry Potter\xe2\x84\xa2") == 2);
    CHECK(call("SHCreateDirectoryExA", 3, 0u, gstr(prof), 0u) == 183);
    CHECK(call("GetFileAttributesA", 1, gstr(prof)) == 0x10u);
    CHECK(find_all("C:\\Users\\Player\\AppData\\Roaming\\WB Games\\*", names, sizeof names, NULL, NULL) == 1);
    CHECK(strcmp(names, "|LEGO\xae Harry Potter\x99|") == 0);

    CHECK(call("MoveFileA", 2, gstr("C:\\Game\\x.txt"), gstr("C:\\Game\\Saves\\x2.txt")) == W32_TRUE);
    CHECK(host_kind(g_save, "Game/SAVES/x2.txt") == 1 && host_kind(g_save, "Game/x.txt") == 0);
    CHECK(call("MoveFileA", 2, gstr("C:\\Game\\GAME.DAT"), gstr("C:\\Game\\g.dat")) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ACCESS_DENIED);
    CHECK(call("MoveFileA", 2, gstr("C:\\Game\\SAVES\\x2.txt"), gstr("C:\\Game\\README.TXT")) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ALREADY_EXISTS);

    CHECK(call("DeleteFileA", 1, gstr("C:\\Game\\SAVES\\X2.TXT")) == W32_TRUE);
    CHECK(host_kind(g_save, "Game/SAVES/x2.txt") == 0);
    CHECK(call("DeleteFileA", 1, gstr("C:\\Game\\GAME.DAT")) == W32_FALSE);
    CHECK(g_err == W32_ERROR_ACCESS_DENIED);
    /* Deleting the overlay copy shows the shipped file again. */
    CHECK(call("DeleteFileA", 1, gstr("C:\\Game\\config.txt")) == W32_TRUE);
    CHECK(call("GetFileAttributesA", 1, gstr("C:\\Game\\config.txt")) == 0x01u);
    CHECK(call("DeleteFileA", 1, gstr("C:\\Game\\none")) == W32_FALSE);
    CHECK(g_err == W32_ERROR_FILE_NOT_FOUND);

    uint32_t buf = galloc(260);
    uint32_t u = call("GetTempFileNameA", 4, gstr("C:\\Users\\Player\\AppData\\Roaming\\WB"), gstr("HPRX"), 0u, buf);
    CHECK(u != 0);
    CHECK(strncmp(gs(buf), "C:\\Users\\Player\\AppData\\Roaming\\WB\\HPR", 38) == 0);
    CHECK(strstr(gs(buf), ".TMP") != NULL);
    CHECK(call("GetFileAttributesA", 1, gstr(gs(buf))) == 0x80u);
    CHECK(call("GetTempFileNameA", 4, gstr("C:\\Game\\"), gstr("ab"), 0x1234u, buf) == 0x1234u);
    CHECK(strcmp(gs(buf), "C:\\Game\\ab1234.TMP") == 0);
    CHECK(call("GetFileAttributesA", 1, gstr(gs(buf))) == INVALID); /* not created */
    CHECK(call("GetTempFileNameA", 4, 0u, gstr("t"), 0u, buf) != 0);
    CHECK(strncmp(gs(buf), "C:\\Users\\Player\\AppData\\Local\\Temp\\t", 36) == 0);
}

static void test_registry(void)
{
    uint32_t hk = galloc(4), type = galloc(4), size = galloc(4), data = galloc(64);

    CHECK(call("RegOpenKeyExA", 5, 0x80000002u, gstr("software\\recomp\\TESTGAME"), 0u, 0x20019u, hk) == 0);
    uint32_t key = rd32(hk);
    CHECK(w32_handle_get(key, W32_KIND_REGKEY) != NULL);

    /* Size query, ERROR_MORE_DATA, then the data. */
    wr32(size, 0);
    CHECK(call("RegQueryValueExA", 6, key, gstr("PathVal"), 0u, type, 0u, size) == 0);
    CHECK(rd32(type) == 1 && rd32(size) == 9);
    wr32(size, 4);
    CHECK(call("RegQueryValueExA", 6, key, gstr("PathVal"), 0u, type, data, size) == 234);
    CHECK(rd32(size) == 9);
    wr32(size, 64);
    CHECK(call("RegQueryValueExA", 6, key, gstr("pathval"), 0u, type, data, size) == 0);
    CHECK(rd32(size) == 9 && strcmp(gs(data), "C:\\Game\\") == 0);
    wr32(size, 4);
    CHECK(call("RegQueryValueExA", 6, key, gstr("LanguageVal"), 0u, type, data, size) == 0);
    CHECK(rd32(type) == 4 && rd32(size) == 4 && rd32(data) == 1033);
    CHECK(call("RegQueryValueExA", 6, key, gstr("Missing"), 0u, type, data, size) == 2);

    wr32(data, 7);
    CHECK(call("RegSetValueExA", 6, key, gstr("NewVal"), 0u, 4u, data, 4u) == 0);
    wr32(data, 0);
    wr32(size, 4);
    CHECK(call("RegQueryValueExA", 6, key, gstr("NewVal"), 0u, type, data, size) == 0 && rd32(data) == 7);

    CHECK(call("RegCloseKey", 1, key) == 0);
    CHECK(call("RegCloseKey", 1, key) == W32_ERROR_INVALID_HANDLE);
    CHECK(call("RegQueryValueExA", 6, key, gstr("PathVal"), 0u, type, data, size) == W32_ERROR_INVALID_HANDLE);
    CHECK(call("RegOpenKeyExA", 5, 0x80000001u, gstr("Software\\Recomp\\TestGame"), 0u, 0x20019u, hk) == 2);
    CHECK(call("RegOpenKeyExA", 5, 0x80000002u, gstr("SOFTWARE\\Microsoft\\DirectX"), 0u, 0x20019u, hk) == 2);
}

static void test_shell(void)
{
    uint32_t buf = galloc(260);

    CHECK(host_kind(g_save, "Users/Player/Documents") == 0);
    CHECK(call("SHGetFolderPathA", 5, 0u, 0x8005u, 0u, 0u, buf) == 0);
    CHECK(strcmp(gs(buf), "C:\\Users\\Player\\Documents") == 0);
    CHECK(host_kind(g_save, "Users/Player/Documents") == 2);
    CHECK(call("SHGetFolderPathA", 5, 0u, 0x1Au, 0u, 0u, buf) == 0);
    CHECK(strcmp(gs(buf), "C:\\Users\\Player\\AppData\\Roaming") == 0);
    CHECK(call("SHGetFolderPathA", 5, 0u, 0x1Cu, 0u, 0u, buf) == 0);
    CHECK(strcmp(gs(buf), "C:\\Users\\Player\\AppData\\Local") == 0);
    CHECK(call("SHGetFolderPathA", 5, 0u, 0x8023u, 0u, 0u, buf) == 0);
    CHECK(strcmp(gs(buf), "C:\\ProgramData") == 0 && host_kind(g_save, "ProgramData") == 2);
    CHECK(call("SHGetFolderPathA", 5, 0u, 0x26u, 0u, 0u, buf) == 0x80004005u);

    CHECK(call("ShellExecuteA", 6, 0u, gstr("open"), gstr("http://example.invalid/"), 0u, 0u, 1u) == 31);
    CHECK(call("GetOpenFileNameA", 1, galloc(76)) == W32_FALSE);
}

static void test_module(void)
{
    uint32_t buf = galloc(260);

    CHECK(call("GetModuleFileNameA", 3, 0u, buf, 260u) == 22);
    CHECK(strcmp(gs(buf), "C:\\Game\\RecompTest.exe") == 0);
    g_err = 0;
    CHECK(call("GetModuleFileNameA", 3, 0x400000u, buf, 10u) == 10);
    CHECK(strcmp(gs(buf), "C:\\Game\\R") == 0 && g_err == W32_ERROR_INSUFFICIENT_BUFFER);
    CHECK(call("GetModuleFileNameA", 3, 0u, buf, 22u) == 22);
    CHECK(strcmp(gs(buf), "C:\\Game\\RecompTest.ex") == 0);
    CHECK(call("GetModuleFileNameA", 3, 0x10000000u, buf, 260u) == 0 && g_err == 126);
}

static void test_console(void)
{
    uint32_t in = call("GetStdHandle", 1, 0xFFFFFFF6u);
    uint32_t out = call("GetStdHandle", 1, 0xFFFFFFF5u);
    uint32_t err = call("GetStdHandle", 1, 0xFFFFFFF4u);
    uint32_t n = galloc(4), mode = galloc(4);

    CHECK(in != INVALID && out != INVALID && err != INVALID && in != out && out != err);
    CHECK(call("GetStdHandle", 1, 0xFFFFFFF5u) == out);
    CHECK(call("GetFileType", 1, out) == 2);
    CHECK(write_text(out, "test_win32_fs: WriteFile to stdout\n") == 35);
    CHECK(write_text(in, "x") == INVALID && g_err == W32_ERROR_ACCESS_DENIED);
    CHECK(call("WriteConsoleA", 5, err, gstr("test_win32_fs: WriteConsoleA\n"), 29u, n, 0u) == W32_TRUE);
    CHECK(rd32(n) == 29);
    /* "WriteConsoleW \u00e9\U0001F600\n": a 2-byte and a surrogate-pair character. */
    static const uint16_t wide[] = {'t', 'e', 's', 't', '_', 'w', 'i', 'n', '3', '2', '_', 'f', 's', ':', ' ',
                                    'W', ' ', 0xE9, ' ', 0xD83D, 0xDE00, '\n'};
    uint32_t w = galloc(sizeof wide);
    memcpy(guest_ptr(w), wide, sizeof wide);
    CHECK(call("WriteConsoleW", 5, err, w, (uint32_t)(sizeof wide / 2), n, 0u) == W32_TRUE);
    CHECK(rd32(n) == sizeof wide / 2);
    CHECK(call("WriteConsoleA", 5, 0x1234u, gstr("x"), 1u, n, 0u) == W32_FALSE);
    CHECK(g_err == W32_ERROR_INVALID_HANDLE);
    CHECK(call("GetConsoleCP", 0) == 437);
    CHECK(call("GetConsoleOutputCP", 0) == 437);
    CHECK(call("GetConsoleMode", 2, out, mode) == W32_FALSE && g_err == W32_ERROR_INVALID_HANDLE);
    CHECK(call("SetConsoleCtrlHandler", 2, 0x401000u, 1u) == W32_TRUE);
    CHECK(call("SetStdHandle", 2, 0xFFFFFFF5u, out) == W32_TRUE);
    CHECK(call("SetHandleCount", 1, 20u) == 20);
}

static void test_drives(void)
{
    uint32_t name = galloc(32), fs = galloc(32), serial = galloc(4), maxc = galloc(4), flags = galloc(4);

    CHECK(call("GetLogicalDrives", 0) == 0x4u);
    CHECK(call("GetDriveTypeA", 1, gstr("C:\\")) == 3);
    CHECK(call("GetDriveTypeA", 1, gstr("c:")) == 3);
    CHECK(call("GetDriveTypeA", 1, 0u) == 3);
    CHECK(call("GetDriveTypeA", 1, gstr("D:\\")) == 1);
    CHECK(call("GetVolumeInformationA", 8, gstr("C:\\"), name, 32u, serial, maxc, flags, fs, 32u) == W32_TRUE);
    CHECK(strcmp(gs(name), "MAC") == 0 && strcmp(gs(fs), "NTFS") == 0);
    CHECK(rd32(serial) == 0x4C485052u && rd32(maxc) == 255);
    CHECK(call("GetVolumeInformationA", 8, gstr("D:\\"), name, 32u, serial, maxc, flags, fs, 32u) == W32_FALSE);
}

/* w32_fs_mkdir_p: missing parents (with a space, as in "Application
 * Support") are created; an existing path is fine; a file in the way fails. */
static void test_mkdir_p(void)
{
    char p[1200], f[1200];
    struct stat st;
    snprintf(p, sizeof p, "%s/home/Library/Application Support/HarryPotterRecomp", g_root);
    CHECK(w32_fs_mkdir_p(p) == 0 && stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(w32_fs_mkdir_p(p) == 0);
    snprintf(f, sizeof f, "%s/home/file", g_root);
    FILE *fp = fopen(f, "w");
    CHECK(fp != NULL);
    if (fp != NULL)
        fclose(fp);
    snprintf(p, sizeof p, "%s/sub", f);
    CHECK(w32_fs_mkdir_p(p) == -1);
}

int main(int argc, char **argv)
{
    (void)argc;
    unsetenv("HPRECOMP_LANGID");
    if (guest_mem_init() != 0)
        return 1;
    g_xbox_mem_offset = (ptrdiff_t)g_guest_base;
    if (mprotect(guest_ptr(GUEST_LO), GUEST_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        return 1;
    }
    make_folders(argv[0]);

    test_paths();
    test_writes();
    test_seek();
    test_find();
    test_dirs();
    test_registry();
    test_shell();
    test_module();
    test_console();
    test_drives();
    test_mkdir_p();

    /* The game dir is exactly as seeded. */
    nftw(g_game, count_entry, 16, FTW_PHYS);
    CHECK(g_game_entries == 6); /* game, DATA, 4 files */
    CHECK(strcmp(host_read(g_game, "CONFIG.TXT"), "old config") == 0);
    CHECK(strcmp(host_read(g_game, "GAME.DAT"), "shipped data") == 0);

    int count = 0;
    for (int i = 0; win32_exports_fs[i].dll != NULL; i++, count++) {
        if (!g_called[i]) {
            fprintf(stderr, "FAIL: %s was never called\n", win32_exports_fs[i].name);
            g_fail = 1;
        }
    }
    CHECK(count == 42);

    nftw(g_game, chmod_entry, 16, FTW_PHYS);
    if (g_fail) {
        fprintf(stderr, "kept %s\n", g_root);
        return 1;
    }
    nftw(g_root, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    printf("ok: %d fs thunks, stack pops and callee-saved registers checked\n", count);
    return 0;
}
