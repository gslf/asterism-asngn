/*
 * os_common.c — portable pieces of the platform shim (os.h).
 *
 * Owns: os_path_join, os_read_file, os_write_file, and — only when
 * ASNGN_NO_THREADS is defined — the no-op locking/threading set. The
 * platform backends (os_posix.c / os_win32.c) compile their threading
 * sections only when ASNGN_NO_THREADS is NOT defined, so exactly one
 * definition set exists per build. os_sleep_ms always lives in the
 * platform backends: it is a real sleep even without threads.
 *
 * MIT License — per aspera ad astra.
 */

#include "os.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---- path join ---------------------------------------------------------- */

static char *os_dup_cstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

char *os_path_join(const char *a, const char *b)
{
    size_t la, lb;
    int need_sep;
    char *s;

    if (a == NULL || *a == '\0')
        return os_dup_cstr(b != NULL ? b : "");
    if (b == NULL || *b == '\0')
        return os_dup_cstr(a);

    la = strlen(a);
    lb = strlen(b);
    need_sep = (a[la - 1] != '/' && a[la - 1] != '\\') ? 1 : 0;

    s = (char *)malloc(la + (size_t)need_sep + lb + 1);
    if (s == NULL)
        return NULL;
    memcpy(s, a, la);
    if (need_sep)
        s[la] = '/';
    memcpy(s + la + (size_t)need_sep, b, lb + 1);
    return s;
}

/* ---- whole-file read / write -------------------------------------------- */

asngn_err os_read_file(const char *path, char **out, size_t *out_len)
{
    FILE *f;
    char *buf, *shrunk;
    size_t cap, len;

    if (path == NULL || out == NULL)
        return ASNGN_ERR_INVALID;
    *out = NULL;
    if (out_len != NULL)
        *out_len = 0;

    errno = 0;
    f = os_fopen(path, "rb");
    if (f == NULL)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;

    cap = 4096;
    len = 0;
    buf = (char *)malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return ASNGN_ERR_NOMEM;
    }

    for (;;) {
        size_t want = cap - len - 1;
        size_t got;

        if (want == 0) {
            char *nb;
            if (cap > (size_t)-1 / 2) {
                free(buf);
                fclose(f);
                return ASNGN_ERR_NOMEM;
            }
            cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                fclose(f);
                return ASNGN_ERR_NOMEM;
            }
            buf = nb;
            continue;
        }

        got = fread(buf + len, 1, want, f);
        len += got;
        if (got < want) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                return ASNGN_ERR_IO;
            }
            break; /* EOF */
        }
    }

    fclose(f);
    buf[len] = '\0';
    shrunk = (char *)realloc(buf, len + 1);
    if (shrunk != NULL)
        buf = shrunk;
    *out = buf;
    if (out_len != NULL)
        *out_len = len;
    return ASNGN_OK;
}

asngn_err os_write_file(const char *path, const void *data, size_t len)
{
    FILE *f;
    asngn_err e = ASNGN_OK;

    if (path == NULL || (data == NULL && len > 0))
        return ASNGN_ERR_INVALID;

    f = os_fopen(path, "wb");
    if (f == NULL)
        return ASNGN_ERR_IO;

    if (len > 0 && fwrite(data, 1, len, f) != len)
        e = ASNGN_ERR_IO;
    /* Durability before a possible os_file_replace: sync while still open. */
    if (e == ASNGN_OK)
        e = os_fsync(f);
    if (fclose(f) != 0 && e == ASNGN_OK)
        e = ASNGN_ERR_IO;
    return e;
}

/* ---- ASNGN_NO_THREADS: no-op locking, no worker thread ------------------ */

#if defined(ASNGN_NO_THREADS)

asngn_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg)
{
    (void)t;
    (void)fn;
    (void)arg;
    return ASNGN_ERR_INVALID;
}

void os_thread_join(os_thread *t) { (void)t; }

void os_mutex_init(os_mutex *m) { (void)m; }
void os_mutex_destroy(os_mutex *m) { (void)m; }
void os_mutex_lock(os_mutex *m) { (void)m; }
void os_mutex_unlock(os_mutex *m) { (void)m; }

void os_cond_init(os_cond *c) { (void)c; }
void os_cond_destroy(os_cond *c) { (void)c; }

int os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms)
{
    /* Single-threaded build: nothing can ever signal — report timeout. */
    (void)c;
    (void)m;
    (void)timeout_ms;
    return 0;
}

void os_cond_wait(os_cond *c, os_mutex *m)
{
    (void)c;
    (void)m;
}

void os_cond_signal(os_cond *c) { (void)c; }
void os_cond_broadcast(os_cond *c) { (void)c; }

void os_rwlock_init(os_rwlock *l) { (void)l; }
void os_rwlock_destroy(os_rwlock *l) { (void)l; }
void os_rwlock_rdlock(os_rwlock *l) { (void)l; }
void os_rwlock_rdunlock(os_rwlock *l) { (void)l; }
void os_rwlock_wrlock(os_rwlock *l) { (void)l; }
void os_rwlock_wrunlock(os_rwlock *l) { (void)l; }

#endif /* ASNGN_NO_THREADS */
