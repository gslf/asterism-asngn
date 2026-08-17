/*
 * os_win32.c — Win32 backend of the platform shim (os.h).
 *
 * Threads: CreateThread; locks: SRWLOCK (mutex = exclusive-only, rwlock =
 * shared/exclusive); condition variables: CONDITION_VARIABLE over SRWLOCK.
 * Atomic replace: ReplaceFileW with MoveFileExW fallback when the target
 * does not exist yet. All UTF-8 paths convert to UTF-16 at this boundary.
 * RNG: rand_s (CSPRNG-backed in the CRT) with an xorshift fallback.
 * Threading section compiles out under ASNGN_NO_THREADS (os_common.c then
 * provides the no-op set).
 *
 * MIT License — per aspera ad astra.
 */

#if defined(_WIN32)

#define _CRT_RAND_S
#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10 */
#endif

#include <windows.h>

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wchar.h>

#include "os.h"

/* ---- UTF-8 <-> UTF-16 --------------------------------------------------- */

static wchar_t *os_u8_to_wide(const char *s)
{
    int n;
    wchar_t *w;

    if (s == NULL)
        return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static char *os_wide_to_u8(const wchar_t *w)
{
    int n;
    char *s;

    if (w == NULL)
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    s = (char *)malloc((size_t)n);
    if (s == NULL)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        return NULL;
    }
    return s;
}

/* ---- threads ------------------------------------------------------------ */

#if !defined(ASNGN_NO_THREADS)

/* os.h stores each primitive in a single void*; verify that fits. */
typedef char os_assert_srwlock_fits[(sizeof(SRWLOCK) <=
                                     sizeof(((os_mutex *)0)->h)) ? 1 : -1];
typedef char os_assert_condvar_fits[(sizeof(CONDITION_VARIABLE) <=
                                     sizeof(((os_cond *)0)->h)) ? 1 : -1];
typedef char os_assert_handle_fits[(sizeof(HANDLE) <=
                                    sizeof(((os_thread *)0)->h)) ? 1 : -1];

typedef struct {
    void *(*fn)(void *);
    void *arg;
} os_thread_tramp;

static DWORD WINAPI os_thread_main(LPVOID p)
{
    os_thread_tramp t = *(os_thread_tramp *)p;
    free(p);
    (void)t.fn(t.arg);
    return 0;
}

asngn_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg)
{
    os_thread_tramp *tp;
    HANDLE h;

    if (t == NULL || fn == NULL)
        return ASNGN_ERR_INVALID;
    t->h = NULL;
    tp = (os_thread_tramp *)malloc(sizeof(*tp));
    if (tp == NULL)
        return ASNGN_ERR_NOMEM;
    tp->fn = fn;
    tp->arg = arg;
    h = CreateThread(NULL, 0, os_thread_main, tp, 0, NULL);
    if (h == NULL) {
        free(tp);
        return ASNGN_ERR_BUSY;
    }
    t->h = (void *)h;
    return ASNGN_OK;
}

void os_thread_join(os_thread *t)
{
    if (t != NULL && t->h != NULL) {
        WaitForSingleObject((HANDLE)t->h, INFINITE);
        CloseHandle((HANDLE)t->h);
        t->h = NULL;
    }
}

void os_mutex_init(os_mutex *m) { InitializeSRWLock((PSRWLOCK)&m->h); }
void os_mutex_destroy(os_mutex *m) { (void)m; /* SRW locks are static */ }
void os_mutex_lock(os_mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->h); }
void os_mutex_unlock(os_mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->h); }

void os_cond_init(os_cond *c)
{
    InitializeConditionVariable((PCONDITION_VARIABLE)&c->h);
}

void os_cond_destroy(os_cond *c) { (void)c; }

int os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms)
{
    DWORD ms;

    if (timeout_ms < 0)
        ms = INFINITE;
    else if (timeout_ms > (int64_t)0x7FFFFFFE)
        ms = (DWORD)0x7FFFFFFE;
    else
        ms = (DWORD)timeout_ms;

    if (!SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->h,
                                   (PSRWLOCK)&m->h, ms, 0))
        return 0; /* timeout (or spurious failure: caller re-checks) */
    return 1;
}

void os_cond_wait(os_cond *c, os_mutex *m)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->h, (PSRWLOCK)&m->h,
                              INFINITE, 0);
}

void os_cond_signal(os_cond *c)
{
    WakeConditionVariable((PCONDITION_VARIABLE)&c->h);
}

void os_cond_broadcast(os_cond *c)
{
    WakeAllConditionVariable((PCONDITION_VARIABLE)&c->h);
}

void os_rwlock_init(os_rwlock *l) { InitializeSRWLock((PSRWLOCK)&l->h); }
void os_rwlock_destroy(os_rwlock *l) { (void)l; }
void os_rwlock_rdlock(os_rwlock *l) { AcquireSRWLockShared((PSRWLOCK)&l->h); }
void os_rwlock_rdunlock(os_rwlock *l) { ReleaseSRWLockShared((PSRWLOCK)&l->h); }
void os_rwlock_wrlock(os_rwlock *l) { AcquireSRWLockExclusive((PSRWLOCK)&l->h); }
void os_rwlock_wrunlock(os_rwlock *l)
{
    ReleaseSRWLockExclusive((PSRWLOCK)&l->h);
}

#endif /* !ASNGN_NO_THREADS */

/* ---- filesystem --------------------------------------------------------- */

asngn_err os_file_replace(const char *src, const char *dst)
{
    wchar_t *ws, *wd;
    asngn_err e = ASNGN_OK;

    if (src == NULL || dst == NULL)
        return ASNGN_ERR_INVALID;
    ws = os_u8_to_wide(src);
    wd = os_u8_to_wide(dst);
    if (ws == NULL || wd == NULL) {
        free(ws);
        free(wd);
        return ASNGN_ERR_NOMEM;
    }

    if (!ReplaceFileW(wd, ws, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS,
                      NULL, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            /* Target missing: plain move is atomic enough here. */
            if (!MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING))
                e = ASNGN_ERR_IO;
        } else {
            e = ASNGN_ERR_IO;
        }
    }
    free(ws);
    free(wd);
    return e;
}

asngn_err os_rename(const char *src, const char *dst)
{
    wchar_t *ws, *wd;
    asngn_err e = ASNGN_OK;

    if (src == NULL || dst == NULL)
        return ASNGN_ERR_INVALID;
    ws = os_u8_to_wide(src);
    wd = os_u8_to_wide(dst);
    if (ws == NULL || wd == NULL) {
        free(ws);
        free(wd);
        return ASNGN_ERR_NOMEM;
    }
    if (!MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        e = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                ? ASNGN_ERR_NOT_FOUND
                : ASNGN_ERR_IO;
    }
    free(ws);
    free(wd);
    return e;
}

asngn_err os_fsync(FILE *f)
{
    if (f == NULL)
        return ASNGN_ERR_INVALID;
    if (fflush(f) != 0)
        return ASNGN_ERR_IO;
    if (_commit(_fileno(f)) != 0)
        return ASNGN_ERR_IO;
    return ASNGN_OK;
}

asngn_err os_mkdir_p(const char *path)
{
    wchar_t *w;
    size_t i, start, len;
    DWORD attrs;
    asngn_err e = ASNGN_OK;

    if (path == NULL || *path == '\0')
        return ASNGN_ERR_INVALID;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return ASNGN_ERR_NOMEM;

    len = wcslen(w);
    while (len > 1 && (w[len - 1] == L'/' || w[len - 1] == L'\\'))
        w[--len] = L'\0';

    /* Never CreateDirectory a bare drive ("C:") or the "\\?\ / \\server"
     * lead-in; component errors are resolved by the final check. */
    start = 0;
    if (len >= 2 && w[1] == L':')
        start = 2;
    else if (len >= 2 && (w[0] == L'\\' || w[0] == L'/') &&
             (w[1] == L'\\' || w[1] == L'/'))
        start = 2;

    for (i = start + 1; i <= len; i++) {
        if (w[i] == L'/' || w[i] == L'\\' || w[i] == L'\0') {
            wchar_t saved = w[i];
            w[i] = L'\0';
            CreateDirectoryW(w, NULL); /* errors checked below */
            w[i] = saved;
            if (saved == L'\0')
                break;
        }
    }

    attrs = GetFileAttributesW(w);
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
        e = ASNGN_ERR_IO;
    free(w);
    return e;
}

int os_file_exists(const char *path)
{
    wchar_t *w;
    DWORD attrs;

    if (path == NULL)
        return 0;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return 0;
    attrs = GetFileAttributesW(w);
    free(w);
    return attrs != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

asngn_err os_remove_file(const char *path)
{
    wchar_t *w;
    asngn_err e = ASNGN_OK;

    if (path == NULL)
        return ASNGN_ERR_INVALID;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return ASNGN_ERR_NOMEM;
    if (!DeleteFileW(w)) {
        DWORD err = GetLastError();
        e = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                ? ASNGN_ERR_NOT_FOUND
                : ASNGN_ERR_IO;
    }
    free(w);
    return e;
}

asngn_err os_remove_dir(const char *path)
{
    wchar_t *w;
    asngn_err e = ASNGN_OK;

    if (path == NULL)
        return ASNGN_ERR_INVALID;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return ASNGN_ERR_NOMEM;
    if (!RemoveDirectoryW(w)) {
        DWORD err = GetLastError();
        e = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                ? ASNGN_ERR_NOT_FOUND
                : ASNGN_ERR_IO;
    }
    free(w);
    return e;
}

asngn_err os_truncate(const char *path, uint64_t size)
{
    wchar_t *w;
    int fd, rc;
    asngn_err e = ASNGN_OK;

    if (path == NULL)
        return ASNGN_ERR_INVALID;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return ASNGN_ERR_NOMEM;
    fd = _wopen(w, _O_WRONLY | _O_BINARY);
    free(w);
    if (fd < 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    rc = _chsize_s(fd, (long long)size);
    if (rc != 0)
        e = ASNGN_ERR_IO;
    if (_close(fd) != 0 && e == ASNGN_OK)
        e = ASNGN_ERR_IO;
    return e;
}

asngn_err os_file_size(const char *path, uint64_t *out)
{
    wchar_t *w;
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (path == NULL || out == NULL)
        return ASNGN_ERR_INVALID;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return ASNGN_ERR_NOMEM;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        free(w);
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                   ? ASNGN_ERR_NOT_FOUND
                   : ASNGN_ERR_IO;
    }
    free(w);
    *out = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
    return ASNGN_OK;
}

static void os_free_names(char **names, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static asngn_err os_list_kind(const char *path, int want_dirs,
                              char ***out_names, size_t *out_n)
{
    wchar_t *wpath, *pattern;
    size_t plen;
    HANDLE h;
    WIN32_FIND_DATAW fd;
    char **names = NULL;
    size_t n = 0, cap = 0;

    if (path == NULL || out_names == NULL || out_n == NULL)
        return ASNGN_ERR_INVALID;
    *out_names = NULL;
    *out_n = 0;

    wpath = os_u8_to_wide(path);
    if (wpath == NULL)
        return ASNGN_ERR_NOMEM;
    plen = wcslen(wpath);
    while (plen > 1 && (wpath[plen - 1] == L'/' || wpath[plen - 1] == L'\\'))
        wpath[--plen] = L'\0';

    pattern = (wchar_t *)malloc((plen + 3) * sizeof(wchar_t));
    if (pattern == NULL) {
        free(wpath);
        return ASNGN_ERR_NOMEM;
    }
    memcpy(pattern, wpath, plen * sizeof(wchar_t));
    pattern[plen] = L'\\';
    pattern[plen + 1] = L'*';
    pattern[plen + 2] = L'\0';
    free(wpath);

    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_NO_MORE_FILES)
            return ASNGN_OK; /* missing dir => 0 entries */
        return ASNGN_ERR_IO;
    }

    do {
        char *name;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        if (want_dirs != is_dir)
            continue;
        if (is_dir && (wcscmp(fd.cFileName, L".") == 0 ||
                       wcscmp(fd.cFileName, L"..") == 0))
            continue;
        name = os_wide_to_u8(fd.cFileName);
        if (name == NULL) {
            FindClose(h);
            os_free_names(names, n);
            return ASNGN_ERR_NOMEM;
        }
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            char **nn = (char **)realloc(names, ncap * sizeof(*nn));
            if (nn == NULL) {
                free(name);
                FindClose(h);
                os_free_names(names, n);
                return ASNGN_ERR_NOMEM;
            }
            names = nn;
            cap = ncap;
        }
        names[n++] = name;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    *out_names = names;
    *out_n = n;
    return ASNGN_OK;
}

FILE *os_fopen(const char *path, const char *mode)
{
    wchar_t *wp;
    wchar_t wmode[16];
    size_t i;
    FILE *f;

    if (path == NULL || mode == NULL)
        return NULL;
    wp = os_u8_to_wide(path);
    if (wp == NULL)
        return NULL;
    for (i = 0; mode[i] != '\0' && i < 15; i++)
        wmode[i] = (wchar_t)(unsigned char)mode[i];
    wmode[i] = L'\0';
    f = _wfopen(wp, wmode);
    free(wp);
    return f;
}

char *os_realpath(const char *path)
{
    wchar_t *w, *full;
    DWORD need, got;
    char *out;

    if (path == NULL)
        return NULL;
    w = os_u8_to_wide(path);
    if (w == NULL)
        return NULL;
    need = GetFullPathNameW(w, 0, NULL, NULL); /* required size incl. NUL */
    if (need == 0) {
        free(w);
        return NULL;
    }
    full = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (full == NULL) {
        free(w);
        return NULL;
    }
    got = GetFullPathNameW(w, need, full, NULL);
    free(w);
    if (got == 0 || got >= need) {
        free(full);
        return NULL;
    }
    out = os_wide_to_u8(full);
    free(full);
    return out;
}

/* ---- misc --------------------------------------------------------------- */

int64_t os_now_unix(void)
{
    FILETIME ft;
    uint64_t ticks;

    GetSystemTimePreciseAsFileTime(&ft);
    ticks = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    /* 100 ns ticks since 1601-01-01 -> unix seconds. */
    return (int64_t)((ticks - 116444736000000000ULL) / 10000000ULL);
}

int64_t os_monotonic_ms(void)
{
    static LONGLONG freq = 0;
    LARGE_INTEGER c;
    LONGLONG f;

    if (freq == 0) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q); /* constant per boot; race benign */
        freq = q.QuadPart;
    }
    f = freq;
    if (f <= 0)
        return 0;
    QueryPerformanceCounter(&c);
    return (int64_t)((c.QuadPart / f) * 1000 + ((c.QuadPart % f) * 1000) / f);
}

void os_sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    Sleep((DWORD)ms);
}

void os_random_bytes(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t i = 0;

    if (buf == NULL || n == 0)
        return;
    while (i < n) {
        unsigned int v;
        size_t take;

        if (rand_s(&v) != 0)
            break;
        take = (n - i < sizeof(v)) ? n - i : sizeof(v);
        memcpy(p + i, &v, take);
        i += take;
    }
    if (i < n) {
        /* Best-effort fallback: xorshift64 seeded from clock + pid. */
        LARGE_INTEGER q;
        uint64_t s;

        QueryPerformanceCounter(&q);
        s = (uint64_t)q.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^
            (uint64_t)(uintptr_t)buf;
        if (s == 0)
            s = 0x9e3779b97f4a7c15ULL;
        for (; i < n; i++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            p[i] = (unsigned char)(s & 0xff);
        }
    }
}

int os_hardware_threads(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

#else /* !_WIN32 */

typedef int asngn_os_win32_unused_on_posix; /* non-empty translation unit */

#endif /* _WIN32 */
