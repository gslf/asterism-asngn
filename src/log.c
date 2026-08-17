/*
 * log.c — leveled rotating file log + host callback fan-out.
 *
 * Sink independence: the file sink is gated by cfg.log_level; the host
 * callback receives EVERY record at any level and filters on its own. The
 * callback `msg` is the complete formatted single-line record
 * ("<RFC3339> LEVEL SUBSYS  message", no trailing newline). asngn_open
 * installs the default stderr-WARN callback (api.c); asngn_set_logger(NULL)
 * disables the callback sink entirely, per asngn.h.
 *
 * Every failure in this module is silent: logging never fails the caller.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

static const char *level_name(int level) {
  switch (level) {
    case ASNGN_LOG_ERROR: return "ERROR";
    case ASNGN_LOG_WARN:  return "WARN";
    case ASNGN_LOG_INFO:  return "INFO";
    default:              return "DEBUG";
  }
}

asngn_err asngn_log_open(asngn_ctx *c) {
  FILE *f;
  uint64_t sz = 0;
  if (!c) return ASNGN_ERR_INVALID;
  if (!c->cfg.log_path || c->cfg.log_path[0] == '\0') return ASNGN_OK;
  f = os_fopen(c->cfg.log_path, "ab");
  if (!f)
    return asngn_seterr(c, ASNGN_ERR_IO, "log: cannot open '%s'",
                        c->cfg.log_path);
  if (os_file_size(c->cfg.log_path, &sz) != ASNGN_OK) sz = 0;
  os_mutex_lock(&c->log_mu);
  if (c->log_fp) fclose(c->log_fp);
  c->log_fp = f;
  c->log_size = (size_t)sz;
  os_mutex_unlock(&c->log_mu);
  return ASNGN_OK;
}

void asngn_log_close(asngn_ctx *c) {
  if (!c) return;
  os_mutex_lock(&c->log_mu);
  if (c->log_fp) {
    fflush(c->log_fp);
    fclose(c->log_fp);
    c->log_fp = NULL;
  }
  c->log_size = 0;
  os_mutex_unlock(&c->log_mu);
}

/* Rotate <path> -> <path>.1 -> ... -> <path>.(max_files-1); the overflow
 * file is deleted. Reopens a fresh current file. c->log_mu held. On any
 * failure the sink may end up disabled (log_fp NULL) — silent by design;
 * a later asngn_log_open can revive it. */
static void log_rotate_locked(asngn_ctx *c) {
  const char *path = c->cfg.log_path;
  int maxf = c->cfg.log_max_files;
  size_t plen = strlen(path);
  char *pa, *pb;
  int i;
  if (c->log_fp) {
    fflush(c->log_fp);
    fclose(c->log_fp);
    c->log_fp = NULL;
  }
  if (maxf < 1) maxf = 1;
  pa = malloc(plen + 32);
  pb = malloc(plen + 32);
  if (pa && pb) {
    if (maxf == 1) {
      (void)os_remove_file(path); /* no rotated copies kept */
    } else {
      snprintf(pa, plen + 32, "%s.%d", path, maxf - 1);
      (void)os_remove_file(pa); /* delete overflow beyond max_files */
      for (i = maxf - 1; i >= 2; i--) {
        snprintf(pa, plen + 32, "%s.%d", path, i - 1);
        snprintf(pb, plen + 32, "%s.%d", path, i);
        (void)os_rename(pa, pb); /* best effort; source may be absent */
      }
      snprintf(pa, plen + 32, "%s.1", path);
      (void)os_rename(path, pa);
    }
  }
  free(pa);
  free(pb);
  c->log_fp = os_fopen(path, "ab");
  c->log_size = 0;
  if (c->log_fp) {
    uint64_t sz = 0;
    /* if the rename failed we appended to the old file: track real size */
    if (os_file_size(path, &sz) == ASNGN_OK) c->log_size = (size_t)sz;
  }
}

void asngn_log(asngn_ctx *c, int level, const char *subsys, const char *fmt,
               ...) {
  char stamp[32];
  char msg_stack[512];
  char *msg = msg_stack;
  char *msg_heap = NULL;
  char line_stack[768];
  char *line = line_stack;
  char *line_heap = NULL;
  char head[64];
  char *q;
  int need, headlen;
  size_t msglen, linelen;
  va_list ap;
  asngn_log_fn cb;
  void *cb_ud;

  if (!c || !fmt) return;
  if (!subsys) subsys = "-";

  va_start(ap, fmt);
  need = vsnprintf(msg_stack, sizeof msg_stack, fmt, ap);
  va_end(ap);
  if (need < 0) return;
  if ((size_t)need >= sizeof msg_stack) {
    msg_heap = malloc((size_t)need + 1);
    if (msg_heap) {
      va_start(ap, fmt);
      vsnprintf(msg_heap, (size_t)need + 1, fmt, ap);
      va_end(ap);
      msg = msg_heap;
    } /* else: proceed with the truncated stack copy */
  }
  for (q = msg; *q; q++) /* records are single-line by contract */
    if (*q == '\n' || *q == '\r') *q = ' ';
  msglen = strlen(msg);

  asngn_time_format_rfc3339(asngn_clock_now(&c->clock), stamp);
  headlen = snprintf(head, sizeof head, "%s %-5s %-8s ", stamp,
                     level_name(level), subsys);
  if (headlen < 0) {
    free(msg_heap);
    return;
  }
  if ((size_t)headlen >= sizeof head) headlen = (int)sizeof head - 1;

  linelen = (size_t)headlen + msglen;
  if (linelen + 2 > sizeof line_stack) {
    line_heap = malloc(linelen + 2);
    if (line_heap) {
      line = line_heap;
    } else {
      msglen = sizeof line_stack - 2 - (size_t)headlen;
      linelen = (size_t)headlen + msglen;
    }
  }
  memcpy(line, head, (size_t)headlen);
  memcpy(line + headlen, msg, msglen);
  line[linelen] = '\0';

  os_mutex_lock(&c->log_mu);
  cb = c->log_cb;
  cb_ud = c->log_ud;
  if (c->log_fp && level <= c->cfg.log_level) {
    size_t wire = linelen + 1; /* + '\n' */
    if (c->cfg.log_max_size_kb > 0 &&
        c->log_size + wire > (uint64_t)c->cfg.log_max_size_kb * 1024u)
      log_rotate_locked(c);
    if (c->log_fp) {
      if (fwrite(line, 1, linelen, c->log_fp) == linelen &&
          fputc('\n', c->log_fp) != EOF)
        c->log_size += wire;
      fflush(c->log_fp);
      if (c->cfg.log_sync) (void)os_fsync(c->log_fp);
    }
  }
  os_mutex_unlock(&c->log_mu);

  /* Callback outside log_mu: a callback that re-enters asngn_log must not
   * deadlock. NULL means the host disabled the callback sink. */
  if (cb)
    cb(level, line, cb_ud);

  free(msg_heap);
  free(line_heap);
}
