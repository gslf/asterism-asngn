/*
 * os.h — platform shim for libasngn (threads, atomic replace, fs, time, rng).
 *
 * Implemented by os_posix.c (pthreads) and os_win32.c (Win32/SRW).
 * With ASNGN_NO_THREADS defined, the locking primitives compile to no-ops
 * and os_thread_start returns ASNGN_ERR_INVALID; os_common.c provides the
 * portable pieces shared by both backends.
 *
 * All paths are UTF-8; the Win32 backend converts to UTF-16 internally.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_OS_H
#define ASNGN_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asngn.h" /* asngn_err */

/* Link-time prefix: libasngn coexists in one process with the siblings'
 * identically-named OS shims (libasper, libastools), so every shim symbol
 * is renamed to asngn_x_* at compile time. Call sites keep the short
 * names. */
#define os_thread_start      asngn_x_thread_start
#define os_thread_join       asngn_x_thread_join
#define os_mutex_init        asngn_x_mutex_init
#define os_mutex_destroy     asngn_x_mutex_destroy
#define os_mutex_lock        asngn_x_mutex_lock
#define os_mutex_unlock      asngn_x_mutex_unlock
#define os_cond_init         asngn_x_cond_init
#define os_cond_destroy      asngn_x_cond_destroy
#define os_cond_timedwait    asngn_x_cond_timedwait
#define os_cond_wait         asngn_x_cond_wait
#define os_cond_signal       asngn_x_cond_signal
#define os_cond_broadcast    asngn_x_cond_broadcast
#define os_rwlock_init       asngn_x_rwlock_init
#define os_rwlock_destroy    asngn_x_rwlock_destroy
#define os_rwlock_rdlock     asngn_x_rwlock_rdlock
#define os_rwlock_rdunlock   asngn_x_rwlock_rdunlock
#define os_rwlock_wrlock     asngn_x_rwlock_wrlock
#define os_rwlock_wrunlock   asngn_x_rwlock_wrunlock
#define os_file_replace      asngn_x_file_replace
#define os_fsync             asngn_x_fsync
#define os_mkdir_p           asngn_x_mkdir_p
#define os_file_exists       asngn_x_file_exists
#define os_read_file         asngn_x_read_file
#define os_write_file        asngn_x_write_file
#define os_remove_file       asngn_x_remove_file
#define os_remove_dir        asngn_x_remove_dir
#define os_truncate          asngn_x_truncate
#define os_file_size         asngn_x_file_size
#define os_rename            asngn_x_rename
#define os_list_dir          asngn_x_list_dir
#define os_list_dirs         asngn_x_list_dirs
#define os_fopen             asngn_x_fopen
#define os_realpath          asngn_x_realpath
#define os_now_unix          asngn_x_now_unix
#define os_monotonic_ms      asngn_x_monotonic_ms
#define os_random_bytes      asngn_x_random_bytes
#define os_hardware_threads  asngn_x_hardware_threads
#define os_sleep_ms          asngn_x_sleep_ms
#define os_path_join         asngn_x_path_join
#define os_path_is_abs       asngn_x_path_is_abs

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque-ish primitive storage -------------------------------------- */

#if defined(ASNGN_NO_THREADS)

typedef struct { int unused; } os_mutex;
typedef struct { int unused; } os_cond;
typedef struct { int unused; } os_rwlock;
typedef struct { int unused; } os_thread;

#elif defined(_WIN32)

/* Storage large enough for SRWLOCK / CONDITION_VARIABLE / HANDLE without
 * leaking <windows.h> into every translation unit. Checked with a static
 * assert in os_win32.c. */
typedef struct { void *h; } os_mutex;   /* SRWLOCK used exclusively */
typedef struct { void *h; } os_cond;    /* CONDITION_VARIABLE       */
typedef struct { void *h; } os_rwlock;  /* SRWLOCK                  */
typedef struct { void *h; } os_thread;  /* HANDLE                   */

#else /* POSIX */

#include <pthread.h>
typedef struct { pthread_mutex_t m; } os_mutex;
typedef struct { pthread_cond_t c; } os_cond;
typedef struct { pthread_rwlock_t l; } os_rwlock;
typedef struct { pthread_t t; int valid; } os_thread;

#endif

/* ---- threads ------------------------------------------------------------ */

asngn_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg);
void      os_thread_join(os_thread *t);

void os_mutex_init(os_mutex *m);
void os_mutex_destroy(os_mutex *m);
void os_mutex_lock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);

void os_cond_init(os_cond *c);
void os_cond_destroy(os_cond *c);
/* Wait with timeout in milliseconds; returns 1 if signaled, 0 on timeout. */
int  os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms);
void os_cond_wait(os_cond *c, os_mutex *m);
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);

void os_rwlock_init(os_rwlock *l);
void os_rwlock_destroy(os_rwlock *l);
void os_rwlock_rdlock(os_rwlock *l);
void os_rwlock_rdunlock(os_rwlock *l);
void os_rwlock_wrlock(os_rwlock *l);
void os_rwlock_wrunlock(os_rwlock *l);

/* ---- filesystem --------------------------------------------------------- */

/* Atomically replace dst with src (rename(2) / ReplaceFileW+MoveFileExW).
 * src is consumed on success. */
asngn_err os_file_replace(const char *src, const char *dst);
/* fsync/_commit an open stream. */
asngn_err os_fsync(FILE *f);
/* Create directory and any missing parents. Existing dir is OK. */
asngn_err os_mkdir_p(const char *path);
int       os_file_exists(const char *path);   /* 1 = yes */
/* Read whole file. *out is NUL-terminated (asngn internal: free with free()).
 * out_len may be NULL. */
asngn_err os_read_file(const char *path, char **out, size_t *out_len);
/* Write whole buffer (truncate). Not atomic — pair with os_file_replace for
 * atomic rewrites. */
asngn_err os_write_file(const char *path, const void *data, size_t len);
asngn_err os_remove_file(const char *path);
asngn_err os_remove_dir(const char *path); /* empty directory only */
/* Truncate file to size bytes (journal torn-tail repair). */
asngn_err os_truncate(const char *path, uint64_t size);
/* File size in bytes; ASNGN_ERR_NOT_FOUND if missing. */
asngn_err os_file_size(const char *path, uint64_t *out);
/* Rename within the same directory tree, replacing target if present
 * (log rotation). */
asngn_err os_rename(const char *src, const char *dst);
/* List regular file names (no dirs) in path, malloc'd array of malloc'd
 * names, unsorted; caller frees each + array. Missing dir => 0 entries. */
asngn_err os_list_dir(const char *path, char ***out_names, size_t *out_n);
/* Immediate subdirectories only (session enumeration); same contract. */
asngn_err os_list_dirs(const char *path, char ***out_names, size_t *out_n);
/* Open a FILE* with UTF-8 path (fopen wrapper; _wfopen on Windows). */
FILE     *os_fopen(const char *path, const char *mode);
/* Canonical absolute path (realpath / GetFullPathNameW). Returns malloc'd
 * UTF-8 string; NULL on failure. */
char     *os_realpath(const char *path);

/* ---- misc --------------------------------------------------------------- */

int64_t os_now_unix(void);                 /* wall clock, unix seconds UTC */
int64_t os_monotonic_ms(void);             /* monotonic milliseconds      */
void    os_random_bytes(void *buf, size_t n); /* CSPRNG or best effort    */
int     os_hardware_threads(void);         /* >= 1 */
/* Sleep for ms milliseconds (real sleep even under ASNGN_NO_THREADS —
 * workers and tests rely on it). ms <= 0 returns immediately. */
void    os_sleep_ms(int ms);
/* Path join with '/' (also fine on Windows APIs used here). Returns
 * malloc'd string. */
char   *os_path_join(const char *a, const char *b);
int     os_path_is_abs(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_OS_H */
