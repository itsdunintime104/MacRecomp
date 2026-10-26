/* Win32 mem slice: heap and virtual memory, environment strings, code pages
 * and locale, string comparison and mapping, process and version
 * information, debug output and the unhandled-exception filter: the 56
 * KERNEL32 imports of slice "mem" (notes/slice_owners.md). Contract:
 * runtime/include/win32_thunk.h and docs/WIN32_THUNKS.md. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_heap.h"
#include "guest_call.h"
#include "diag.h"
#include "win32_thunk.h"

#define W32_ERROR_NO_UNICODE_TRANSLATION 1113u
#define W32_ERROR_OLD_WIN_VERSION 1150u

#define W32_IMAGE_BASE 0x00400000u
#define W32_STILL_ACTIVE 259u

/* ---- Shared helpers ---------------------------------------------------- */

/* Guest heap calls made on behalf of the guest carry its return address. */
static uint32_t thunk_malloc(uint32_t n)
{
    g_heap_site = MEM32(g_esp);
    uint32_t a = guest_malloc(n);
    g_heap_site = 0;
    return a;
}

static void thunk_free(uint32_t a)
{
    g_heap_site = MEM32(g_esp);
    guest_free(a);
    g_heap_site = 0;
}

/* UTF-16 units are read and written byte-wise: guest strings need not be
 * 2-byte aligned. */
static uint32_t get16(const uint8_t *p, uint32_t i)
{
    return (uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 8;
}

static void put16(uint8_t *p, uint32_t i, uint32_t v)
{
    p[2 * i] = (uint8_t)v;
    p[2 * i + 1] = (uint8_t)(v >> 8);
}

static uint32_t wide_len(const uint8_t *p)
{
    uint32_t n = 0;
    while (get16(p, n) != 0)
        n++;
    return n;
}

/* Copy s and its NUL to the guest buffer dst of cch characters (bytes or
 * UTF-16 units), with the usual Win32 rules: cch 0 asks for the size. */
static uint32_t put_string(const char *s, uint32_t dst, int32_t cch, int wide)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    if (cch < 0 || (cch > 0 && dst == 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cch == 0)
        return n;
    if (n > (uint32_t)cch) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    uint8_t *out = guest_ptr(dst);
    for (uint32_t i = 0; i < n; i++) {
        if (wide)
            put16(out, i, (uint8_t)s[i]);
        else
            out[i] = (uint8_t)s[i];
    }
    return n;
}

/* A growing host string. */
struct sbuf {
    char *p;
    size_t len, cap;
};

static int sb_add(struct sbuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = (sb->len + n + 1) * 2;
        char *p = realloc(sb->p, cap);
        if (p == NULL)
            return -1;
        sb->p = p;
        sb->cap = cap;
    }
    memcpy(sb->p + sb->len, s, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
    return 0;
}

/* ---- Process state ----------------------------------------------------- */

/* The environment the guest sees: a fixed minimal Windows one, never the
 * host's (it holds local paths). Entries are "NAME=value" host strings. */
static const char *const k_env_fixed[] = {
    "PATH=C:\\Windows\\system32;C:\\Windows",
    "SystemRoot=C:\\Windows",
    "TEMP=C:\\Users\\Player\\AppData\\Local\\Temp",
    "TMP=C:\\Users\\Player\\AppData\\Local\\Temp",
    "USERPROFILE=C:\\Users\\Player",
    "APPDATA=C:\\Users\\Player\\AppData\\Roaming",
    "LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local",
    "COMPUTERNAME=MAC",
    "USERNAME=Player",
    "OS=Windows_NT",
};

static pthread_once_t g_mem_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;
static char **g_env;
static size_t g_env_count;
static uint32_t g_process_heap; /* GetProcessHeap's handle */
static uint32_t g_cmdline;      /* guest string */
static uint32_t g_locale_id;    /* guest "00000409" for EnumSystemLocalesA */
static uint32_t g_langid = 0x0409;
static _Atomic uint32_t g_unhandled_filter;

static void env_append(const char *entry)
{
    char *copy = strdup(entry);
    char **env = realloc(g_env, (g_env_count + 1) * sizeof *env);
    if (copy == NULL || env == NULL) {
        fprintf(stderr, "win32_mem: out of host memory for the environment\n");
        abort();
    }
    g_env = env;
    g_env[g_env_count++] = copy;
}

static uint32_t guest_string(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = guest_malloc(n);
    if (a == 0) {
        fprintf(stderr, "win32_mem: out of guest memory\n");
        abort();
    }
    memcpy(guest_ptr(a), s, n);
    return a;
}

static void mem_init(void)
{
    for (size_t i = 0; i < sizeof k_env_fixed / sizeof k_env_fixed[0]; i++)
        env_append(k_env_fixed[i]);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char entry[64];
    snprintf(entry, sizeof entry, "NUMBER_OF_PROCESSORS=%ld", cpus < 1 ? 1 : cpus > 8 ? 8 : cpus);
    env_append(entry);

    struct sbuf cmd = {0};
    const char *args = getenv("HPRECOMP_ARGS");
    const char *exe = "\"C:\\Game\\LEGOHarryPotter.exe\"";
    if (sb_add(&cmd, exe, strlen(exe)) != 0 ||
        (args != NULL && args[0] != '\0' &&
         (sb_add(&cmd, " ", 1) != 0 || sb_add(&cmd, args, strlen(args)) != 0))) {
        fprintf(stderr, "win32_mem: out of host memory for the command line\n");
        abort();
    }
    g_cmdline = guest_string(cmd.p);
    free(cmd.p);

    g_locale_id = guest_string("00000409");
    g_process_heap = guest_calloc(1, 16);

    const char *lang = getenv("HPRECOMP_LANGID");
    if (lang != NULL && lang[0] != '\0') {
        char *end;
        unsigned long v = strtoul(lang, &end, 10);
        if (*end == '\0' && v != 0 && v <= 0xFFFF)
            g_langid = (uint32_t)v;
    }
}

static void mem_ready(void)
{
    pthread_once(&g_mem_once, mem_init);
}

/* Index of the entry for name[0..n), or -1. Caller holds g_env_lock. */
static long env_find(const char *name, size_t n)
{
    for (size_t i = 0; i < g_env_count; i++)
        if (strncasecmp(g_env[i], name, n) == 0 && g_env[i][n] == '=')
            return (long)i;
    return -1;
}

/* ---- Code pages -------------------------------------------------------- */

#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 0x8u

/* CP1252 0x80-0x9F; the five unassigned bytes map to the C1 control of the
 * same value, as Windows does. 0xA0-0xFF are Latin-1. */
static const uint16_t k_cp1252_80[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* CP437 0x80-0xFF. */
static const uint16_t k_cp437_80[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* 1252, 437 or CP_UTF8 for a code page argument; 0 when unsupported. */
static uint32_t cp_resolve(uint32_t cp)
{
    switch (cp) {
    case 0: /* CP_ACP */
    case 1252:
        return 1252;
    case 1: /* CP_OEMCP */
    case 437:
        return 437;
    case CP_UTF8:
        return CP_UTF8;
    default:
        return 0;
    }
}

static uint32_t sbcs_to_wide(uint32_t cp, uint8_t b)
{
    if (b < 0x80)
        return b;
    if (cp == 437)
        return k_cp437_80[b - 0x80];
    return b < 0xA0 ? k_cp1252_80[b - 0x80] : b;
}

/* 1 and *out set when u has a byte in cp. */
static int wide_to_sbcs(uint32_t cp, uint32_t u, uint8_t *out)
{
    if (u < 0x80 || (cp == 1252 && u >= 0xA0 && u <= 0xFF)) {
        *out = (uint8_t)u;
        return 1;
    }
    const uint16_t *table = cp == 437 ? k_cp437_80 : k_cp1252_80;
    uint32_t n = cp == 437 ? 128u : 32u;
    for (uint32_t i = 0; i < n; i++) {
        if (table[i] == u) {
            *out = (uint8_t)(0x80 + i);
            return 1;
        }
    }
    return 0;
}

/* Next code point of s[0..n) at *i, advancing *i. An invalid or truncated
 * sequence consumes one byte and gives U+FFFD, or -1 when strict. */
static int32_t utf8_next(const uint8_t *s, uint32_t n, uint32_t *i, int strict)
{
    uint32_t c = s[*i], len, min;
    if (c < 0x80) {
        (*i)++;
        return (int32_t)c;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2, c &= 0x1F, min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3, c &= 0x0F, min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4, c &= 0x07, min = 0x10000;
    } else {
        goto bad;
    }
    if (n - *i < len)
        goto bad;
    for (uint32_t k = 1; k < len; k++) {
        uint32_t b = s[*i + k];
        if ((b & 0xC0) != 0x80)
            goto bad;
        c = c << 6 | (b & 0x3F);
    }
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        goto bad;
    *i += len;
    return (int32_t)c;
bad:
    (*i)++;
    return strict ? -1 : 0xFFFD;
}

/* Decode n bytes of s in code page cp (a cp_resolve result) to UTF-16,
 * writing at most cap units to out (NULL: count only). Returns the unit
 * count, or -1 for invalid UTF-8 when strict. */
static int64_t mb_to_wide(uint32_t cp, const uint8_t *s, uint32_t n, uint8_t *out, uint32_t cap,
                          int strict)
{
    uint32_t count = 0;
#define EMIT16(v)                       \
    do {                                \
        if (out != NULL && count < cap) \
            put16(out, count, (v));     \
        count++;                        \
    } while (0)
    for (uint32_t i = 0; i < n;) {
        uint32_t u;
        if (cp == CP_UTF8) {
            int32_t c = utf8_next(s, n, &i, strict);
            if (c < 0)
                return -1;
            u = (uint32_t)c;
        } else {
            u = sbcs_to_wide(cp, s[i++]);
        }
        if (u >= 0x10000) {
            EMIT16(0xD800 + ((u - 0x10000) >> 10));
            EMIT16(0xDC00 + ((u - 0x10000) & 0x3FF));
        } else {
            EMIT16(u);
        }
    }
#undef EMIT16
    return count;
}

/* Encode n UTF-16 units of s to code page cp, writing at most cap bytes to
 * out (NULL: count only). A unit with no byte in a single-byte code page
 * becomes def and sets *used; an unpaired surrogate becomes U+FFFD in
 * UTF-8. Returns the byte count. */
static uint32_t wide_to_mb(uint32_t cp, const uint8_t *s, uint32_t n, uint8_t *out, uint32_t cap,
                           uint8_t def, int *used)
{
    uint32_t count = 0;
#define EMIT8(v)                        \
    do {                                \
        if (out != NULL && count < cap) \
            out[count] = (uint8_t)(v);  \
        count++;                        \
    } while (0)
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = get16(s, i);
        if (cp != CP_UTF8) {
            uint8_t b;
            if (!wide_to_sbcs(cp, u, &b)) {
                b = def;
                *used = 1;
            }
            EMIT8(b);
            continue;
        }
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            uint32_t lo = get16(s, i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF)
            u = 0xFFFD;
        if (u < 0x80) {
            EMIT8(u);
        } else if (u < 0x800) {
            EMIT8(0xC0 | u >> 6);
            EMIT8(0x80 | (u & 0x3F));
        } else if (u < 0x10000) {
            EMIT8(0xE0 | u >> 12);
            EMIT8(0x80 | (u >> 6 & 0x3F));
            EMIT8(0x80 | (u & 0x3F));
        } else {
            EMIT8(0xF0 | u >> 18);
            EMIT8(0x80 | (u >> 12 & 0x3F));
            EMIT8(0x80 | (u >> 6 & 0x3F));
            EMIT8(0x80 | (u & 0x3F));
        }
    }
#undef EMIT8
    return count;
}

/* ---- Case and character classes (Latin-1) ------------------------------ */

#define C1_UPPER 0x001u
#define C1_LOWER 0x002u
#define C1_DIGIT 0x004u
#define C1_SPACE 0x008u
#define C1_PUNCT 0x010u
#define C1_CNTRL 0x020u
#define C1_BLANK 0x040u
#define C1_XDIGIT 0x080u
#define C1_ALPHA 0x100u
#define C1_DEFINED 0x200u

static uint32_t latin1_upper(uint32_t u)
{
    if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7))
        return u - 0x20;
    return u;
}

static uint32_t latin1_lower(uint32_t u)
{
    if ((u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7))
        return u + 0x20;
    return u;
}

/* CT_CTYPE1 class of one UTF-16 unit. Units above Latin-1 are not
 * classified beyond "defined, punctuation". */
static uint32_t ctype1(uint32_t u)
{
    uint32_t t = C1_DEFINED;
    if (u > 0xFF)
        return t | C1_PUNCT;
    if (u < 0x20 || (u >= 0x7F && u < 0xA0)) {
        t |= C1_CNTRL;
        if (u >= 0x09 && u <= 0x0D)
            t |= C1_SPACE;
        if (u == 0x09)
            t |= C1_BLANK;
        return t;
    }
    if (u == 0x20 || u == 0xA0)
        return t | C1_SPACE | C1_BLANK;
    if (u >= '0' && u <= '9')
        return t | C1_DIGIT | C1_XDIGIT;
    if (latin1_lower(u) != u)
        t |= C1_UPPER | C1_ALPHA;
    else if (latin1_upper(u) != u || u == 0xAA || u == 0xB5 || u == 0xBA || u == 0xDF || u == 0xFF)
        t |= C1_LOWER | C1_ALPHA;
    else if (u == 0xB2 || u == 0xB3 || u == 0xB9)
        return t | C1_PUNCT | C1_DIGIT;
    else
        return t | C1_PUNCT;
    if (u < 0x80 && (u | 0x20) >= 'a' && (u | 0x20) <= 'f')
        t |= C1_XDIGIT;
    return t;
}

/* ---- Locale ------------------------------------------------------------ */

#define LOCALE_NOUSEROVERRIDE 0x80000000u
#define LOCALE_USE_CP_ACP 0x40000000u

static const char *const k_day[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const k_day_abbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_month[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};
static const char *const k_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* en-US answers for the LCTYPEs the MSVC CRT reads (getqloc.c, initnum.c,
 * initmon.c, inittime.c) and a few neighbours. */
static const struct {
    uint32_t type;
    const char *value;
} k_locale[] = {
    {0x0001, "0409"},                    /* LOCALE_ILANGUAGE */
    {0x0002, "English (United States)"}, /* LOCALE_SLANGUAGE */
    {0x0003, "ENU"},                     /* LOCALE_SABBREVLANGNAME */
    {0x0004, "English"},                 /* LOCALE_SNATIVELANGNAME */
    {0x0005, "1"},                       /* LOCALE_ICOUNTRY */
    {0x0006, "United States"},           /* LOCALE_SCOUNTRY */
    {0x0007, "USA"},                     /* LOCALE_SABBREVCTRYNAME */
    {0x0008, "United States"},           /* LOCALE_SNATIVECTRYNAME */
    {0x0009, "0409"},                    /* LOCALE_IDEFAULTLANGUAGE */
    {0x000A, "1"},                       /* LOCALE_IDEFAULTCOUNTRY */
    {0x000B, "437"},                     /* LOCALE_IDEFAULTCODEPAGE */
    {0x000C, ","},                       /* LOCALE_SLIST */
    {0x000D, "1"},                       /* LOCALE_IMEASURE */
    {0x000E, "."},                       /* LOCALE_SDECIMAL */
    {0x000F, ","},                       /* LOCALE_STHOUSAND */
    {0x0010, "3;0"},                     /* LOCALE_SGROUPING */
    {0x0011, "2"},                       /* LOCALE_IDIGITS */
    {0x0012, "1"},                       /* LOCALE_ILZERO */
    {0x0013, "0123456789"},              /* LOCALE_SNATIVEDIGITS */
    {0x0014, "$"},                       /* LOCALE_SCURRENCY */
    {0x0015, "USD"},                     /* LOCALE_SINTLSYMBOL */
    {0x0016, "."},                       /* LOCALE_SMONDECIMALSEP */
    {0x0017, ","},                       /* LOCALE_SMONTHOUSANDSEP */
    {0x0018, "3;0"},                     /* LOCALE_SMONGROUPING */
    {0x0019, "2"},                       /* LOCALE_ICURRDIGITS */
    {0x001A, "2"},                       /* LOCALE_IINTLCURRDIGITS */
    {0x001B, "0"},                       /* LOCALE_ICURRENCY */
    {0x001C, "0"},                       /* LOCALE_INEGCURR */
    {0x001D, "/"},                       /* LOCALE_SDATE */
    {0x001E, ":"},                       /* LOCALE_STIME */
    {0x001F, "M/d/yyyy"},                /* LOCALE_SSHORTDATE */
    {0x0020, "dddd, MMMM dd, yyyy"},     /* LOCALE_SLONGDATE */
    {0x0021, "0"},                       /* LOCALE_IDATE */
    {0x0022, "0"},                       /* LOCALE_ILDATE */
    {0x0023, "0"},                       /* LOCALE_ITIME */
    {0x0024, "1"},                       /* LOCALE_ICENTURY */
    {0x0025, "0"},                       /* LOCALE_ITLZERO */
    {0x0026, "0"},                       /* LOCALE_IDAYLZERO */
    {0x0027, "0"},                       /* LOCALE_IMONLZERO */
    {0x0028, "AM"},                      /* LOCALE_S1159 */
    {0x0029, "PM"},                      /* LOCALE_S2359 */
    {0x0050, ""},                        /* LOCALE_SPOSITIVESIGN */
    {0x0051, "-"},                       /* LOCALE_SNEGATIVESIGN */
    {0x0052, "3"},                       /* LOCALE_IPOSSIGNPOSN */
    {0x0053, "0"},                       /* LOCALE_INEGSIGNPOSN */
    {0x0054, "1"},                       /* LOCALE_IPOSSYMPRECEDES */
    {0x0055, "0"},                       /* LOCALE_IPOSSEPBYSPACE */
    {0x0056, "1"},                       /* LOCALE_INEGSYMPRECEDES */
    {0x0057, "0"},                       /* LOCALE_INEGSEPBYSPACE */
    {0x0059, "en"},                      /* LOCALE_SISO639LANGNAME */
    {0x005A, "US"},                      /* LOCALE_SISO3166CTRYNAME */
    {0x1001, "English"},                 /* LOCALE_SENGLANGUAGE */
    {0x1002, "United States"},           /* LOCALE_SENGCOUNTRY */
    {0x1003, "h:mm:ss tt"},              /* LOCALE_STIMEFORMAT */
    {0x1004, "1252"},                    /* LOCALE_IDEFAULTANSICODEPAGE */
    {0x1009, "1"},                       /* LOCALE_ICALENDARTYPE */
    {0x100C, "6"},                       /* LOCALE_IFIRSTDAYOFWEEK */
    {0x100D, "0"},                       /* LOCALE_IFIRSTWEEKOFYEAR */
    {0x1010, "1"},                       /* LOCALE_INEGNUMBER */
    {0x1011, "10000"},                   /* LOCALE_IDEFAULTMACCODEPAGE */
};

/* The en-US value of an LCTYPE (flag bits removed), or NULL. Every LCID
 * gets the en-US answers. */
static const char *locale_value(uint32_t type)
{
    if (type >= 0x2A && type <= 0x30) /* LOCALE_SDAYNAME1 (Monday) .. 7 */
        return k_day[(type - 0x2A + 1) % 7];
    if (type >= 0x31 && type <= 0x37) /* LOCALE_SABBREVDAYNAME1 .. 7 */
        return k_day_abbr[(type - 0x31 + 1) % 7];
    if (type >= 0x38 && type <= 0x43) /* LOCALE_SMONTHNAME1 .. 12 */
        return k_month[type - 0x38];
    if (type >= 0x44 && type <= 0x4F) /* LOCALE_SABBREVMONTHNAME1 .. 12 */
        return k_month_abbr[type - 0x44];
    for (size_t i = 0; i < sizeof k_locale / sizeof k_locale[0]; i++)
        if (k_locale[i].type == type)
            return k_locale[i].value;
    return NULL;
}

static uint32_t locale_info(const char *func, int wide)
{
    uint32_t type = W32_ARG(1), buf = W32_ARG(2);
    int32_t cch = (int32_t)W32_ARG(3);
    const char *v = locale_value(type & ~(LOCALE_NOUSEROVERRIDE | LOCALE_USE_CP_ACP));
    if (v == NULL) {
        diag_unsupported(func, MEM32(g_esp), "LCTYPE 0x%X", type);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return put_string(v, buf, cch, wide);
}

/* ---- Date and time pictures -------------------------------------------- */

struct systime {
    uint32_t year, month, dow, day, hour, minute, second;
};

static uint32_t day_of_week(uint32_t y, uint32_t m, uint32_t d)
{
    static const uint32_t t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* The host's local time, or the guest SYSTEMTIME at p. Returns -1 when the
 * guest value is out of range. */
static int read_systime(uint32_t p, struct systime *t)
{
    if (p == 0) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        *t = (struct systime){(uint32_t)tm.tm_year + 1900, (uint32_t)tm.tm_mon + 1,
                              (uint32_t)tm.tm_wday, (uint32_t)tm.tm_mday, (uint32_t)tm.tm_hour,
                              (uint32_t)tm.tm_min, (uint32_t)tm.tm_sec};
        return 0;
    }
    static const uint8_t mdays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    t->year = MEM16(p);
    t->month = MEM16(p + 2);
    t->day = MEM16(p + 6);
    t->hour = MEM16(p + 8);
    t->minute = MEM16(p + 10);
    t->second = MEM16(p + 12);
    int leap = (t->year % 4 == 0 && t->year % 100 != 0) || t->year % 400 == 0;
    if (t->year < 1601 || t->month < 1 || t->month > 12 || t->day < 1 ||
        t->day > mdays[t->month - 1] - (t->month == 2 && !leap) || t->hour > 23 ||
        t->minute > 59 || t->second > 59)
        return -1;
    /* Windows ignores wDayOfWeek and works it out. */
    t->dow = day_of_week(t->year, t->month, t->day);
    return 0;
}

/* Expand a GetDateFormat (date) or GetTimeFormat picture with en-US names.
 * 'quoted' text is literal; '' inside quotes is one quote. */
static int format_picture(struct sbuf *sb, const char *pic, const struct systime *t, int date)
{
    char num[16];
    for (const char *p = pic; *p != '\0';) {
        char c = *p;
        if (c == '\'') {
            for (p++; *p != '\0'; p++) {
                if (*p == '\'') {
                    if (p[1] != '\'') {
                        p++;
                        break;
                    }
                    p++;
                }
                if (sb_add(sb, p, 1) != 0)
                    return -1;
            }
            continue;
        }
        size_t n = 1;
        while (p[n] == c)
            n++;
        /* A number field: one letter has no leading zero, two letters do. */
        int value = -1;
        const char *s = NULL;
        if (date && c == 'd' && n <= 2)
            value = (int)t->day;
        else if (date && c == 'd')
            s = n == 3 ? k_day_abbr[t->dow] : k_day[t->dow];
        else if (date && c == 'M' && n <= 2)
            value = (int)t->month;
        else if (date && c == 'M')
            s = n == 3 ? k_month_abbr[t->month - 1] : k_month[t->month - 1];
        else if (date && c == 'y' && n <= 2)
            value = (int)(t->year % 100);
        else if (date && c == 'y') {
            snprintf(num, sizeof num, "%u", t->year);
            s = num;
        }
        else if (date && c == 'g')
            s = "A.D.";
        else if (!date && c == 'h')
            value = (int)(t->hour % 12 == 0 ? 12 : t->hour % 12);
        else if (!date && c == 'H')
            value = (int)t->hour;
        else if (!date && c == 'm')
            value = (int)t->minute;
        else if (!date && c == 's')
            value = (int)t->second;
        else if (!date && c == 't')
            s = t->hour < 12 ? (n == 1 ? "A" : "AM") : (n == 1 ? "P" : "PM");
        if (value >= 0) {
            snprintf(num, sizeof num, n == 1 ? "%d" : "%02d", value);
            s = num;
        }
        if (s == NULL ? sb_add(sb, p, n) != 0 : sb_add(sb, s, strlen(s)) != 0)
            return -1;
        p += n;
    }
    return 0;
}

/* GetDateFormatA / GetTimeFormatA: (LCID, flags, SYSTEMTIME*, picture,
 * out, cch). Only the default pictures and an explicit picture with no
 * flags are supported. */
static uint32_t date_time_format(const char *func, int date)
{
    uint32_t flags = W32_ARG(1) & ~LOCALE_NOUSEROVERRIDE, st = W32_ARG(2), pic = W32_ARG(3);
    const char *picture;
    if (pic != 0 && flags == 0)
        picture = guest_ptr(pic);
    else if (pic == 0 && date && (flags == 0 || flags == 1)) /* DATE_SHORTDATE */
        picture = "M/d/yyyy";
    else if (pic == 0 && date && flags == 2) /* DATE_LONGDATE */
        picture = "dddd, MMMM dd, yyyy";
    else if (pic == 0 && !date && flags == 0)
        picture = "h:mm:ss tt";
    else {
        diag_unsupported(func, MEM32(g_esp), "flags 0x%X", W32_ARG(1));
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct systime t;
    if (read_systime(st, &t) != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct sbuf sb = {0};
    if (sb_add(&sb, "", 0) != 0 || format_picture(&sb, picture, &t, date) != 0) {
        free(sb.p);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    uint32_t r = put_string(sb.p, W32_ARG(4), (int32_t)W32_ARG(5), 0);
    free(sb.p);
    return r;
}

/* ---- Heap -------------------------------------------------------------- */

#define HEAP_ZERO_MEMORY 0x8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

/* Every heap shares the one guest heap. A block carries a 16-byte header
 * holding the size the guest asked for (HeapSize returns it), so the guest
 * pointer stays 16-byte aligned. */
#define HEAP_HDR 16u

static uint32_t heap_alloc(uint32_t n, int zero)
{
    if (n > 0xFFFFFFFFu - HEAP_HDR)
        return 0;
    g_heap_site = MEM32(g_esp);
    uint32_t a = zero ? guest_calloc(1, n + HEAP_HDR) : guest_malloc(n + HEAP_HDR);
    g_heap_site = 0;
    if (a == 0)
        return 0;
    MEM32(a) = n;
    return a + HEAP_HDR;
}

/* HANDLE GetProcessHeap(void) */
static void imp_GetProcessHeap(void)
{
    mem_ready();
    W32_RETURN(0, g_process_heap);
}

/* HANDLE HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize)
 * Each call gets its own 16-byte guest block as the handle, so handles are
 * distinct; the options and sizes are not enforced. */
static void imp_HeapCreate(void)
{
    g_heap_site = MEM32(g_esp);
    uint32_t h = guest_calloc(1, 16);
    g_heap_site = 0;
    if (h == 0)
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    W32_RETURN(3, h);
}

/* BOOL HeapDestroy(HANDLE hHeap)
 * Limitation: blocks still allocated from the heap are not freed, because
 * all heaps share the guest heap and no block records its heap. */
static void imp_HeapDestroy(void)
{
    W32_RETURN(1, W32_TRUE);
}

/* LPVOID HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) */
static void imp_HeapAlloc(void)
{
    W32_RETURN(3, heap_alloc(W32_ARG(2), (W32_ARG(1) & HEAP_ZERO_MEMORY) != 0));
}

/* LPVOID HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) */
static void imp_HeapReAlloc(void)
{
    uint32_t flags = W32_ARG(1), p = W32_ARG(2), n = W32_ARG(3), r = 0;
    if (p != 0 && n <= 0xFFFFFFFFu - HEAP_HDR) {
        uint32_t old = MEM32(p - HEAP_HDR);
        if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
            /* Only a shrink is known to fit in place. */
            if (n <= old) {
                MEM32(p - HEAP_HDR) = n;
                r = p;
            }
        } else {
            g_heap_site = MEM32(g_esp);
            uint32_t a = guest_realloc(p - HEAP_HDR, n + HEAP_HDR);
            g_heap_site = 0;
            if (a != 0) {
                MEM32(a) = n;
                r = a + HEAP_HDR;
                if ((flags & HEAP_ZERO_MEMORY) && n > old)
                    memset(guest_ptr(r + old), 0, n - old);
            }
        }
    }
    W32_RETURN(4, r);
}

/* BOOL HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) */
static void imp_HeapFree(void)
{
    uint32_t p = W32_ARG(2);
    if (p != 0)
        thunk_free(p - HEAP_HDR);
    W32_RETURN(3, W32_TRUE);
}

/* SIZE_T HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) */
static void imp_HeapSize(void)
{
    uint32_t p = W32_ARG(2);
    W32_RETURN(3, p == 0 ? 0xFFFFFFFFu : MEM32(p - HEAP_HDR));
}

/* ---- Virtual memory ---------------------------------------------------- */

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_TOP_DOWN 0x100000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READWRITE 0x04u

/* VirtualAlloc's arena: guest 0x02000000-0x1FFFFFFF. It lies above the PE
 * image (from 0x400000; HP's ends near 0x01400000) and below the guest
 * heap; 0xE0000000 and up hold stacks and fake addresses (guest_call.h,
 * host_funcs.h). Pages are 4 KB and reservations start on 64 KB boundaries,
 * as on Windows. */
#define VA_BASE 0x02000000u
#define VA_END GUEST_HEAP_BASE
#define VA_PAGE 0x1000u
#define VA_GRAN 0x10000u
#define VA_PAGES ((VA_END - VA_BASE) / VA_PAGE)
#define VA_HOST_PAGE 0x4000u /* arm64 macOS */
#define VA_PER_HOST (VA_HOST_PAGE / VA_PAGE)

enum { VA_FREE, VA_RESERVED, VA_COMMITTED };

static uint8_t g_va_state[VA_PAGES];
static uint32_t g_va_prot[VA_PAGES];                         /* PAGE_* of a committed page */
static uint32_t g_va_alloc[VA_PAGES];                        /* allocation base of a used page */
static uint32_t g_va_alloc_prot[(VA_END - VA_BASE) / VA_GRAN]; /* by allocation base */
static pthread_mutex_t g_va_lock = PTHREAD_MUTEX_INITIALIZER;

/* Host protection for a PAGE_* value, or -1. Guest memory is never
 * executable, so the EXECUTE kinds map to their data equivalents. */
static int host_prot(uint32_t prot)
{
    switch (prot) {
    case PAGE_NOACCESS:
        return PROT_NONE;
    case 0x02: /* PAGE_READONLY */
    case 0x10: /* PAGE_EXECUTE */
    case 0x20: /* PAGE_EXECUTE_READ */
        return PROT_READ;
    case PAGE_READWRITE:
    case 0x08: /* PAGE_WRITECOPY */
    case 0x40: /* PAGE_EXECUTE_READWRITE */
    case 0x80: /* PAGE_EXECUTE_WRITECOPY */
        return PROT_READ | PROT_WRITE;
    default:
        return -1;
    }
}

static uint32_t va_page(uint32_t a)
{
    return (a - VA_BASE) / VA_PAGE;
}

/* Give every host page that overlaps guest pages [p0, p1) the protection of
 * the committed pages it holds. A host page with none is replaced by a fresh
 * PROT_NONE mapping, which drops its contents; in a host page that keeps
 * committed pages, the uncommitted pages of [p0, p1) are zeroed, so a later
 * commit reads zeros. Limitation: host pages are 16 KB, so a reserved page
 * that shares one with a committed page is accessible. */
static void va_sync(uint32_t p0, uint32_t p1)
{
    for (uint32_t h = p0 / VA_PER_HOST * VA_PER_HOST; h < p1; h += VA_PER_HOST) {
        int prot = PROT_NONE, any = 0;
        for (uint32_t p = h; p < h + VA_PER_HOST; p++) {
            if (g_va_state[p] == VA_COMMITTED) {
                any = 1;
                prot |= host_prot(g_va_prot[p]);
            }
        }
        void *host = guest_ptr(VA_BASE + h * VA_PAGE);
        if (!any) {
            if (mmap(host, VA_HOST_PAGE, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) ==
                MAP_FAILED) {
                perror("win32_mem: mmap");
                abort();
            }
            continue;
        }
        if (mprotect(host, VA_HOST_PAGE, PROT_READ | PROT_WRITE) != 0) {
            perror("win32_mem: mprotect");
            abort();
        }
        for (uint32_t p = h; p < h + VA_PER_HOST; p++)
            if (p >= p0 && p < p1 && g_va_state[p] != VA_COMMITTED)
                memset(guest_ptr(VA_BASE + p * VA_PAGE), 0, VA_PAGE);
        if (prot != (PROT_READ | PROT_WRITE) && mprotect(host, VA_HOST_PAGE, prot) != 0) {
            perror("win32_mem: mprotect");
            abort();
        }
    }
}

/* First page of n free pages starting on a 64 KB boundary, or VA_PAGES. */
static uint32_t va_find_free(uint32_t n)
{
    const uint32_t gran = VA_GRAN / VA_PAGE;
    uint32_t p = 0;
    while (p + n <= VA_PAGES) {
        uint32_t q = p + n;
        while (q > p && g_va_state[q - 1] == VA_FREE)
            q--;
        if (q == p)
            return p;
        p = (q + gran - 1) / gran * gran; /* q - 1 is in use */
    }
    return VA_PAGES;
}

/* Pages [p0, p1) all belong to the reservation at base. */
static int va_in_one(uint32_t p0, uint32_t p1, uint32_t base)
{
    for (uint32_t p = p0; p < p1; p++)
        if (g_va_state[p] == VA_FREE || g_va_alloc[p] != base)
            return 0;
    return 1;
}

/* End page of the reservation whose first page is p0. */
static uint32_t va_extent(uint32_t p0)
{
    uint32_t base = g_va_alloc[p0], p = p0;
    while (p < VA_PAGES && g_va_state[p] != VA_FREE && g_va_alloc[p] == base)
        p++;
    return p;
}

static uint32_t va_fail(uint32_t code)
{
    w32_set_last_error(code);
    return 0;
}

/* Returns the base, or 0 with the last error set. Caller holds g_va_lock. */
static uint32_t va_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot)
{
    uint64_t end = ((uint64_t)addr + size + VA_PAGE - 1) & ~(uint64_t)(VA_PAGE - 1);
    uint32_t base, p0, p1;
    if ((type & MEM_RESERVE) || addr == 0) {
        if (addr != 0) {
            base = addr & ~(VA_GRAN - 1);
            if (base < VA_BASE || end > VA_END)
                return va_fail(W32_ERROR_INVALID_PARAMETER);
            p0 = va_page(base);
            p1 = va_page((uint32_t)end);
            for (uint32_t p = p0; p < p1; p++)
                if (g_va_state[p] != VA_FREE)
                    return va_fail(W32_ERROR_INVALID_PARAMETER);
        } else {
            uint32_t n = (uint32_t)(((uint64_t)size + VA_PAGE - 1) / VA_PAGE);
            p0 = n <= VA_PAGES ? va_find_free(n) : VA_PAGES;
            if (p0 == VA_PAGES)
                return va_fail(W32_ERROR_NOT_ENOUGH_MEMORY);
            p1 = p0 + n;
            base = VA_BASE + p0 * VA_PAGE;
        }
        g_va_alloc_prot[(base - VA_BASE) / VA_GRAN] = prot;
        for (uint32_t p = p0; p < p1; p++) {
            g_va_alloc[p] = base;
            g_va_state[p] = VA_RESERVED;
        }
    } else {
        base = addr & ~(VA_PAGE - 1);
        if (base < VA_BASE || end > VA_END)
            return va_fail(W32_ERROR_INVALID_PARAMETER);
        p0 = va_page(base);
        p1 = va_page((uint32_t)end);
        if (!va_in_one(p0, p1, g_va_alloc[p0]))
            return va_fail(W32_ERROR_INVALID_PARAMETER);
    }
    if (type & MEM_COMMIT) {
        for (uint32_t p = p0; p < p1; p++) {
            g_va_state[p] = VA_COMMITTED;
            g_va_prot[p] = prot;
        }
    }
    va_sync(p0, p1);
    return base;
}

/* LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType,
 *                     DWORD flProtect) */
static void imp_VirtualAlloc(void)
{
    uint32_t addr = W32_ARG(0), size = W32_ARG(1), type = W32_ARG(2), prot = W32_ARG(3), r = 0;
    if ((type & ~(MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN)) != 0 || host_prot(prot) < 0) {
        diag_unsupported("KERNEL32.dll!VirtualAlloc", MEM32(g_esp), "type 0x%X protect 0x%X", type,
                         prot);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else if (size == 0 || (type & (MEM_COMMIT | MEM_RESERVE)) == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        pthread_mutex_lock(&g_va_lock);
        r = va_alloc(addr, size, type, prot);
        pthread_mutex_unlock(&g_va_lock);
    }
    W32_RETURN(4, r);
}

/* BOOL VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) */
static void imp_VirtualFree(void)
{
    uint32_t addr = W32_ARG(0), size = W32_ARG(1), type = W32_ARG(2), ok = W32_FALSE;
    uint64_t end = ((uint64_t)addr + size + VA_PAGE - 1) & ~(uint64_t)(VA_PAGE - 1);
    pthread_mutex_lock(&g_va_lock);
    if (addr >= VA_BASE && addr < VA_END && end <= VA_END && (type == MEM_RELEASE || type == MEM_DECOMMIT)) {
        uint32_t p0 = va_page(addr & ~(VA_PAGE - 1)), p1;
        int whole = g_va_state[p0] != VA_FREE && g_va_alloc[p0] == addr;
        if (size == 0 && whole) {
            p1 = va_extent(p0);
            ok = W32_TRUE;
        } else {
            p1 = va_page((uint32_t)end);
            ok = type == MEM_DECOMMIT && size != 0 && va_in_one(p0, p1, g_va_alloc[p0]);
        }
        if (ok) {
            for (uint32_t p = p0; p < p1; p++)
                g_va_state[p] = type == MEM_RELEASE ? VA_FREE : VA_RESERVED;
            va_sync(p0, p1);
        }
    }
    pthread_mutex_unlock(&g_va_lock);
    if (!ok)
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    W32_RETURN(3, ok);
}

/* SIZE_T VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer,
 *                     SIZE_T dwLength)
 * Exact for the arena. The guest heap is one committed read/write region.
 * Any other address is reported as one 4 KB page: committed read/write when
 * it can be read, free otherwise. */
static void imp_VirtualQuery(void)
{
    uint32_t addr = W32_ARG(0), out = W32_ARG(1), len = W32_ARG(2), r = 0;
    uint32_t page = addr & ~(VA_PAGE - 1);
    uint32_t mbi[7] = {page, 0, 0, VA_PAGE, MEM_FREE, PAGE_NOACCESS, 0};
    if (out == 0 || len < sizeof mbi) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else if (addr >= VA_BASE && addr < VA_END) {
        pthread_mutex_lock(&g_va_lock);
        uint32_t p = va_page(page), q = p, s = g_va_state[p];
        while (q < VA_PAGES && g_va_state[q] == s &&
               (s == VA_FREE ||
                (g_va_alloc[q] == g_va_alloc[p] && (s != VA_COMMITTED || g_va_prot[q] == g_va_prot[p]))))
            q++;
        mbi[3] = (q - p) * VA_PAGE;
        if (s != VA_FREE) {
            mbi[1] = g_va_alloc[p];
            mbi[2] = g_va_alloc_prot[(g_va_alloc[p] - VA_BASE) / VA_GRAN];
            mbi[4] = s == VA_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
            mbi[5] = s == VA_COMMITTED ? g_va_prot[p] : 0;
            mbi[6] = MEM_PRIVATE;
        }
        pthread_mutex_unlock(&g_va_lock);
    } else {
        uint8_t byte;
        int heap = addr >= GUEST_HEAP_BASE && addr < GUEST_HEAP_END;
        if (heap || guest_mem_probe(page, &byte) == 0) {
            mbi[1] = heap ? GUEST_HEAP_BASE : page;
            mbi[2] = PAGE_READWRITE;
            mbi[3] = heap ? GUEST_HEAP_END - page : VA_PAGE;
            mbi[4] = MEM_COMMIT;
            mbi[5] = PAGE_READWRITE;
            mbi[6] = MEM_PRIVATE;
        }
    }
    if (out != 0 && len >= sizeof mbi) {
        for (uint32_t i = 0; i < 7; i++)
            MEM32(out + 4 * i) = mbi[i];
        r = sizeof mbi;
    }
    W32_RETURN(3, r);
}

/* ---- Environment ------------------------------------------------------- */

/* The current environment as a guest block of NUL-terminated strings ended
 * by an extra NUL; UTF-16 when wide. */
static uint32_t env_block(int wide)
{
    mem_ready();
    pthread_mutex_lock(&g_env_lock);
    uint32_t units = 1;
    for (size_t i = 0; i < g_env_count; i++)
        units += (uint32_t)strlen(g_env[i]) + 1;
    if (g_env_count == 0)
        units = 2;
    uint32_t block = thunk_malloc(units * (wide ? 2u : 1u));
    if (block != 0) {
        uint8_t *out = guest_ptr(block);
        uint32_t at = 0;
        for (size_t i = 0; i < g_env_count; i++) {
            uint32_t n = (uint32_t)strlen(g_env[i]) + 1;
            if (wide)
                mb_to_wide(1252, (const uint8_t *)g_env[i], n, out + 2 * at, n, 0);
            else
                memcpy(out + at, g_env[i], n);
            at += n;
        }
        for (; at < units; at++) {
            if (wide)
                put16(out, at, 0);
            else
                out[at] = 0;
        }
    } else {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    }
    pthread_mutex_unlock(&g_env_lock);
    return block;
}

/* LPCH GetEnvironmentStrings(void) */
static void imp_GetEnvironmentStrings(void)
{
    W32_RETURN(0, env_block(0));
}

/* LPWCH GetEnvironmentStringsW(void) */
static void imp_GetEnvironmentStringsW(void)
{
    W32_RETURN(0, env_block(1));
}

/* BOOL FreeEnvironmentStringsA(LPCH) */
static void imp_FreeEnvironmentStringsA(void)
{
    if (W32_ARG(0) != 0)
        thunk_free(W32_ARG(0));
    W32_RETURN(1, W32_TRUE);
}

/* BOOL FreeEnvironmentStringsW(LPWCH) */
static void imp_FreeEnvironmentStringsW(void)
{
    if (W32_ARG(0) != 0)
        thunk_free(W32_ARG(0));
    W32_RETURN(1, W32_TRUE);
}

/* BOOL SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue)
 * Edits the host-side copy; lpValue NULL deletes the variable. */
static void imp_SetEnvironmentVariableA(void)
{
    uint32_t name = W32_ARG(0), value = W32_ARG(1), ok = W32_FALSE;
    const char *n = name != 0 ? guest_ptr(name) : NULL;
    if (n == NULL || n[0] == '\0' || strchr(n + 1, '=') != NULL) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        mem_ready();
        pthread_mutex_lock(&g_env_lock);
        long i = env_find(n, strlen(n));
        if (i >= 0) {
            free(g_env[i]);
            g_env[i] = g_env[--g_env_count];
        }
        ok = W32_TRUE;
        if (value != 0) {
            struct sbuf sb = {0};
            const char *v = guest_ptr(value);
            if (sb_add(&sb, n, strlen(n)) == 0 && sb_add(&sb, "=", 1) == 0 &&
                sb_add(&sb, v, strlen(v)) == 0) {
                env_append(sb.p);
            } else {
                w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
                ok = W32_FALSE;
            }
            free(sb.p);
        }
        pthread_mutex_unlock(&g_env_lock);
    }
    W32_RETURN(2, ok);
}

/* DWORD ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize)
 * Returns the characters needed, NUL included; an unknown %NAME% is kept. */
static void imp_ExpandEnvironmentStringsA(void)
{
    uint32_t src = W32_ARG(0), dst = W32_ARG(1), size = W32_ARG(2), r = 0;
    mem_ready();
    struct sbuf sb = {0};
    int fail = sb_add(&sb, "", 0);
    pthread_mutex_lock(&g_env_lock);
    for (const char *s = src != 0 ? guest_ptr(src) : ""; *s != '\0' && fail == 0;) {
        const char *end = s[0] == '%' ? strchr(s + 1, '%') : NULL;
        if (end == NULL || end == s + 1) {
            fail = sb_add(&sb, s, 1);
            s++;
            continue;
        }
        long i = env_find(s + 1, (size_t)(end - s - 1));
        if (i >= 0) {
            const char *v = g_env[i] + (end - s);
            fail = sb_add(&sb, v, strlen(v));
        } else {
            fail = sb_add(&sb, s, (size_t)(end - s + 1));
        }
        s = end + 1;
    }
    pthread_mutex_unlock(&g_env_lock);
    if (src == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else if (fail != 0) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
    } else {
        r = (uint32_t)sb.len + 1;
        if (dst != 0 && r <= size)
            memcpy(guest_ptr(dst), sb.p, r);
    }
    free(sb.p);
    W32_RETURN(3, r);
}

/* ---- Code page imports ------------------------------------------------- */

/* UINT GetACP(void) */
static void imp_GetACP(void)
{
    W32_RETURN(0, 1252);
}

/* UINT GetOEMCP(void) */
static void imp_GetOEMCP(void)
{
    W32_RETURN(0, 437);
}

/* BOOL GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo) */
static void imp_GetCPInfo(void)
{
    uint32_t cp = cp_resolve(W32_ARG(0)), info = W32_ARG(1), ok = W32_FALSE;
    if (cp == 0 || info == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        memset(guest_ptr(info), 0, 18);
        MEM32(info) = cp == CP_UTF8 ? 4 : 1; /* MaxCharSize */
        MEM8(info + 4) = '?';                /* DefaultChar; no LeadByte ranges */
        ok = W32_TRUE;
    }
    W32_RETURN(2, ok);
}

/* BOOL IsValidCodePage(UINT CodePage) */
static void imp_IsValidCodePage(void)
{
    uint32_t cp = W32_ARG(0);
    W32_RETURN(1, cp == 1252 || cp == 437 || cp == CP_UTF8);
}

/* int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr,
 *                         int cbMultiByte, LPWSTR lpWideCharStr, int cchWideChar) */
static void imp_MultiByteToWideChar(void)
{
    uint32_t cp = cp_resolve(W32_ARG(0)), flags = W32_ARG(1), src = W32_ARG(2), dst = W32_ARG(4);
    int32_t cb = (int32_t)W32_ARG(3), cch = (int32_t)W32_ARG(5);
    uint32_t r = 0;
    if (cp == 0 || src == 0 || cb == 0 || cb < -1 || cch < 0 || (cch > 0 && (dst == 0 || dst == src))) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        const uint8_t *s = guest_ptr(src);
        uint32_t n = cb == -1 ? (uint32_t)strlen((const char *)s) + 1 : (uint32_t)cb;
        int strict = (flags & MB_ERR_INVALID_CHARS) != 0;
        int64_t need = mb_to_wide(cp, s, n, NULL, 0, strict);
        if (need < 0)
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        else if (cch == 0)
            r = (uint32_t)need;
        else if (need > cch)
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        else
            r = (uint32_t)mb_to_wide(cp, s, n, guest_ptr(dst), (uint32_t)cch, strict);
    }
    W32_RETURN(6, r);
}

/* int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr,
 *                         int cchWideChar, LPSTR lpMultiByteStr, int cbMultiByte,
 *                         LPCSTR lpDefaultChar, LPBOOL lpUsedDefaultChar)
 * No best-fit mapping: a character with no byte becomes the default char. */
static void imp_WideCharToMultiByte(void)
{
    uint32_t cp = cp_resolve(W32_ARG(0)), src = W32_ARG(2), dst = W32_ARG(4);
    uint32_t def = W32_ARG(6), used_out = W32_ARG(7), r = 0;
    int32_t cch = (int32_t)W32_ARG(3), cb = (int32_t)W32_ARG(5);
    if (cp == 0 || src == 0 || cch == 0 || cch < -1 || cb < 0 || (cb > 0 && (dst == 0 || dst == src)) ||
        (cp == CP_UTF8 && (def != 0 || used_out != 0))) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        const uint8_t *s = guest_ptr(src);
        uint32_t n = cch == -1 ? wide_len(s) + 1 : (uint32_t)cch;
        uint8_t d = def != 0 ? MEM8(def) : '?';
        int used = 0;
        uint32_t need = wide_to_mb(cp, s, n, NULL, 0, d, &used);
        if (cb == 0)
            r = need;
        else if (need > (uint32_t)cb)
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        else
            r = wide_to_mb(cp, s, n, guest_ptr(dst), (uint32_t)cb, d, &used);
        if (used_out != 0)
            MEM32(used_out) = used ? W32_TRUE : W32_FALSE;
    }
    W32_RETURN(8, r);
}

/* ---- String comparison, mapping and types ------------------------------ */

#define NORM_IGNORECASE 0x1u
#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u
#define CT_CTYPE1 0x1u

static uint32_t unit_at(const uint8_t *s, uint32_t i, int wide)
{
    return wide ? get16(s, i) : s[i];
}

/* CompareStringA/W (LCID, flags, s1, cch1, s2, cch2): ordinal comparison of
 * code units, Latin-1 case folded under NORM_IGNORECASE; other flags are
 * ignored. A negative count means NUL-terminated. */
static uint32_t compare_string(int wide)
{
    uint32_t a = W32_ARG(2), b = W32_ARG(4);
    int32_t na = (int32_t)W32_ARG(3), nb = (int32_t)W32_ARG(5);
    if (a == 0 || b == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint8_t *s = guest_ptr(a), *t = guest_ptr(b);
    uint32_t la = na < 0 ? (wide ? wide_len(s) : (uint32_t)strlen((const char *)s)) : (uint32_t)na;
    uint32_t lb = nb < 0 ? (wide ? wide_len(t) : (uint32_t)strlen((const char *)t)) : (uint32_t)nb;
    int nocase = (W32_ARG(1) & NORM_IGNORECASE) != 0;
    for (uint32_t i = 0; i < la && i < lb; i++) {
        uint32_t x = unit_at(s, i, wide), y = unit_at(t, i, wide);
        if (nocase) {
            x = latin1_upper(x);
            y = latin1_upper(y);
        }
        if (x != y)
            return x < y ? 1 : 3; /* CSTR_LESS_THAN, CSTR_GREATER_THAN */
    }
    return la == lb ? 2 : la < lb ? 1 : 3;
}

/* LCMapStringA/W (LCID, flags, src, cchSrc, dst, cchDest): Latin-1 case
 * mapping only. */
static uint32_t lcmap_string(const char *func, int wide)
{
    uint32_t flags = W32_ARG(1), src = W32_ARG(2), dst = W32_ARG(4);
    int32_t ns = (int32_t)W32_ARG(3), nd = (int32_t)W32_ARG(5);
    if (flags != LCMAP_LOWERCASE && flags != LCMAP_UPPERCASE) {
        diag_unsupported(func, MEM32(g_esp), "flags 0x%X", flags);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (src == 0 || ns == 0 || ns < -1 || nd < 0 || (nd > 0 && dst == 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint8_t *s = guest_ptr(src);
    uint32_t n = ns == -1 ? (wide ? wide_len(s) : (uint32_t)strlen((const char *)s)) + 1 : (uint32_t)ns;
    if (nd == 0)
        return n;
    if (n > (uint32_t)nd) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    uint8_t *out = guest_ptr(dst);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = unit_at(s, i, wide);
        u = flags == LCMAP_UPPERCASE ? latin1_upper(u) : latin1_lower(u);
        if (wide)
            put16(out, i, u);
        else
            out[i] = (uint8_t)u;
    }
    return n;
}

/* GetStringTypeA/W core: CT_CTYPE1 for n units of src into the WORD array
 * at out. A strings are CP1252. */
static uint32_t string_type(const char *func, uint32_t type, uint32_t src, int32_t cch, uint32_t out,
                            int wide)
{
    if (type != CT_CTYPE1) {
        diag_unsupported(func, MEM32(g_esp), "info type 0x%X", type);
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (src == 0 || out == 0 || cch == 0 || cch < -1) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    const uint8_t *s = guest_ptr(src);
    uint32_t n = cch == -1 ? (wide ? wide_len(s) : (uint32_t)strlen((const char *)s)) + 1 : (uint32_t)cch;
    uint8_t *o = guest_ptr(out);
    for (uint32_t i = 0; i < n; i++)
        put16(o, i, ctype1(wide ? get16(s, i) : sbcs_to_wide(1252, s[i])));
    return W32_TRUE;
}

/* int CompareStringA(LCID, DWORD, LPCSTR, int, LPCSTR, int) */
static void imp_CompareStringA(void)
{
    W32_RETURN(6, compare_string(0));
}

/* int CompareStringW(LCID, DWORD, LPCWSTR, int, LPCWSTR, int) */
static void imp_CompareStringW(void)
{
    W32_RETURN(6, compare_string(1));
}

/* int LCMapStringA(LCID, DWORD, LPCSTR, int, LPSTR, int) */
static void imp_LCMapStringA(void)
{
    W32_RETURN(6, lcmap_string("KERNEL32.dll!LCMapStringA", 0));
}

/* int LCMapStringW(LCID, DWORD, LPCWSTR, int, LPWSTR, int) */
static void imp_LCMapStringW(void)
{
    W32_RETURN(6, lcmap_string("KERNEL32.dll!LCMapStringW", 1));
}

/* BOOL GetStringTypeA(LCID, DWORD dwInfoType, LPCSTR, int, LPWORD) */
static void imp_GetStringTypeA(void)
{
    W32_RETURN(5, string_type("KERNEL32.dll!GetStringTypeA", W32_ARG(1), W32_ARG(2),
                              (int32_t)W32_ARG(3), W32_ARG(4), 0));
}

/* BOOL GetStringTypeW(DWORD dwInfoType, LPCWSTR, int, LPWORD) */
static void imp_GetStringTypeW(void)
{
    W32_RETURN(4, string_type("KERNEL32.dll!GetStringTypeW", W32_ARG(0), W32_ARG(1),
                              (int32_t)W32_ARG(2), W32_ARG(3), 1));
}

/* ---- Locale imports ---------------------------------------------------- */

/* int GetLocaleInfoA(LCID, LCTYPE, LPSTR, int) */
static void imp_GetLocaleInfoA(void)
{
    W32_RETURN(4, locale_info("KERNEL32.dll!GetLocaleInfoA", 0));
}

/* int GetLocaleInfoW(LCID, LCTYPE, LPWSTR, int) */
static void imp_GetLocaleInfoW(void)
{
    W32_RETURN(4, locale_info("KERNEL32.dll!GetLocaleInfoW", 1));
}

/* LCID GetUserDefaultLCID(void) */
static void imp_GetUserDefaultLCID(void)
{
    mem_ready();
    W32_RETURN(0, g_langid); /* SORT_DEFAULT */
}

/* LANGID GetUserDefaultLangID(void) */
static void imp_GetUserDefaultLangID(void)
{
    mem_ready();
    W32_RETURN(0, g_langid);
}

/* BOOL IsValidLocale(LCID Locale, DWORD dwFlags) */
static void imp_IsValidLocale(void)
{
    mem_ready();
    uint32_t lcid = W32_ARG(0);
    W32_RETURN(2, lcid == 0x0409 || lcid == g_langid);
}

/* BOOL EnumSystemLocalesA(LOCALE_ENUMPROCA lpLocaleEnumProc, DWORD dwFlags)
 * Calls the stdcall callback once, for en-US. */
static void imp_EnumSystemLocalesA(void)
{
    uint32_t proc = W32_ARG(0), ok = W32_FALSE;
    if (proc == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
    } else {
        mem_ready();
        guest_call_cdecl(proc, 1, &g_locale_id);
        ok = W32_TRUE;
    }
    W32_RETURN(2, ok);
}

/* GEOID GetUserGeoID(GEOCLASS) */
static void imp_GetUserGeoID(void)
{
    W32_RETURN(1, 244); /* United States */
}

/* int GetDateFormatA(LCID, DWORD, const SYSTEMTIME *, LPCSTR, LPSTR, int) */
static void imp_GetDateFormatA(void)
{
    W32_RETURN(6, date_time_format("KERNEL32.dll!GetDateFormatA", 1));
}

/* int GetTimeFormatA(LCID, DWORD, const SYSTEMTIME *, LPCSTR, LPSTR, int) */
static void imp_GetTimeFormatA(void)
{
    W32_RETURN(6, date_time_format("KERNEL32.dll!GetTimeFormatA", 0));
}

/* ---- Process and version ----------------------------------------------- */

#define VER_PLATFORM_WIN32_NT 2u
#define VER_SUITE_SINGLEUSERTS 0x100u
#define VER_NT_WORKSTATION 1u

/* LPSTR GetCommandLineA(void) */
static void imp_GetCommandLineA(void)
{
    mem_ready();
    W32_RETURN(0, g_cmdline);
}

/* void GetStartupInfoA(LPSTARTUPINFOA) */
static void imp_GetStartupInfoA(void)
{
    uint32_t p = W32_ARG(0);
    if (p != 0) {
        memset(guest_ptr(p), 0, 68);
        MEM32(p) = 68; /* cb */
    }
    W32_RETURN(1, 0);
}

/* HANDLE GetCurrentProcess(void) */
static void imp_GetCurrentProcess(void)
{
    W32_RETURN(0, W32_PSEUDO_CURRENT_PROCESS);
}

/* DWORD GetCurrentProcessId(void) */
static void imp_GetCurrentProcessId(void)
{
    W32_RETURN(0, (uint32_t)getpid());
}

/* BOOL IsDebuggerPresent(void) */
static void imp_IsDebuggerPresent(void)
{
    W32_RETURN(0, W32_FALSE);
}

/* BOOL IsProcessorFeaturePresent(DWORD). The CRT finds it only through
 * GetProcAddress. Answers from the fixed CPU that recomp_cpuid reports (leaf
 * 1 EDX: FPU, TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2; ECX 0, so no SSE3). */
static void imp_IsProcessorFeaturePresent(void)
{
    switch (W32_ARG(0)) {
    case 2:  /* PF_COMPARE_EXCHANGE_DOUBLE (CX8) */
    case 3:  /* PF_MMX_INSTRUCTIONS_AVAILABLE */
    case 6:  /* PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE) */
    case 8:  /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 10: /* PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2) */
        W32_RETURN(1, W32_TRUE);
        return;
    default: /* 0 FDIV errata, 1 emulated FPU, 7 3DNow, 13 SSE3, ... */
        W32_RETURN(1, W32_FALSE);
        return;
    }
}

/* The Windows version every version API reports: 6.2 build 9200, no service
 * pack, what Windows 8.1 and later (and Wine) give an exe without a
 * compatibility manifest. The CRT branches on it: with major > 5,
 * it skips its Windows XP image check, as in the
 * Wine golden trace (notes/golden_trace.md). */
#define W32_VER_MAJOR 6u
#define W32_VER_MINOR 2u
#define W32_VER_BUILD 9200u

/* BOOL GetVersionExA(LPOSVERSIONINFOA) */
static void imp_GetVersionExA(void)
{
    uint32_t p = W32_ARG(0), size = p != 0 ? MEM32(p) : 0, ok = W32_FALSE;
    if (size != 148 && size != 156) { /* OSVERSIONINFOA, OSVERSIONINFOEXA */
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
    } else {
        memset(guest_ptr(p + 4), 0, size - 4);
        MEM32(p + 4) = W32_VER_MAJOR;  /* dwMajorVersion */
        MEM32(p + 8) = W32_VER_MINOR;  /* dwMinorVersion */
        MEM32(p + 12) = W32_VER_BUILD; /* dwBuildNumber */
        MEM32(p + 16) = VER_PLATFORM_WIN32_NT;
        /* szCSDVersion "" and wServicePackMajor/Minor 0: the memset. */
        if (size == 156) {
            MEM16(p + 152) = VER_SUITE_SINGLEUSERTS;
            MEM8(p + 154) = VER_NT_WORKSTATION;
        }
        ok = W32_TRUE;
    }
    W32_RETURN(1, ok);
}

/* ULONGLONG VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask,
 *                               BYTE Condition)
 * Each VER_* type bit k owns the 3-bit field at 3*k; with several bits set,
 * the highest one is used, as ntdll does. */
static void imp_VerSetConditionMask(void)
{
    uint64_t mask = W32_ARG(0) | (uint64_t)W32_ARG(1) << 32;
    uint32_t type = W32_ARG(2) & 0xFF, cond = W32_ARG(3) & 7;
    if (type != 0 && cond != 0)
        mask |= (uint64_t)cond << (3 * (31 - __builtin_clz(type)));
    g_edx = (uint32_t)(mask >> 32);
    W32_RETURN(4, (uint32_t)mask);
}

/* 1 when ours <cond> want holds (VER_EQUAL .. VER_LESS_EQUAL). */
static int ver_holds(uint32_t ours, uint32_t want, uint32_t cond)
{
    switch (cond) {
    case 1:
        return ours == want;
    case 2:
        return ours > want;
    case 3:
        return ours >= want;
    case 4:
        return ours < want;
    case 5:
        return ours <= want;
    default:
        return 0;
    }
}

/* BOOL VerifyVersionInfoA(LPOSVERSIONINFOEXA, DWORD dwTypeMask,
 *                         DWORDLONG dwlConditionMask), against W32_VER_*. */
static void imp_VerifyVersionInfoA(void)
{
    uint32_t p = W32_ARG(0), type = W32_ARG(1), ok = W32_FALSE;
    uint64_t mask = W32_ARG(2) | (uint64_t)W32_ARG(3) << 32;
#define VER_COND(bit) ((uint32_t)(mask >> (3 * __builtin_ctz(bit))) & 7)
    /* Major, minor, service pack major and minor compare as one version
     * number: the first of them that differs decides. */
    static const struct {
        uint32_t bit, off, wide, ours;
    } k_order[4] = {{0x02, 4, 1, W32_VER_MAJOR}, {0x01, 8, 1, W32_VER_MINOR},
                    {0x20, 148, 0, 0}, {0x10, 150, 0, 0}};
    int valid = p != 0 && type != 0 && mask != 0, pass = valid;
    for (int i = 0; i < 4 && pass; i++) {
        if (!(type & k_order[i].bit))
            continue;
        uint32_t want = k_order[i].wide ? MEM32(p + k_order[i].off) : MEM16(p + k_order[i].off);
        pass = ver_holds(k_order[i].ours, want, VER_COND(k_order[i].bit));
        if (k_order[i].ours != want)
            break;
    }
    if (pass && (type & 0x04)) /* VER_BUILDNUMBER */
        pass = ver_holds(W32_VER_BUILD, MEM32(p + 12), VER_COND(0x04));
    if (pass && (type & 0x08)) /* VER_PLATFORMID */
        pass = ver_holds(VER_PLATFORM_WIN32_NT, MEM32(p + 16), VER_COND(0x08));
    if (pass && (type & 0x80)) /* VER_PRODUCT_TYPE */
        pass = ver_holds(VER_NT_WORKSTATION, MEM8(p + 154), VER_COND(0x80));
    if (pass && (type & 0x40)) { /* VER_SUITENAME: VER_AND 6, VER_OR 7 */
        uint32_t want = MEM16(p + 152), cond = VER_COND(0x40);
        pass = cond == 6   ? (VER_SUITE_SINGLEUSERTS & want) == want
               : cond == 7 ? (VER_SUITE_SINGLEUSERTS & want) != 0
                           : 0;
    }
#undef VER_COND
    if (pass)
        ok = W32_TRUE;
    else
        w32_set_last_error(valid ? W32_ERROR_OLD_WIN_VERSION : W32_ERROR_INVALID_PARAMETER);
    W32_RETURN(4, ok);
}

/* ---- Exit, debug output, exceptions ------------------------------------ */

/* void ExitProcess(UINT uExitCode) */
static void imp_ExitProcess(void)
{
    fflush(NULL);
    exit((int)W32_ARG(0));
    W32_RETURN(1, 0);
}

/* BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode) */
static void imp_TerminateProcess(void)
{
    if (W32_ARG(0) == W32_PSEUDO_CURRENT_PROCESS) {
        fflush(NULL);
        exit((int)W32_ARG(1));
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    W32_RETURN(2, W32_FALSE);
}

/* BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) */
static void imp_GetExitCodeProcess(void)
{
    uint32_t ok = W32_FALSE;
    if (W32_ARG(0) == W32_PSEUDO_CURRENT_PROCESS && W32_ARG(1) != 0) {
        MEM32(W32_ARG(1)) = W32_STILL_ACTIVE;
        ok = W32_TRUE;
    } else {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    }
    W32_RETURN(2, ok);
}

/* BOOL CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD) */
static void imp_CreatePipe(void)
{
    diag_unsupported("KERNEL32.dll!CreatePipe", MEM32(g_esp), "pipes are not supported");
    w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    W32_RETURN(4, W32_FALSE);
}

/* BOOL CreateProcessA(10 arguments) */
static void imp_CreateProcessA(void)
{
    diag_unsupported("KERNEL32.dll!CreateProcessA", MEM32(g_esp), "child processes are not supported");
    w32_set_last_error(W32_ERROR_ACCESS_DENIED);
    W32_RETURN(10, W32_FALSE);
}

/* void OutputDebugStringA(LPCSTR) */
static void imp_OutputDebugStringA(void)
{
    if (W32_ARG(0) != 0) {
        const char *s = guest_ptr(W32_ARG(0));
        size_t n = strlen(s);
        fprintf(stderr, "ODS: %s%s", s, n > 0 && s[n - 1] == '\n' ? "" : "\n");
    }
    W32_RETURN(1, 0);
}

/* void OutputDebugStringW(LPCWSTR) */
static void imp_OutputDebugStringW(void)
{
    if (W32_ARG(0) != 0) {
        const uint8_t *s = guest_ptr(W32_ARG(0));
        uint32_t n = wide_len(s);
        int used = 0;
        uint32_t len = wide_to_mb(CP_UTF8, s, n, NULL, 0, 0, &used);
        char *text = malloc(len + 1);
        if (text != NULL) {
            wide_to_mb(CP_UTF8, s, n, (uint8_t *)text, len, 0, &used);
            text[len] = '\0';
            fprintf(stderr, "ODS: %s%s", text, len > 0 && text[len - 1] == '\n' ? "" : "\n");
            free(text);
        }
    }
    W32_RETURN(1, 0);
}

/* LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER)
 * Stored only: there is no exception dispatch to call it. */
static void imp_SetUnhandledExceptionFilter(void)
{
    W32_RETURN(1, atomic_exchange(&g_unhandled_filter, W32_ARG(0)));
}

/* LONG UnhandledExceptionFilter(EXCEPTION_POINTERS *) */
static void imp_UnhandledExceptionFilter(void)
{
    uint32_t ep = W32_ARG(0), rec = ep != 0 ? MEM32(ep) : 0;
    if (rec != 0)
        diag_unsupported("KERNEL32.dll!UnhandledExceptionFilter", MEM32(g_esp),
                         "exception 0x%08X at 0x%08X", MEM32(rec), MEM32(rec + 12));
    else
        diag_unsupported("KERNEL32.dll!UnhandledExceptionFilter", MEM32(g_esp), "no exception record");
    abort();
    W32_RETURN(1, 0);
}

/* void DebugBreak(void) */
static void imp_DebugBreak(void)
{
    diag_unsupported("KERNEL32.dll!DebugBreak", MEM32(g_esp), "breakpoint");
    abort();
    W32_RETURN(0, 0);
}

/* void FatalAppExitA(UINT uAction, LPCSTR lpMessageText) */
static void imp_FatalAppExitA(void)
{
    const char *msg = W32_ARG(1) != 0 ? guest_ptr(W32_ARG(1)) : "";
    diag_unsupported("KERNEL32.dll!FatalAppExitA", MEM32(g_esp), "%s", msg);
    abort();
    W32_RETURN(2, 0);
}

/* ---- Export table ------------------------------------------------------ */

#define K32(name) {"KERNEL32.dll", #name, 0, imp_##name}

const win32_export_t win32_exports_mem[] = {
    K32(CompareStringA),
    K32(CompareStringW),
    K32(CreatePipe),
    K32(CreateProcessA),
    K32(DebugBreak),
    K32(EnumSystemLocalesA),
    K32(ExitProcess),
    K32(ExpandEnvironmentStringsA),
    K32(FatalAppExitA),
    K32(FreeEnvironmentStringsA),
    K32(FreeEnvironmentStringsW),
    K32(GetACP),
    K32(GetCPInfo),
    K32(GetCommandLineA),
    K32(GetCurrentProcess),
    K32(GetCurrentProcessId),
    K32(GetDateFormatA),
    K32(GetEnvironmentStrings),
    K32(GetEnvironmentStringsW),
    K32(GetExitCodeProcess),
    K32(GetLocaleInfoA),
    K32(GetLocaleInfoW),
    K32(GetOEMCP),
    K32(GetProcessHeap),
    K32(GetStartupInfoA),
    K32(GetStringTypeA),
    K32(GetStringTypeW),
    K32(GetTimeFormatA),
    K32(GetUserDefaultLCID),
    K32(GetUserDefaultLangID),
    K32(GetUserGeoID),
    K32(GetVersionExA),
    K32(HeapAlloc),
    K32(HeapCreate),
    K32(HeapDestroy),
    K32(HeapFree),
    K32(HeapReAlloc),
    K32(HeapSize),
    K32(IsDebuggerPresent),
    K32(IsProcessorFeaturePresent),
    K32(IsValidCodePage),
    K32(IsValidLocale),
    K32(LCMapStringA),
    K32(LCMapStringW),
    K32(MultiByteToWideChar),
    K32(OutputDebugStringA),
    K32(OutputDebugStringW),
    K32(SetEnvironmentVariableA),
    K32(SetUnhandledExceptionFilter),
    K32(TerminateProcess),
    K32(UnhandledExceptionFilter),
    K32(VerSetConditionMask),
    K32(VerifyVersionInfoA),
    K32(VirtualAlloc),
    K32(VirtualFree),
    K32(VirtualQuery),
    K32(WideCharToMultiByte),
    {0},
};
