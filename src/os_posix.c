/*
 * os_posix.c — POSIX backend of the platform shim (os.h).
 *
 * Threads via pthreads (compiled out under ASNGN_NO_THREADS — os_common.c
 * then provides the no-op set). Atomic replace via rename(2). Compiles on
 * Linux and macOS/BSD; the only divergence is the CSPRNG (arc4random_buf
 * where available, /dev/urandom elsewhere, seeded rand() as last resort).
 *
 * MIT License — per aspera ad astra.
 */

#if !defined(_WIN32)

/* Feature-test macros must precede every include. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "os.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- threads ------------------------------------------------------------ */

#if !defined(ASNGN_NO_THREADS)

asngn_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg)
{
    if (t == NULL || fn == NULL)
        return ASNGN_ERR_INVALID;
    t->valid = 0;
    if (pthread_create(&t->t, NULL, fn, arg) != 0)
        return ASNGN_ERR_BUSY;
    t->valid = 1;
    return ASNGN_OK;
}

void os_thread_join(os_thread *t)
{
    if (t != NULL && t->valid) {
        pthread_join(t->t, NULL);
        t->valid = 0;
    }
}

void os_mutex_init(os_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void os_mutex_destroy(os_mutex *m) { pthread_mutex_destroy(&m->m); }
void os_mutex_lock(os_mutex *m) { pthread_mutex_lock(&m->m); }
void os_mutex_unlock(os_mutex *m) { pthread_mutex_unlock(&m->m); }

void os_cond_init(os_cond *c) { pthread_cond_init(&c->c, NULL); }
void os_cond_destroy(os_cond *c) { pthread_cond_destroy(&c->c); }

int os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms)
{
    struct timespec ts;

    if (timeout_ms < 0) {
        pthread_cond_wait(&c->c, &m->m);
        return 1;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == ETIMEDOUT ? 0 : 1;
}

void os_cond_wait(os_cond *c, os_mutex *m)
{
    pthread_cond_wait(&c->c, &m->m);
}

void os_cond_signal(os_cond *c) { pthread_cond_signal(&c->c); }
void os_cond_broadcast(os_cond *c) { pthread_cond_broadcast(&c->c); }

void os_rwlock_init(os_rwlock *l) { pthread_rwlock_init(&l->l, NULL); }
void os_rwlock_destroy(os_rwlock *l) { pthread_rwlock_destroy(&l->l); }
void os_rwlock_rdlock(os_rwlock *l) { pthread_rwlock_rdlock(&l->l); }
void os_rwlock_rdunlock(os_rwlock *l) { pthread_rwlock_unlock(&l->l); }
void os_rwlock_wrlock(os_rwlock *l) { pthread_rwlock_wrlock(&l->l); }
void os_rwlock_wrunlock(os_rwlock *l) { pthread_rwlock_unlock(&l->l); }

#endif /* !ASNGN_NO_THREADS */

/* ---- filesystem --------------------------------------------------------- */

asngn_err os_file_replace(const char *src, const char *dst)
{
    if (src == NULL || dst == NULL)
        return ASNGN_ERR_INVALID;
    if (rename(src, dst) != 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    return ASNGN_OK;
}

asngn_err os_rename(const char *src, const char *dst)
{
    /* On POSIX rename(2) already replaces the target atomically. */
    return os_file_replace(src, dst);
}

asngn_err os_fsync(FILE *f)
{
    if (f == NULL)
        return ASNGN_ERR_INVALID;
    if (fflush(f) != 0)
        return ASNGN_ERR_IO;
    if (fsync(fileno(f)) != 0)
        return ASNGN_ERR_IO;
    return ASNGN_OK;
}

asngn_err os_mkdir_p(const char *path)
{
    char *tmp;
    size_t i, n;
    struct stat st;

    if (path == NULL || *path == '\0')
        return ASNGN_ERR_INVALID;

    n = strlen(path);
    tmp = (char *)malloc(n + 1);
    if (tmp == NULL)
        return ASNGN_ERR_NOMEM;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';

    for (i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST && errno != EISDIR) {
                free(tmp);
                return ASNGN_ERR_IO;
            }
            tmp[i] = saved;
            if (saved == '\0')
                break;
        }
    }
    free(tmp);

    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return ASNGN_ERR_IO;
    return ASNGN_OK;
}

int os_file_exists(const char *path)
{
    struct stat st;
    if (path == NULL)
        return 0;
    return stat(path, &st) == 0 ? 1 : 0;
}

asngn_err os_remove_file(const char *path)
{
    if (path == NULL)
        return ASNGN_ERR_INVALID;
    if (unlink(path) != 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    return ASNGN_OK;
}

asngn_err os_remove_dir(const char *path)
{
    if (path == NULL)
        return ASNGN_ERR_INVALID;
    if (rmdir(path) != 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    return ASNGN_OK;
}

asngn_err os_truncate(const char *path, uint64_t size)
{
    int fd;
    asngn_err e = ASNGN_OK;

    if (path == NULL)
        return ASNGN_ERR_INVALID;
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    if (ftruncate(fd, (off_t)size) != 0)
        e = ASNGN_ERR_IO;
    if (close(fd) != 0 && e == ASNGN_OK)
        e = ASNGN_ERR_IO;
    return e;
}

asngn_err os_file_size(const char *path, uint64_t *out)
{
    struct stat st;

    if (path == NULL || out == NULL)
        return ASNGN_ERR_INVALID;
    if (stat(path, &st) != 0)
        return (errno == ENOENT) ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_IO;
    *out = (uint64_t)st.st_size;
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
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    size_t n = 0, cap = 0;

    if (path == NULL || out_names == NULL || out_n == NULL)
        return ASNGN_ERR_INVALID;
    *out_names = NULL;
    *out_n = 0;

    d = opendir(path);
    if (d == NULL) {
        if (errno == ENOENT || errno == ENOTDIR)
            return ASNGN_OK; /* missing dir => 0 entries */
        return ASNGN_ERR_IO;
    }

    while ((de = readdir(d)) != NULL) {
        char *full, *name;
        struct stat st;
        size_t len;
        int keep;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        full = os_path_join(path, de->d_name);
        if (full == NULL) {
            closedir(d);
            os_free_names(names, n);
            return ASNGN_ERR_NOMEM;
        }
        keep = stat(full, &st) == 0 &&
               (want_dirs ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
        free(full);
        if (!keep)
            continue;

        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            char **nn = (char **)realloc(names, ncap * sizeof(*nn));
            if (nn == NULL) {
                closedir(d);
                os_free_names(names, n);
                return ASNGN_ERR_NOMEM;
            }
            names = nn;
            cap = ncap;
        }
        len = strlen(de->d_name) + 1;
        name = (char *)malloc(len);
        if (name == NULL) {
            closedir(d);
            os_free_names(names, n);
            return ASNGN_ERR_NOMEM;
        }
        memcpy(name, de->d_name, len);
        names[n++] = name;
    }
    closedir(d);

    *out_names = names;
    *out_n = n;
    return ASNGN_OK;
}

asngn_err os_list_dir(const char *path, char ***out_names, size_t *out_n)
{
    return os_list_kind(path, 0, out_names, out_n);
}

asngn_err os_list_dirs(const char *path, char ***out_names, size_t *out_n)
{
    return os_list_kind(path, 1, out_names, out_n);
}

FILE *os_fopen(const char *path, const char *mode)
{
    if (path == NULL || mode == NULL)
        return NULL;
    return fopen(path, mode);
}

char *os_realpath(const char *path)
{
    if (path == NULL)
        return NULL;
    /* POSIX.1-2008: NULL resolved_name asks realpath to malloc. */
    return realpath(path, NULL);
}

/* ---- misc --------------------------------------------------------------- */

int64_t os_now_unix(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

int64_t os_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

void os_sleep_ms(int ms)
{
    struct timespec ts, rem;

    if (ms <= 0)
        return;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &rem) != 0 && errno == EINTR)
        ts = rem;
}

/* /dev/urandom, falling back to seeded rand() (best effort). Compiled on
 * every POSIX platform so both paths stay warning-clean; used directly only
 * where arc4random_buf is unavailable. */
static void os_random_fallback(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    size_t off = 0;
    int fd;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (off < n) {
            ssize_t got = read(fd, p + off, n - off);
            if (got > 0) {
                off += (size_t)got;
            } else if (got < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
        if (off == n)
            return;
    }

    {
        static int seeded = 0;
        if (!seeded) {
            srand((unsigned)(time(NULL) ^ (long)getpid() ^ (long)clock()));
            seeded = 1;
        }
        for (; off < n; off++)
            p[off] = (unsigned char)(rand() & 0xff);
    }
}

void os_random_bytes(void *buf, size_t n)
{
    if (buf == NULL || n == 0)
        return;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
    arc4random_buf(buf, n);
    (void)os_random_fallback; /* keep the portable path compiled */
#else
    os_random_fallback(buf, n);
#endif
}

int os_hardware_threads(void)
{
    long v = sysconf(_SC_NPROCESSORS_ONLN);
    return v > 0 ? (int)v : 1;
}

#else /* _WIN32 */

typedef int asngn_os_posix_unused_on_win32; /* non-empty translation unit */

#endif /* !_WIN32 */
