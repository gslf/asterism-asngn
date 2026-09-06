/* Versioned, checksummed WAL frames. Only an incomplete final frame is
 * repairable; a complete frame with a bad checksum is corruption. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>

#define WAL_FRAME_MAX (16u * 1024u * 1024u)
#define WAL_LOG_MAX (256u * 1024u * 1024u)

static void digest(const char *s, size_t n, char out[65]) {
  uint8_t hash[32];
  asngn_sha256(s, n, hash);
  asngn_sha256_hex(hash, sizeof hash, out);
}

asngn_err asngn_wal_append(asngn_ctx *c, asngn_stream *st,
                           const char *record, size_t n) {
  char hash[65], header_hash[65], prefix[128];
  uint64_t size = 0;
  asngn_buf b;
  asngn_err e;
  if (!record || !n || n > WAL_FRAME_MAX) return ASNGN_ERR_LIMIT;
  e = os_file_size(st->path, &size);
  if (e != ASNGN_OK) return e;
  if (size > WAL_LOG_MAX - n - 256) return ASNGN_ERR_LIMIT;
  digest(record, n, hash);
  asngn_buf_init(&b);
  snprintf(prefix, sizeof prefix, "// asngn-wal-v2 %zu %s", n, hash);
  digest(prefix, strlen(prefix), header_hash);
  e = asngn_buf_printf(&b, "%s %s\n", prefix, header_hash);
  if (e == ASNGN_OK) e = asngn_buf_append(&b, record, n);
  if (e == ASNGN_OK) e = asngn_buf_append(&b, "\n", 1);
  if (e == ASNGN_OK) e = asngn_stream_append(c, st, b.data, b.len);
  asngn_buf_free(&b);
  return e;
}

asngn_err asngn_wal_visit(asngn_ctx *c, const char *path, size_t frame_limit,
                          asngn_wal_record_fn record_fn, void *ud) {
  FILE *f;
  asngn_err e = ASNGN_OK;
  uint64_t file_size = 0;
  long good = 0;
  bool torn = false;
  char header[224];
  if (!record_fn || !frame_limit || frame_limit > WAL_FRAME_MAX) return ASNGN_ERR_INVALID;
  if (!os_file_exists(path)) return ASNGN_OK;
  if (os_file_size(path, &file_size) != ASNGN_OK) return ASNGN_ERR_IO;
  if (file_size > WAL_LOG_MAX) return ASNGN_ERR_LIMIT;
  f = os_fopen(path, "rb");
  if (!f) return ASNGN_ERR_IO;
  while (fgets(header, sizeof header, f)) {
    size_t n = 0, got;
    char expected[65], actual[65], canonical[224], *record;
    char *end;
    if (!strchr(header, '\n')) {
      if (feof(f)) torn = true;
      else e = ASNGN_ERR_PARSE;
      break;
    }
    const char magic[] = "// asngn-wal-v2 ";
    if (strncmp(header, magic, sizeof magic-1) ||
        !isdigit((unsigned char)header[sizeof magic-1])) { e = ASNGN_ERR_PARSE; break; }
    errno = 0;
    unsigned long long parsed = strtoull(header+sizeof magic-1, &end, 10);
    if (errno == ERANGE || !parsed || parsed > WAL_FRAME_MAX ||
        strlen(end) != 131 || end[0] != ' ' || end[65] != ' ' || end[130] != '\n') {
      e = ASNGN_ERR_PARSE; break;
    }
    n = (size_t)parsed;
    memcpy(expected, end+1, 64); expected[64] = 0;
    snprintf(canonical, sizeof canonical, "%s%zu %s", magic, n, expected);
    size_t prefix_len = strlen(canonical);
    if (strncmp(header, canonical, prefix_len) || header[prefix_len] != ' ') {
      e = ASNGN_ERR_PARSE; break;
    }
    digest(canonical, prefix_len, actual);
    if (memcmp(end+66, actual, 64)) { e = ASNGN_ERR_PARSE; break; }
    if (n > frame_limit) { e = ASNGN_ERR_LIMIT; break; }
    record = malloc(n + 1);
    if (!record) { e = ASNGN_ERR_NOMEM; break; }
    got = fread(record, 1, n, f);
    if (got != n) { free(record); torn = !ferror(f); break; }
    int separator = fgetc(f);
    if (separator == EOF) { free(record); torn = !ferror(f); break; }
    if (separator != '\n') { free(record); e = ASNGN_ERR_PARSE; break; }
    digest(record, n, actual);
    if (strcmp(expected, actual) || memchr(record, 0, n)) {
      free(record); e = ASNGN_ERR_PARSE; break;
    }
    e = record_fn(ud,record,n);
    free(record);
    if (e != ASNGN_OK) break;
    good = ftell(f);
    if (good < 0) { e = ASNGN_ERR_IO; break; }
  }
  if (ferror(f)) e = ASNGN_ERR_IO;
  fclose(f);
  if (e == ASNGN_OK && torn) {
    e = os_truncate(path, (uint64_t)good);
    f = e == ASNGN_OK ? os_fopen(path, "ab") : NULL;
    if (f) { e = os_fsync(f); fclose(f); }
    else if (e == ASNGN_OK) e = ASNGN_ERR_IO;
    if (e == ASNGN_OK) asngn_log(c, ASNGN_LOG_WARN, "storage", "discarded incomplete WAL tail: %s", path);
  }
  return e;
}

static asngn_err collect(void *ud, const char *record, size_t bytes) {
  xcdn_document_t *out = ud, *frame = xcdn_parse_str(record,bytes,NULL);
  if (!frame) return ASNGN_ERR_PARSE;
  asngn_err e = ASNGN_OK;
  for (size_t i = 0; e == ASNGN_OK && i < frame->values_len; i++) {
    if (!asngn_xdoc_push(out,frame->values[i])) e = ASNGN_ERR_NOMEM;
    else frame->values[i] = NULL;
  }
  xcdn_document_free(frame); return e;
}

asngn_err asngn_wal_load(asngn_ctx *c, const char *path, struct xcdn_document **out) {
  *out = NULL;
  if (!os_file_exists(path)) return ASNGN_OK;
  xcdn_document_t *doc = xcdn_document_new();
  if (!doc) return ASNGN_ERR_NOMEM;
  asngn_err e = asngn_wal_visit(c,path,WAL_FRAME_MAX,collect,doc);
  if (e == ASNGN_OK) *out = doc;
  else xcdn_document_free(doc);
  return e;
}
