/*
 * stream.c — append streams with the torn-tail rule.
 *
 * An append stream is a FILE* opened "ab" plus its path; every append is
 * one compact single-line value + '\n', flushed, with offset rollback on
 * partial writes (the asper journal discipline). Loading applies the
 * torn-tail rule: a parse failure confined to the final value truncates
 * the file to the last good offset with a WARN; a parse error before the
 * tail is fatal and the file is left untouched.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* ═══════════════════════ open / close ═══════════════════════ */

asngn_err asngn_stream_open(asngn_ctx *c, asngn_stream *st, const char *path,
                            bool sync) {
  if (!c || !st || !path) return ASNGN_ERR_INVALID;
  memset(st, 0, sizeof(*st));
  st->path = asngn_strdup(path);
  if (!st->path)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "stream: out of memory");
  st->fp = os_fopen(path, "ab");
  if (!st->fp) {
    free(st->path);
    memset(st, 0, sizeof(*st));
    return asngn_seterr(c, ASNGN_ERR_IO, "stream: cannot open %s", path);
  }
  st->sync = sync;
  return ASNGN_OK;
}

void asngn_stream_close(asngn_stream *st) {
  if (!st) return;
  if (st->fp) fclose(st->fp);
  free(st->path);
  memset(st, 0, sizeof(*st));
}

/* ═══════════════════════ append ═══════════════════════ */

asngn_err asngn_stream_append(asngn_ctx *c, asngn_stream *st,
                              const char *line, size_t len) {
  long good_off;
  bool ok;

  if (!c || !st || !line) return ASNGN_ERR_INVALID;
  if (!st->fp)
    return asngn_seterr(c, ASNGN_ERR_IO, "stream closed: %s",
                        st->path ? st->path : "(null)");

  /* Capture the end offset of the last fully-written line: every prior
   * append flushed, so file size == logical end. A failed write below is
   * rolled back to this offset — otherwise the partial line would be
   * buried mid-file by later appends, where the torn-tail rule cannot
   * repair it and the stream refuses to load. */
  good_off = -1;
  if (fseek(st->fp, 0, SEEK_END) == 0) good_off = ftell(st->fp);

  ok = true;
  if (len > 0 && fwrite(line, 1, len, st->fp) != len) ok = false;
  if (ok && (len == 0 || line[len - 1] != '\n') &&
      fwrite("\n", 1, 1, st->fp) != 1)
    ok = false;
  if (ok && fflush(st->fp) != 0) ok = false;

  if (!ok) {
    bool recovered = false;
    clearerr(st->fp);
    fclose(st->fp);
    st->fp = NULL;
    if (good_off >= 0 &&
        os_truncate(st->path, (uint64_t)good_off) == ASNGN_OK) {
      st->fp = os_fopen(st->path, "ab");
      recovered = st->fp != NULL;
    }
    if (!recovered)
      asngn_log(c, ASNGN_LOG_ERROR, "session",
                "stream: append to %s failed and rollback to offset %ld "
                "failed; stream closed, further appends will fail",
                st->path, good_off);
    return asngn_seterr(c, ASNGN_ERR_IO, "stream: append failed: %s",
                        st->path);
  }

  if (st->sync) {
    asngn_err e = os_fsync(st->fp);
    if (e != ASNGN_OK)
      return asngn_seterr(c, e, "stream: fsync failed: %s", st->path);
  }
  return ASNGN_OK;
}

/* ═══════════════════════ load (torn-tail rule) ═══════════════════════ */

asngn_err asngn_stream_load(asngn_ctx *c, const char *path, const char *what,
                            struct xcdn_document **out_doc) {
  char *text = NULL;
  size_t len = 0;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  asngn_err e;

  if (!c || !path || !what || !out_doc) return ASNGN_ERR_INVALID;
  *out_doc = NULL;
  if (!os_file_exists(path)) return ASNGN_OK;
  e = os_read_file(path, &text, &len);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "%s: cannot read %s", what, path);
  if (len == 0) {
    free(text);
    doc = xcdn_document_new();
    if (!doc)
      return asngn_seterr(c, ASNGN_ERR_NOMEM, "%s: out of memory", what);
    *out_doc = doc;
    return ASNGN_OK;
  }

  doc = xcdn_parse_str(text, len, &xe);
  if (!doc) {
    /* Torn-tail rule: retry with the buffer truncated after the last '\n'
     * that is followed by a non-empty remainder (drops exactly the final
     * value); success => truncate the file there and WARN. Anything else
     * is a mid-file error: ASNGN_ERR_PARSE, file untouched. The first
     * error (xe) is kept so its message survives for the mid-file report. */
    size_t off = 0;
    xcdn_error_t xe2;
    for (size_t i = len; i-- > 0;) {
      if (text[i] != '\n') continue;
      bool nonempty = false;
      for (size_t j = i + 1; j < len; j++) {
        char ch = text[j];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
          nonempty = true;
          break;
        }
      }
      if (nonempty) {
        off = i + 1;
        break;
      }
    }
    doc = off > 0 ? xcdn_parse_str(text, off, &xe2) : xcdn_document_new();
    if (!doc) {
      free(text);
      if (off == 0)
        return asngn_seterr(c, ASNGN_ERR_NOMEM, "%s: out of memory", what);
      return asngn_seterr(c, ASNGN_ERR_PARSE,
                          "%s: parse error before the tail: %s (line %zu)",
                          what, xe.message, xe.span.line);
    }
    e = os_truncate(path, (uint64_t)off);
    if (e != ASNGN_OK) {
      xcdn_document_free(doc);
      free(text);
      return asngn_seterr(c, e, "%s: cannot truncate torn tail of %s", what,
                          path);
    }
    asngn_log(c, ASNGN_LOG_WARN, "session",
              "%s torn tail: 1 value discarded (%zu byte(s) dropped)", what,
              len - off);
  }
  free(text);

  *out_doc = doc;
  return ASNGN_OK;
}

/* ═══════════════════════ atomic replace ═══════════════════════ */

asngn_err asngn_write_atomic(asngn_ctx *c, const char *path, const char *data,
                             size_t len) {
  asngn_err e = ASNGN_OK;
  size_t plen;
  FILE *f;
  char *tmp;

  if (!c || !path) return ASNGN_ERR_INVALID;
  plen = strlen(path);
  tmp = (char *)malloc(plen + 5);
  if (!tmp)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "stream: out of memory");
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", 5);

  f = os_fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return asngn_seterr(c, ASNGN_ERR_IO, "stream: cannot write %s", path);
  }
  if (len > 0 && fwrite(data, 1, len, f) != len) e = ASNGN_ERR_IO;
  if (e == ASNGN_OK && fflush(f) != 0) e = ASNGN_ERR_IO;
  if (e == ASNGN_OK) e = os_fsync(f);
  if (fclose(f) != 0 && e == ASNGN_OK) e = ASNGN_ERR_IO;
  if (e == ASNGN_OK) e = os_file_replace(tmp, path);
  if (e != ASNGN_OK) {
    (void)os_remove_file(tmp);
    asngn_seterr(c, e, "stream: atomic replace of %s failed", path);
  }
  free(tmp);
  return e;
}
