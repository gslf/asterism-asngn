/*
 * util.c — growable buffer, strings, slug/UTF-8 checks, FNV-1a, sentence
 * trimming.
 *
 * asngn_buf: data stays NULL until the first append/printf; every mutating
 * call leaves data NUL-terminated on success (the asper_buf discipline).
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- growable buffer ---------------------------------------------------- */

void asngn_buf_init(asngn_buf *b) {
  if (!b) return;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void asngn_buf_free(asngn_buf *b) {
  if (!b) return;
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static asngn_err buf_reserve(asngn_buf *b, size_t extra) {
  size_t need, cap;
  char *p;
  if (extra > SIZE_MAX - b->len - 1) return ASNGN_ERR_NOMEM;
  need = b->len + extra + 1;
  if (need <= b->cap) return ASNGN_OK;
  cap = b->cap ? b->cap : 64;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  p = realloc(b->data, cap);
  if (!p) return ASNGN_ERR_NOMEM;
  b->data = p;
  b->cap = cap;
  return ASNGN_OK;
}

asngn_err asngn_buf_append(asngn_buf *b, const void *data, size_t len) {
  asngn_err e;
  if (!b || (!data && len > 0)) return ASNGN_ERR_INVALID;
  e = buf_reserve(b, len);
  if (e != ASNGN_OK) return e;
  if (len > 0) memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return ASNGN_OK;
}

asngn_err asngn_buf_appends(asngn_buf *b, const char *s) {
  if (!s) return ASNGN_ERR_INVALID;
  return asngn_buf_append(b, s, strlen(s));
}

asngn_err asngn_buf_appendc(asngn_buf *b, char ch) {
  return asngn_buf_append(b, &ch, 1);
}

asngn_err asngn_buf_printf(asngn_buf *b, const char *fmt, ...) {
  va_list ap;
  int need;
  asngn_err e;
  if (!b || !fmt) return ASNGN_ERR_INVALID;
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return ASNGN_ERR_INVALID;
  e = buf_reserve(b, (size_t)need);
  if (e != ASNGN_OK) return e;
  va_start(ap, fmt);
  vsnprintf(b->data + b->len, (size_t)need + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)need;
  return ASNGN_OK;
}

char *asngn_buf_detach(asngn_buf *b) {
  char *p;
  if (!b) return NULL;
  p = b->data;
  if (!p) {
    p = malloc(1);
    if (p) p[0] = '\0';
  }
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return p;
}

/* ---- strings ------------------------------------------------------------ */

char *asngn_strdup(const char *s) {
  size_t n;
  char *p;
  if (!s) return NULL;
  n = strlen(s);
  p = malloc(n + 1);
  if (!p) return NULL;
  memcpy(p, s, n + 1);
  return p;
}

char *asngn_strndup(const char *s, size_t n) {
  size_t len = 0;
  char *p;
  if (!s) return NULL;
  while (len < n && s[len] != '\0') len++;
  p = malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = '\0';
  return p;
}

int asngn_strcasecmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  for (;; a++, b++) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return (int)ca - (int)cb;
    if (ca == '\0') return 0;
  }
}

bool asngn_str_has_prefix(const char *s, const char *prefix) {
  if (!s || !prefix) return false;
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* ---- session/project slug ----------------------------------------------- */

bool asngn_slug_valid(const char *s) {
  size_t i;
  if (!s) return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
    return false;
  for (i = 1; s[i] != '\0'; i++) {
    char ch = s[i];
    if (i > 63) return false;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return false;
  }
  return true;
}

/* ---- UTF-8 validation ---------------------------------------------------
 * Strict: overlong encodings, UTF-16 surrogates (U+D800..U+DFFF) and code
 * points above U+10FFFF are rejected, as are truncated sequences. */

bool asngn_utf8_valid(const char *s, size_t len) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  if (!s) return len == 0;
  while (i < len) {
    unsigned char b = p[i];
    if (b < 0x80) { /* ASCII */
      i += 1;
    } else if (b >= 0xC2 && b <= 0xDF) { /* U+0080..U+07FF */
      if (len - i < 2 || (p[i + 1] & 0xC0) != 0x80) return false;
      i += 2;
    } else if (b == 0xE0) { /* U+0800..U+0FFF (no overlongs) */
      if (len - i < 3 || p[i + 1] < 0xA0 || p[i + 1] > 0xBF ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b >= 0xE1 && b <= 0xEC) { /* U+1000..U+CFFF */
      if (len - i < 3 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b == 0xED) { /* U+D000..U+D7FF (no surrogates) */
      if (len - i < 3 || p[i + 1] < 0x80 || p[i + 1] > 0x9F ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b >= 0xEE && b <= 0xEF) { /* U+E000..U+FFFF */
      if (len - i < 3 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80)
        return false;
      i += 3;
    } else if (b == 0xF0) { /* U+10000..U+3FFFF (no overlongs) */
      if (len - i < 4 || p[i + 1] < 0x90 || p[i + 1] > 0xBF ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else if (b >= 0xF1 && b <= 0xF3) { /* U+40000..U+FFFFF */
      if (len - i < 4 || (p[i + 1] & 0xC0) != 0x80 ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else if (b == 0xF4) { /* U+100000..U+10FFFF (cap) */
      if (len - i < 4 || p[i + 1] < 0x80 || p[i + 1] > 0x8F ||
          (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80)
        return false;
      i += 4;
    } else { /* 0x80..0xC1, 0xF5..0xFF: stray continuation / invalid lead */
      return false;
    }
  }
  return true;
}

/* ---- control stripping -------------------------------------------------- */

size_t asngn_strip_controls(char *s, size_t len) {
  size_t r, w = 0;
  if (!s) return 0;
  for (r = 0; r < len; r++) {
    unsigned char ch = (unsigned char)s[r];
    if ((ch < 0x20 && ch != '\n' && ch != '\t') || ch == 0x7F)
      continue; /* NUL and other controls stripped, DEL too */
    if (w != r) s[w] = s[r];
    w++;
  }
  if (w < len) s[w] = '\0';
  return w;
}

/* ---- FNV-1a 64 ---------------------------------------------------------- */

uint64_t asngn_fnv1a64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = UINT64_C(14695981039346656037);
  size_t i;
  if (!p) return h;
  for (i = 0; i < len; i++) {
    h ^= p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

/* ---- sentence trimming -------------------------------------------------- */

static bool is_ascii_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' ||
         ch == '\f';
}

/* Last i < len with s[i] in ".!?" followed by whitespace/EOL, or a '\n';
 * the later boundary wins (backward scan returns it first). Boundaries are
 * ASCII, so a UTF-8 sequence is never split. */
size_t asngn_sentence_trim(const char *s, size_t len) {
  size_t i;
  if (!s || len == 0) return len;
  for (i = len; i-- > 0;) {
    char ch = s[i];
    if (ch == '\n') return i + 1;
    if ((ch == '.' || ch == '!' || ch == '?') &&
        (i + 1 == len || is_ascii_space(s[i + 1])))
      return i + 1;
  }
  return len;
}
