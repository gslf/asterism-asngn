/*
 * json.c — strict RFC 8259 JSON codec for asngn-mcp. See json.h.
 *
 * MIT License — per aspera ad astra.
 */

#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JX_MAX_DEPTH 64

typedef struct jx_member {
  char *key;
  jx_value *val;
} jx_member;

struct jx_value {
  jx_type type;
  union {
    int b;
    struct { double d; long long i; int is_int; } num;
    struct { char *ptr; size_t len; } str;
    struct { jx_value **items; size_t n, cap; } arr;
    struct { jx_member *members; size_t n, cap; } obj;
  } u;
};

/* ═══════════════════════ growable byte buffer ═══════════════════════ */

typedef struct {
  char *d;
  size_t n, cap;
} jbuf;

static int jbuf_reserve(jbuf *b, size_t extra) {
  size_t need;
  if (extra > (size_t)-1 - b->n) return -1;
  need = b->n + extra;
  if (need <= b->cap) return 0;
  {
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
      if (nc > (size_t)-1 / 2) return -1;
      nc *= 2;
    }
    {
      char *nd = realloc(b->d, nc);
      if (!nd) return -1;
      b->d = nd;
      b->cap = nc;
    }
  }
  return 0;
}

static int jbuf_put(jbuf *b, const char *s, size_t n) {
  if (jbuf_reserve(b, n)) return -1;
  memcpy(b->d + b->n, s, n);
  b->n += n;
  return 0;
}

static int jbuf_putc(jbuf *b, char c) {
  if (jbuf_reserve(b, 1)) return -1;
  b->d[b->n++] = c;
  return 0;
}

static int jbuf_puts(jbuf *b, const char *s) {
  return jbuf_put(b, s, strlen(s));
}

/* NUL-terminate without extending n; guarantees d != NULL afterwards. */
static int jbuf_term(jbuf *b) {
  if (jbuf_reserve(b, 1)) return -1;
  b->d[b->n] = '\0';
  return 0;
}

static void jbuf_free(jbuf *b) {
  free(b->d);
  b->d = NULL;
  b->n = b->cap = 0;
}

/* ═══════════════════════ UTF-8 ═══════════════════════ */

/* Length (1..4) of the valid UTF-8 sequence starting at p, or 0 when the
 * bytes are not well-formed (overlongs, surrogates and > U+10FFFF are
 * rejected). */
static size_t utf8_seq(const unsigned char *p, size_t avail) {
  unsigned char c = p[0];
  if (c < 0x80) return 1;
  if (c < 0xC2) return 0; /* continuation byte or overlong lead */
  if (c < 0xE0) {
    if (avail < 2 || (p[1] & 0xC0) != 0x80) return 0;
    return 2;
  }
  if (c < 0xF0) {
    if (avail < 3) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0; /* overlong */
    if (c == 0xED && p[1] > 0x9F) return 0; /* UTF-16 surrogates */
    return 3;
  }
  if (c < 0xF5) {
    if (avail < 4) return 0;
    if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
        (p[3] & 0xC0) != 0x80)
      return 0;
    if (c == 0xF0 && p[1] < 0x90) return 0; /* overlong */
    if (c == 0xF4 && p[1] > 0x8F) return 0; /* > U+10FFFF */
    return 4;
  }
  return 0;
}

static int utf8_valid(const char *s, size_t len) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < len) {
    size_t n = utf8_seq(p + i, len - i);
    if (!n) return 0;
    i += n;
  }
  return 1;
}

/* cp <= 0x10FFFF and not a surrogate (caller guarantees). */
static size_t utf8_encode(unsigned long cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* ═══════════════════════ values ═══════════════════════ */

static jx_value *val_new(jx_type t) {
  jx_value *v = calloc(1, sizeof *v);
  if (v) v->type = t;
  return v;
}

void jx_free(jx_value *v) {
  if (!v) return;
  switch (v->type) {
  case JX_STRING:
    free(v->u.str.ptr);
    break;
  case JX_ARRAY: {
    size_t i;
    for (i = 0; i < v->u.arr.n; i++) jx_free(v->u.arr.items[i]);
    free(v->u.arr.items);
    break;
  }
  case JX_OBJECT: {
    size_t i;
    for (i = 0; i < v->u.obj.n; i++) {
      free(v->u.obj.members[i].key);
      jx_free(v->u.obj.members[i].val);
    }
    free(v->u.obj.members);
    break;
  }
  default:
    break;
  }
  free(v);
}

jx_value *jx_null(void) { return val_new(JX_NULL); }

jx_value *jx_bool(int b) {
  jx_value *v = val_new(JX_BOOL);
  if (v) v->u.b = (b != 0);
  return v;
}

jx_value *jx_int(long long i) {
  jx_value *v = val_new(JX_NUMBER);
  if (v) {
    v->u.num.d = (double)i;
    v->u.num.i = i;
    v->u.num.is_int = 1;
  }
  return v;
}

jx_value *jx_double(double d) {
  jx_value *v;
  if (!isfinite(d)) return NULL;
  v = val_new(JX_NUMBER);
  if (v) {
    v->u.num.d = d;
    v->u.num.i = 0;
    v->u.num.is_int = 0;
  }
  return v;
}

/* Takes ownership of buf (malloc'd, NUL-terminated, len bytes). */
static jx_value *val_take_string(char *buf, size_t len) {
  jx_value *v = val_new(JX_STRING);
  if (!v) {
    free(buf);
    return NULL;
  }
  v->u.str.ptr = buf;
  v->u.str.len = len;
  return v;
}

jx_value *jx_string(const char *utf8) {
  size_t len;
  char *copy;
  if (!utf8) return NULL;
  len = strlen(utf8);
  if (!utf8_valid(utf8, len)) return NULL;
  copy = malloc(len + 1);
  if (!copy) return NULL;
  memcpy(copy, utf8, len + 1);
  return val_take_string(copy, len);
}

jx_value *jx_array(void) { return val_new(JX_ARRAY); }
jx_value *jx_object(void) { return val_new(JX_OBJECT); }

jx_type jx_typeof(const jx_value *v) { return v ? v->type : JX_NULL; }

int jx_bool_value(const jx_value *v) {
  return (v && v->type == JX_BOOL) ? v->u.b : 0;
}

int jx_is_int(const jx_value *v) {
  return v && v->type == JX_NUMBER && v->u.num.is_int;
}

long long jx_int_value(const jx_value *v) {
  if (!v || v->type != JX_NUMBER) return 0;
  return v->u.num.is_int ? v->u.num.i : (long long)v->u.num.d;
}

double jx_double_value(const jx_value *v) {
  return (v && v->type == JX_NUMBER) ? v->u.num.d : 0.0;
}

const char *jx_string_value(const jx_value *v) {
  return (v && v->type == JX_STRING) ? v->u.str.ptr : NULL;
}

size_t jx_string_length(const jx_value *v) {
  return (v && v->type == JX_STRING) ? v->u.str.len : 0;
}

int jx_array_push(jx_value *arr, jx_value *v) {
  if (!v) return -1;
  if (!arr || arr->type != JX_ARRAY) {
    jx_free(v);
    return -1;
  }
  if (arr->u.arr.n == arr->u.arr.cap) {
    size_t nc = arr->u.arr.cap ? arr->u.arr.cap * 2 : 4;
    jx_value **ni;
    if (nc <= arr->u.arr.cap || nc > (size_t)-1 / sizeof *ni) {
      jx_free(v);
      return -1;
    }
    ni = realloc(arr->u.arr.items, nc * sizeof *ni);
    if (!ni) {
      jx_free(v);
      return -1;
    }
    arr->u.arr.items = ni;
    arr->u.arr.cap = nc;
  }
  arr->u.arr.items[arr->u.arr.n++] = v;
  return 0;
}

size_t jx_array_len(const jx_value *arr) {
  return (arr && arr->type == JX_ARRAY) ? arr->u.arr.n : 0;
}

jx_value *jx_array_at(const jx_value *arr, size_t i) {
  if (!arr || arr->type != JX_ARRAY || i >= arr->u.arr.n) return NULL;
  return arr->u.arr.items[i];
}

/* Takes ownership of key (malloc'd) and v; replaces on duplicate key. */
static int obj_put(jx_value *obj, char *key, jx_value *v) {
  size_t i;
  for (i = 0; i < obj->u.obj.n; i++) {
    if (strcmp(obj->u.obj.members[i].key, key) == 0) {
      free(key);
      jx_free(obj->u.obj.members[i].val);
      obj->u.obj.members[i].val = v;
      return 0;
    }
  }
  if (obj->u.obj.n == obj->u.obj.cap) {
    size_t nc = obj->u.obj.cap ? obj->u.obj.cap * 2 : 4;
    jx_member *nm;
    if (nc <= obj->u.obj.cap || nc > (size_t)-1 / sizeof *nm) {
      free(key);
      jx_free(v);
      return -1;
    }
    nm = realloc(obj->u.obj.members, nc * sizeof *nm);
    if (!nm) {
      free(key);
      jx_free(v);
      return -1;
    }
    obj->u.obj.members = nm;
    obj->u.obj.cap = nc;
  }
  obj->u.obj.members[obj->u.obj.n].key = key;
  obj->u.obj.members[obj->u.obj.n].val = v;
  obj->u.obj.n++;
  return 0;
}

int jx_object_set(jx_value *obj, const char *key, jx_value *v) {
  char *k;
  size_t klen;
  if (!v) return -1;
  if (!obj || obj->type != JX_OBJECT || !key) {
    jx_free(v);
    return -1;
  }
  klen = strlen(key);
  k = malloc(klen + 1);
  if (!k) {
    jx_free(v);
    return -1;
  }
  memcpy(k, key, klen + 1);
  return obj_put(obj, k, v);
}

jx_value *jx_object_get(const jx_value *obj, const char *key) {
  size_t i;
  if (!obj || obj->type != JX_OBJECT || !key) return NULL;
  for (i = 0; i < obj->u.obj.n; i++) {
    if (strcmp(obj->u.obj.members[i].key, key) == 0)
      return obj->u.obj.members[i].val;
  }
  return NULL;
}

size_t jx_object_count(const jx_value *obj) {
  return (obj && obj->type == JX_OBJECT) ? obj->u.obj.n : 0;
}

const char *jx_object_key_at(const jx_value *obj, size_t i) {
  if (!obj || obj->type != JX_OBJECT || i >= obj->u.obj.n) return NULL;
  return obj->u.obj.members[i].key;
}

jx_value *jx_object_value_at(const jx_value *obj, size_t i) {
  if (!obj || obj->type != JX_OBJECT || i >= obj->u.obj.n) return NULL;
  return obj->u.obj.members[i].val;
}

jx_value *jx_clone(const jx_value *v) {
  if (!v) return NULL;
  switch (v->type) {
  case JX_NULL:
    return jx_null();
  case JX_BOOL:
    return jx_bool(v->u.b);
  case JX_NUMBER: {
    jx_value *c = val_new(JX_NUMBER);
    if (c) c->u.num = v->u.num;
    return c;
  }
  case JX_STRING: {
    char *buf = malloc(v->u.str.len + 1);
    if (!buf) return NULL;
    memcpy(buf, v->u.str.ptr, v->u.str.len);
    buf[v->u.str.len] = '\0';
    return val_take_string(buf, v->u.str.len);
  }
  case JX_ARRAY: {
    jx_value *c = jx_array();
    size_t i;
    if (!c) return NULL;
    for (i = 0; i < v->u.arr.n; i++) {
      if (jx_array_push(c, jx_clone(v->u.arr.items[i])) != 0) {
        jx_free(c);
        return NULL;
      }
    }
    return c;
  }
  case JX_OBJECT: {
    jx_value *c = jx_object();
    size_t i;
    if (!c) return NULL;
    for (i = 0; i < v->u.obj.n; i++) {
      if (jx_object_set(c, v->u.obj.members[i].key,
                        jx_clone(v->u.obj.members[i].val)) != 0) {
        jx_free(c);
        return NULL;
      }
    }
    return c;
  }
  }
  return NULL;
}

/* ═══════════════════════ parser ═══════════════════════ */

typedef struct {
  const unsigned char *s;
  size_t len, pos;
  int depth;
} jparse;

static int p_value(jparse *p, jx_value **out);

static int isdig(unsigned char c) { return c >= '0' && c <= '9'; }

static void skip_ws(jparse *p) {
  while (p->pos < p->len) {
    unsigned char c = p->s[p->pos];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    p->pos++;
  }
}

static int p_lit(jparse *p, const char *lit) {
  size_t n = strlen(lit);
  if (p->len - p->pos < n || memcmp(p->s + p->pos, lit, n) != 0) return -1;
  p->pos += n;
  return 0;
}

static int p_hex4(jparse *p, unsigned *out) {
  unsigned v = 0;
  int i;
  if (p->len - p->pos < 4) return -1;
  for (i = 0; i < 4; i++) {
    unsigned char c = p->s[p->pos + i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  p->pos += 4;
  *out = v;
  return 0;
}

/* Caller verified the opening quote. *out is malloc'd + NUL-terminated;
 * *out_len excludes the terminator (embedded NULs from \u0000 escapes are
 * counted). */
static int p_string_raw(jparse *p, char **out, size_t *out_len) {
  jbuf b = {NULL, 0, 0};
  p->pos++; /* '"' */
  for (;;) {
    unsigned char c;
    if (p->pos >= p->len) goto fail;
    c = p->s[p->pos];
    if (c == '"') {
      p->pos++;
      break;
    }
    if (c == '\\') {
      unsigned char e;
      int r;
      p->pos++;
      if (p->pos >= p->len) goto fail;
      e = p->s[p->pos++];
      switch (e) {
      case '"':  r = jbuf_putc(&b, '"'); break;
      case '\\': r = jbuf_putc(&b, '\\'); break;
      case '/':  r = jbuf_putc(&b, '/'); break;
      case 'b':  r = jbuf_putc(&b, '\b'); break;
      case 'f':  r = jbuf_putc(&b, '\f'); break;
      case 'n':  r = jbuf_putc(&b, '\n'); break;
      case 'r':  r = jbuf_putc(&b, '\r'); break;
      case 't':  r = jbuf_putc(&b, '\t'); break;
      case 'u': {
        unsigned cp;
        char enc[4];
        if (p_hex4(p, &cp)) goto fail;
        if (cp >= 0xDC00 && cp <= 0xDFFF) goto fail; /* lone low surrogate */
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          unsigned lo;
          if (p->len - p->pos < 2 || p->s[p->pos] != '\\' ||
              p->s[p->pos + 1] != 'u')
            goto fail;
          p->pos += 2;
          if (p_hex4(p, &lo)) goto fail;
          if (lo < 0xDC00 || lo > 0xDFFF) goto fail;
          cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
        }
        r = jbuf_put(&b, enc, utf8_encode(cp, enc));
        break;
      }
      default:
        goto fail;
      }
      if (r) goto fail;
    } else if (c < 0x20) {
      goto fail; /* unescaped control character */
    } else {
      size_t sl = utf8_seq(p->s + p->pos, p->len - p->pos);
      if (!sl) goto fail;
      if (jbuf_put(&b, (const char *)(p->s + p->pos), sl)) goto fail;
      p->pos += sl;
    }
  }
  if (jbuf_term(&b)) goto fail;
  *out = b.d;
  *out_len = b.n;
  return 0;
fail:
  jbuf_free(&b);
  return -1;
}

static int p_number(jparse *p, jx_value **out) {
  const unsigned char *s = p->s;
  size_t start = p->pos, tlen;
  int has_frac = 0, has_exp = 0, bad, is_int = 0;
  char stack[64], *tok = stack, *end;
  double d;
  long long iv = 0;
  jx_value *v;

  if (p->pos < p->len && s[p->pos] == '-') p->pos++;
  if (p->pos >= p->len) return -1;
  if (s[p->pos] == '0') {
    p->pos++;
  } else if (isdig(s[p->pos])) {
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  } else {
    return -1;
  }
  if (p->pos < p->len && s[p->pos] == '.') {
    has_frac = 1;
    p->pos++;
    if (p->pos >= p->len || !isdig(s[p->pos])) return -1;
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  }
  if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
    has_exp = 1;
    p->pos++;
    if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) p->pos++;
    if (p->pos >= p->len || !isdig(s[p->pos])) return -1;
    while (p->pos < p->len && isdig(s[p->pos])) p->pos++;
  }

  tlen = p->pos - start;
  if (tlen + 1 > sizeof stack) {
    tok = malloc(tlen + 1);
    if (!tok) return -1;
  }
  memcpy(tok, s + start, tlen);
  tok[tlen] = '\0';

  end = NULL;
  d = strtod(tok, &end);
  /* Grammar already validated; overflow to infinity is rejected. */
  bad = (end != tok + tlen) || !isfinite(d);
  if (!bad && !has_frac && !has_exp) {
    char *e2 = NULL;
    long long t;
    errno = 0;
    t = strtoll(tok, &e2, 10);
    if (errno != ERANGE && e2 == tok + tlen) {
      is_int = 1;
      iv = t;
    }
    /* "-0" keeps its sign only as a double. */
    if (is_int && iv == 0 && tok[0] == '-') is_int = 0;
  }
  if (tok != stack) free(tok);
  if (bad) return -1;

  v = val_new(JX_NUMBER);
  if (!v) return -1;
  v->u.num.d = d;
  v->u.num.i = iv;
  v->u.num.is_int = is_int;
  *out = v;
  return 0;
}

static int p_array(jparse *p, jx_value **out) {
  jx_value *arr;
  if (p->depth >= JX_MAX_DEPTH) return -1;
  p->depth++;
  p->pos++; /* '[' */
  arr = jx_array();
  if (!arr) {
    p->depth--;
    return -1;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == ']') {
    p->pos++;
    p->depth--;
    *out = arr;
    return 0;
  }
  for (;;) {
    jx_value *item = NULL;
    if (p_value(p, &item)) goto fail;
    if (jx_array_push(arr, item)) goto fail;
    skip_ws(p);
    if (p->pos >= p->len) goto fail;
    if (p->s[p->pos] == ',') {
      p->pos++;
      continue;
    }
    if (p->s[p->pos] == ']') {
      p->pos++;
      break;
    }
    goto fail;
  }
  p->depth--;
  *out = arr;
  return 0;
fail:
  jx_free(arr);
  p->depth--;
  return -1;
}

static int p_object(jparse *p, jx_value **out) {
  jx_value *obj;
  if (p->depth >= JX_MAX_DEPTH) return -1;
  p->depth++;
  p->pos++; /* '{' */
  obj = jx_object();
  if (!obj) {
    p->depth--;
    return -1;
  }
  skip_ws(p);
  if (p->pos < p->len && p->s[p->pos] == '}') {
    p->pos++;
    p->depth--;
    *out = obj;
    return 0;
  }
  for (;;) {
    char *key = NULL;
    size_t klen = 0;
    jx_value *v = NULL;
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != '"') goto fail;
    if (p_string_raw(p, &key, &klen)) goto fail;
    skip_ws(p);
    if (p->pos >= p->len || p->s[p->pos] != ':') {
      free(key);
      goto fail;
    }
    p->pos++;
    if (p_value(p, &v)) {
      free(key);
      goto fail;
    }
    if (obj_put(obj, key, v)) goto fail; /* obj_put consumed key + v */
    skip_ws(p);
    if (p->pos >= p->len) goto fail;
    if (p->s[p->pos] == ',') {
      p->pos++;
      continue;
    }
    if (p->s[p->pos] == '}') {
      p->pos++;
      break;
    }
    goto fail;
  }
  p->depth--;
  *out = obj;
  return 0;
fail:
  jx_free(obj);
  p->depth--;
  return -1;
}

static int p_value(jparse *p, jx_value **out) {
  unsigned char c;
  skip_ws(p);
  if (p->pos >= p->len) return -1;
  c = p->s[p->pos];
  switch (c) {
  case 'n':
    if (p_lit(p, "null")) return -1;
    *out = jx_null();
    return *out ? 0 : -1;
  case 't':
    if (p_lit(p, "true")) return -1;
    *out = jx_bool(1);
    return *out ? 0 : -1;
  case 'f':
    if (p_lit(p, "false")) return -1;
    *out = jx_bool(0);
    return *out ? 0 : -1;
  case '"': {
    char *buf = NULL;
    size_t blen = 0;
    jx_value *v;
    if (p_string_raw(p, &buf, &blen)) return -1;
    v = val_take_string(buf, blen);
    if (!v) return -1;
    *out = v;
    return 0;
  }
  case '[':
    return p_array(p, out);
  case '{':
    return p_object(p, out);
  default:
    if (c == '-' || isdig(c)) return p_number(p, out);
    return -1;
  }
}

int jx_parse(const char *text, size_t len, jx_value **out) {
  jparse p;
  jx_value *v = NULL;
  if (out) *out = NULL;
  if (!text || !out) return -1;
  p.s = (const unsigned char *)text;
  p.len = len;
  p.pos = 0;
  p.depth = 0;
  if (p_value(&p, &v)) return -1;
  skip_ws(&p);
  if (p.pos != p.len) { /* trailing garbage */
    jx_free(v);
    return -1;
  }
  *out = v;
  return 0;
}

/* ═══════════════════════ writer ═══════════════════════ */

static int w_string(jbuf *b, const char *s, size_t len) {
  size_t i;
  if (jbuf_putc(b, '"')) return -1;
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    int r;
    switch (c) {
    case '"':  r = jbuf_put(b, "\\\"", 2); break;
    case '\\': r = jbuf_put(b, "\\\\", 2); break;
    case '\b': r = jbuf_put(b, "\\b", 2); break;
    case '\f': r = jbuf_put(b, "\\f", 2); break;
    case '\n': r = jbuf_put(b, "\\n", 2); break;
    case '\r': r = jbuf_put(b, "\\r", 2); break;
    case '\t': r = jbuf_put(b, "\\t", 2); break;
    default:
      if (c < 0x20) {
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", (unsigned)c);
        r = jbuf_put(b, esc, 6);
      } else {
        r = jbuf_putc(b, (char)c);
      }
      break;
    }
    if (r) return -1;
  }
  return jbuf_putc(b, '"');
}

static int w_number(jbuf *b, const jx_value *v) {
  char tmp[40];
  if (v->u.num.is_int) {
    snprintf(tmp, sizeof tmp, "%lld", v->u.num.i);
  } else {
    double d = v->u.num.d;
    /* Shortest form that round-trips; fall back to full precision. */
    snprintf(tmp, sizeof tmp, "%.15g", d);
    if (strtod(tmp, NULL) != d) snprintf(tmp, sizeof tmp, "%.17g", d);
  }
  return jbuf_puts(b, tmp);
}

static int w_nl_indent(jbuf *b, int level) {
  int i;
  if (jbuf_putc(b, '\n')) return -1;
  for (i = 0; i < level; i++) {
    if (jbuf_put(b, "  ", 2)) return -1;
  }
  return 0;
}

static int w_value(jbuf *b, const jx_value *v, int pretty, int level) {
  switch (v->type) {
  case JX_NULL:
    return jbuf_puts(b, "null");
  case JX_BOOL:
    return jbuf_puts(b, v->u.b ? "true" : "false");
  case JX_NUMBER:
    return w_number(b, v);
  case JX_STRING:
    return w_string(b, v->u.str.ptr, v->u.str.len);
  case JX_ARRAY: {
    size_t i;
    if (v->u.arr.n == 0) return jbuf_puts(b, "[]");
    if (jbuf_putc(b, '[')) return -1;
    for (i = 0; i < v->u.arr.n; i++) {
      if (i && jbuf_putc(b, ',')) return -1;
      if (pretty && w_nl_indent(b, level + 1)) return -1;
      if (w_value(b, v->u.arr.items[i], pretty, level + 1)) return -1;
    }
    if (pretty && w_nl_indent(b, level)) return -1;
    return jbuf_putc(b, ']');
  }
  case JX_OBJECT: {
    size_t i;
    if (v->u.obj.n == 0) return jbuf_puts(b, "{}");
    if (jbuf_putc(b, '{')) return -1;
    for (i = 0; i < v->u.obj.n; i++) {
      const jx_member *m = &v->u.obj.members[i];
      if (i && jbuf_putc(b, ',')) return -1;
      if (pretty && w_nl_indent(b, level + 1)) return -1;
      if (w_string(b, m->key, strlen(m->key))) return -1;
      if (jbuf_put(b, pretty ? ": " : ":", pretty ? 2 : 1)) return -1;
      if (w_value(b, m->val, pretty, level + 1)) return -1;
    }
    if (pretty && w_nl_indent(b, level)) return -1;
    return jbuf_putc(b, '}');
  }
  }
  return -1;
}

char *jx_write(const jx_value *v, int pretty) {
  jbuf b = {NULL, 0, 0};
  if (!v) return NULL;
  if (w_value(&b, v, pretty != 0, 0) || jbuf_term(&b)) {
    jbuf_free(&b);
    return NULL;
  }
  return b.d;
}
