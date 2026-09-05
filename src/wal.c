/* Versioned, checksummed WAL frames. Only an incomplete final frame is
 * repairable; a complete frame with a bad checksum is corruption. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

#define WAL_FRAME_MAX (16u * 1024u * 1024u)
#define WAL_LOG_MAX (256u * 1024u * 1024u)

static void digest(const char *s, size_t n, char out[65]) {
  uint8_t hash[32];
  asngn_sha256(s, n, hash);
  for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", (unsigned)hash[i]);
}

asngn_err asngn_wal_append(asngn_ctx *c, asngn_stream *st,
                           const char *record, size_t n) {
  char hash[65];
  uint64_t size = 0;
  asngn_buf b;
  asngn_err e;
  if (!record || !n || n > WAL_FRAME_MAX) return ASNGN_ERR_LIMIT;
  e = os_file_size(st->path, &size);
  if (e != ASNGN_OK) return e;
  if (size > WAL_LOG_MAX - n - 128) return ASNGN_ERR_LIMIT;
  digest(record, n, hash);
  asngn_buf_init(&b);
  e = asngn_buf_printf(&b, "// asngn-wal-v1 %zu %s\n", n, hash);
  if (e == ASNGN_OK) e = asngn_buf_append(&b, record, n);
  if (e == ASNGN_OK) e = asngn_buf_append(&b, "\n", 1);
  if (e == ASNGN_OK) e = asngn_stream_append(c, st, b.data, b.len);
  asngn_buf_free(&b);
  return e;
}

asngn_err asngn_wal_load(asngn_ctx *c, const char *path,
                         struct xcdn_document **out) {
  FILE *f;
  asngn_buf payload;
  asngn_err e = ASNGN_OK;
  uint64_t file_size = 0;
  long good = 0;
  bool torn = false;
  char header[128];
  *out = NULL;
  if (!os_file_exists(path)) return ASNGN_OK;
  if (os_file_size(path, &file_size) != ASNGN_OK) return ASNGN_ERR_IO;
  if (file_size > WAL_LOG_MAX) return ASNGN_ERR_LIMIT;
  f = os_fopen(path, "rb");
  if (!f) return ASNGN_ERR_IO;
  asngn_buf_init(&payload);
  while (fgets(header, sizeof header, f)) {
    size_t n = 0, got;
    int end = 0;
    char expected[65], actual[65], canonical[128], *record;
    if (!strchr(header, '\n')) {
      if (feof(f)) torn = true;
      else e = ASNGN_ERR_PARSE;
      break;
    }
    if (sscanf(header, "// asngn-wal-v1 %zu %64[0-9a-f]%n", &n, expected, &end) != 2 ||
        strlen(expected) != 64 || header[end] != '\n' || header[end + 1] ||
        n == 0 || n > WAL_FRAME_MAX) { e = ASNGN_ERR_PARSE; break; }
    snprintf(canonical, sizeof canonical, "// asngn-wal-v1 %zu %s\n", n, expected);
    if (strcmp(header, canonical)) { e = ASNGN_ERR_PARSE; break; }
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
    e = asngn_buf_append(&payload, record, n);
    if (e == ASNGN_OK) e = asngn_buf_append(&payload, "\n", 1);
    free(record);
    if (e != ASNGN_OK) break;
    good = ftell(f);
    if (good < 0) { e = ASNGN_ERR_IO; break; }
  }
  if (ferror(f)) e = ASNGN_ERR_IO;
  fclose(f);
  if (e == ASNGN_OK) {
    *out = xcdn_parse_str(payload.data ? payload.data : "", payload.len, NULL);
    if (!*out) e = ASNGN_ERR_PARSE;
  }
  if (e == ASNGN_OK && torn) {
    e = os_truncate(path, (uint64_t)good);
    f = e == ASNGN_OK ? os_fopen(path, "ab") : NULL;
    if (f) { e = os_fsync(f); fclose(f); }
    else if (e == ASNGN_OK) e = ASNGN_ERR_IO;
    if (e == ASNGN_OK) asngn_log(c, ASNGN_LOG_WARN, "storage", "discarded incomplete WAL tail: %s", path);
  }
  asngn_buf_free(&payload);
  if (e != ASNGN_OK) { xcdn_document_free(*out); *out = NULL; }
  return e;
}
