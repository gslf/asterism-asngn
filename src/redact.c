/*
 * redact.c — secret scanner. Masks likely secrets with
 * «redacted:XXXXXXXX», where XXXXXXXX is the first 8 lowercase hex chars of
 * the SHA-256 of the exact matched bytes — equal secrets redact equally and
 * stay correlatable without being disclosed.
 *
 * Two phases: structural patterns first (cloud-style access keys, PEM
 * private-key blocks, Authorization headers, key=value pairs), scanned left
 * to right with longest-match-wins and consumed regions; then a Shannon-
 * entropy pass for base64/hex-looking runs over whatever the structural
 * pass left untouched. Replacements are never rescanned. Pure libc + asngn
 * utils; every pattern is ASCII-delimited, so valid UTF-8 in means valid
 * UTF-8 out.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* «redacted:xxxxxxxx» = 2 + 9 + 8 + 2 bytes. */
#define RD_REP_LEN 21

/* ---- character classes -------------------------------------------------- */

static bool cls_upnum(unsigned char ch) { /* [A-Z0-9] */
  return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

static bool cls_alnum(unsigned char ch) { /* [A-Za-z0-9] */
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
         (ch >= '0' && ch <= '9');
}

static bool cls_b64url(unsigned char ch) { /* [A-Za-z0-9_-] */
  return cls_alnum(ch) || ch == '_' || ch == '-';
}

static bool cls_auth(unsigned char ch) { /* [A-Za-z0-9._~+/=-] */
  return cls_alnum(ch) || ch == '.' || ch == '_' || ch == '~' || ch == '+' ||
         ch == '/' || ch == '=' || ch == '-';
}

static bool cls_entropy(unsigned char ch) { /* [A-Za-z0-9+/=_-] */
  return cls_alnum(ch) || ch == '+' || ch == '/' || ch == '=' || ch == '_' ||
         ch == '-';
}

static bool cls_value(unsigned char ch) { /* non-space, non-quote */
  return ch > 0x20 && ch != '"' && ch != '\'';
}

static unsigned char rd_lower(unsigned char ch) {
  return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + 32) : ch;
}

/* Case-insensitive prefix match; `p` must be given in lowercase. */
static bool ci_prefix(const char *s, size_t len, size_t i, const char *p) {
  size_t k, n = strlen(p);
  if (n > len - i) return false;
  for (k = 0; k < n; k++)
    if (rd_lower((unsigned char)s[i + k]) != (unsigned char)p[k]) return false;
  return true;
}

static const char *rd_memfind(const char *hay, size_t haylen,
                              const char *needle, size_t nlen) {
  size_t i;
  if (nlen == 0 || nlen > haylen) return NULL;
  for (i = 0; i + nlen <= haylen; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
      return hay + i;
  return NULL;
}

/* ---- span bookkeeping --------------------------------------------------- */

typedef struct {
  size_t start, end; /* byte range [start, end) */
} rd_span;

typedef struct {
  rd_span *v;
  size_t   n, cap;
} rd_spans;

static bool spans_push(rd_spans *sv, size_t start, size_t end) {
  if (sv->n == sv->cap) {
    size_t   cap = sv->cap ? sv->cap * 2 : 8;
    rd_span *p = realloc(sv->v, cap * sizeof(*p));
    if (!p) return false;
    sv->v = p;
    sv->cap = cap;
  }
  sv->v[sv->n].start = start;
  sv->v[sv->n].end = end;
  sv->n++;
  return true;
}

static int span_cmp(const void *a, const void *b) {
  const rd_span *x = a, *y = b;
  if (x->start < y->start) return -1;
  if (x->start > y->start) return 1;
  return 0;
}

/* ---- structural matchers (a..g) ----------------------------------------- *
 * Each returns the total number of bytes consumed at position i (0 = no
 * match) and sets *moff and *mlen to the masked sub-range relative to i. */

/* a. AWS-style: "AKIA" + exactly 16 [A-Z0-9] (total 20). */
static size_t m_aws(const char *s, size_t len, size_t i, size_t *moff,
                    size_t *mlen) {
  size_t k;
  if (len - i < 20 || memcmp(s + i, "AKIA", 4) != 0) return 0;
  for (k = 4; k < 20; k++)
    if (!cls_upnum((unsigned char)s[i + k])) return 0;
  if (i + 20 < len && cls_upnum((unsigned char)s[i + 20])) return 0;
  *moff = 0;
  *mlen = 20;
  return 20;
}

/* b. Google-style: "AIza" + exactly 35 [A-Za-z0-9_-] (total 39). */
static size_t m_gcp(const char *s, size_t len, size_t i, size_t *moff,
                    size_t *mlen) {
  size_t k;
  if (len - i < 39 || memcmp(s + i, "AIza", 4) != 0) return 0;
  for (k = 4; k < 39; k++)
    if (!cls_b64url((unsigned char)s[i + k])) return 0;
  if (i + 39 < len && cls_b64url((unsigned char)s[i + 39])) return 0;
  *moff = 0;
  *mlen = 39;
  return 39;
}

/* c. GitHub tokens: gh[pousr]_ + 36+ [A-Za-z0-9]; mask prefix + run. */
static size_t m_github(const char *s, size_t len, size_t i, size_t *moff,
                       size_t *mlen) {
  static const char pre[5][5] = {"ghp_", "gho_", "ghu_", "ghs_", "ghr_"};
  size_t k, run;
  if (len - i < 4) return 0;
  for (k = 0; k < 5; k++)
    if (memcmp(s + i, pre[k], 4) == 0) break;
  if (k == 5) return 0;
  run = 0;
  while (i + 4 + run < len && cls_alnum((unsigned char)s[i + 4 + run])) run++;
  if (run < 36) return 0;
  *moff = 0;
  *mlen = 4 + run;
  return 4 + run;
}

/* d. OpenAI-style: "sk-" + 20+ [A-Za-z0-9_-] ("sk-proj-" falls out). */
static size_t m_openai(const char *s, size_t len, size_t i, size_t *moff,
                       size_t *mlen) {
  size_t run;
  if (len - i < 3 || memcmp(s + i, "sk-", 3) != 0) return 0;
  run = 0;
  while (i + 3 + run < len && cls_b64url((unsigned char)s[i + 3 + run])) run++;
  if (run < 20) return 0;
  *moff = 0;
  *mlen = 3 + run;
  return 3 + run;
}

/* e. PEM blocks: "-----BEGIN <alg>PRIVATE KEY-----" through the matching
 * END marker inclusive; through end of text when no END exists. */
static size_t m_pem(const char *s, size_t len, size_t i, size_t *moff,
                    size_t *mlen) {
  static const char *const algs[] = {"RSA ", "EC ", "OPENSSH ", "DSA ", ""};
  static const char begin[] = "-----BEGIN ";
  static const char tail[] = "PRIVATE KEY-----";
  const size_t begin_n = sizeof(begin) - 1, tail_n = sizeof(tail) - 1;
  const char *alg = NULL, *p;
  char   needle[48];
  size_t a, hdr, total;

  if (len - i < begin_n + tail_n) return 0;
  if (memcmp(s + i, begin, begin_n) != 0) return 0;
  hdr = i + begin_n;
  for (a = 0; a < sizeof(algs) / sizeof(algs[0]); a++) {
    size_t al = strlen(algs[a]);
    if (len - hdr >= al + tail_n && memcmp(s + hdr, algs[a], al) == 0 &&
        memcmp(s + hdr + al, tail, tail_n) == 0) {
      alg = algs[a];
      break;
    }
  }
  if (!alg) return 0;
  hdr += strlen(alg) + tail_n;

  snprintf(needle, sizeof needle, "-----END %s%s", alg, tail);
  p = rd_memfind(s + hdr, len - hdr, needle, strlen(needle));
  total = p ? (size_t)(p - s) + strlen(needle) - i : len - i;
  *moff = 0;
  *mlen = total;
  return total;
}

/* f. Authorization headers: "authorization:" [sp] (bearer|basic|token)
 * sp+ token{8,}; mask only the token, keep the scheme. */
static size_t m_auth(const char *s, size_t len, size_t i, size_t *moff,
                     size_t *mlen) {
  static const char *const schemes[] = {"bearer", "basic", "token"};
  size_t a, j, run, tok;
  if (!ci_prefix(s, len, i, "authorization:")) return 0;
  j = i + 14;
  while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
  for (a = 0; a < 3; a++)
    if (ci_prefix(s, len, j, schemes[a])) {
      j += strlen(schemes[a]);
      break;
    }
  if (a == 3) return 0;
  if (j >= len || (s[j] != ' ' && s[j] != '\t')) return 0;
  while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
  tok = j;
  run = 0;
  while (tok + run < len && cls_auth((unsigned char)s[tok + run])) run++;
  if (run < 8) return 0;
  *moff = tok - i;
  *mlen = run;
  return tok + run - i;
}

/* g. key=value pairs: known key at a word boundary, "=" or ":", optional
 * quote, value of 6+ non-space non-quote chars; mask only the value. */
static size_t m_kv(const char *s, size_t len, size_t i, size_t *moff,
                   size_t *mlen) {
  static const char *const keys[] = {
      "client_secret", "access_token", "private_key", "auth_token",
      "password",      "api_key",      "apikey",      "passwd",
      "secret",        "token"};
  size_t k, best = 0, bmoff = 0, bmlen = 0;

  if (i > 0 && cls_alnum((unsigned char)s[i - 1])) return 0;
  for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
    size_t j, v, run;
    if (!ci_prefix(s, len, i, keys[k])) continue;
    j = i + strlen(keys[k]);
    while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j >= len || (s[j] != '=' && s[j] != ':')) continue;
    j++;
    while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j < len && (s[j] == '"' || s[j] == '\'')) j++;
    v = j;
    run = 0;
    while (v + run < len && cls_value((unsigned char)s[v + run])) run++;
    if (run < 6) continue;
    if (v + run - i > best) {
      best = v + run - i;
      bmoff = v - i;
      bmlen = run;
    }
  }
  if (best == 0) return 0;
  *moff = bmoff;
  *mlen = bmlen;
  return best;
}

typedef size_t (*rd_matcher)(const char *, size_t, size_t, size_t *, size_t *);

/* Longest structural match at position i; ties go to the earlier pattern. */
static size_t match_structural(const char *s, size_t len, size_t i,
                               size_t *moff, size_t *mlen) {
  static const rd_matcher m[] = {m_aws, m_gcp,  m_github, m_openai,
                                 m_pem, m_auth, m_kv};
  size_t best = 0, k;
  for (k = 0; k < sizeof(m) / sizeof(m[0]); k++) {
    size_t o = 0, l = 0;
    size_t t = m[k](s, len, i, &o, &l);
    if (t > best) {
      best = t;
      *moff = o;
      *mlen = l;
    }
  }
  return best;
}

/* ---- h. high-entropy runs ----------------------------------------------- */

/* Shannon entropy over bytes, plus at least one digit and one letter
 * (plain English words fail the entropy bar; the digit/letter
 * requirement drops separators and pure punctuation runs). The bar is
 * alphabet-aware: a pure-hex run maxes out at log2(16) = 4.0 bits/char,
 * so holding it to the base64 bar of 4.0 would exempt the entire hex
 * class; random hex comfortably clears 3.4 bits/char while English
 * hex-ish words ("deadbeef...") stay below it. */
static bool run_is_secret(const unsigned char *p, size_t n) {
  size_t hist[256] = {0};
  size_t i, b;
  bool   digit = false, letter = false, hex_only = true;
  double h = 0.0;
  for (i = 0; i < n; i++) {
    unsigned char ch = p[i];
    hist[ch]++;
    if (ch >= '0' && ch <= '9')
      digit = true;
    else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
      letter = true;
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
          (ch >= 'A' && ch <= 'F')))
      hex_only = false;
  }
  if (!digit || !letter) return false;
  for (b = 0; b < 256; b++) {
    if (hist[b] > 0) {
      double q = (double)hist[b] / (double)n;
      h -= q * log2(q);
    }
  }
  return h >= (hex_only ? 3.4 : 4.0);
}

/* Mask [A-Za-z0-9+/=_-] runs of 32+ passing the entropy bar in [a, b). */
static bool scan_entropy(const char *s, size_t a, size_t b, rd_spans *masks) {
  size_t i = a;
  while (i < b) {
    size_t r;
    if (!cls_entropy((unsigned char)s[i])) {
      i++;
      continue;
    }
    r = i + 1;
    while (r < b && cls_entropy((unsigned char)s[r])) r++;
    if (r - i >= 32 && run_is_secret((const unsigned char *)s + i, r - i)) {
      if (!spans_push(masks, i, r)) return false;
    }
    i = r;
  }
  return true;
}

/* ---- entry point -------------------------------------------------------- */

asngn_err asngn_redact(const char *text, size_t len, char **out,
                       size_t *n_masked) {
  rd_spans consumed, masks;
  size_t   i, pos, masked_bytes, out_len, d;
  char    *dst;
  asngn_err e;

  if (!out || !n_masked) return ASNGN_ERR_INVALID;
  *out = NULL;
  *n_masked = 0;
  if (!text || len == 0) return ASNGN_OK;

  consumed.v = NULL;
  consumed.n = consumed.cap = 0;
  masks.v = NULL;
  masks.n = masks.cap = 0;

  /* Phase 1: structural patterns a..g, left to right; a matched region is
   * consumed and never rescanned. */
  i = 0;
  while (i < len) {
    size_t moff = 0, mlen = 0;
    size_t total = match_structural(text, len, i, &moff, &mlen);
    if (total > 0) {
      if (!spans_push(&consumed, i, i + total) ||
          !spans_push(&masks, i + moff, i + moff + mlen))
        goto oom;
      i += total;
    } else {
      i++;
    }
  }

  /* Phase 2: high-entropy runs over the gaps phase 1 left unconsumed. */
  pos = 0;
  for (i = 0; i <= consumed.n; i++) {
    size_t gap_end = (i < consumed.n) ? consumed.v[i].start : len;
    if (!scan_entropy(text, pos, gap_end, &masks)) goto oom;
    if (i < consumed.n) pos = consumed.v[i].end;
  }

  if (masks.n == 0) {
    e = ASNGN_OK;
    goto done;
  }

  qsort(masks.v, masks.n, sizeof(*masks.v), span_cmp);

  masked_bytes = 0;
  for (i = 0; i < masks.n; i++)
    masked_bytes += masks.v[i].end - masks.v[i].start;
  out_len = len - masked_bytes + masks.n * RD_REP_LEN;
  dst = malloc(out_len + 1);
  if (!dst) goto oom;

  d = 0;
  pos = 0;
  for (i = 0; i < masks.n; i++) {
    size_t  ms = masks.v[i].start, me = masks.v[i].end;
    uint8_t hash[32];
    char    hex[9];
    memcpy(dst + d, text + pos, ms - pos);
    d += ms - pos;
    asngn_sha256(text + ms, me - ms, hash);
    asngn_sha256_hex(hash, 4, hex);
    memcpy(dst + d, "\xC2\xAB", 2); /* « */
    d += 2;
    memcpy(dst + d, "redacted:", 9);
    d += 9;
    memcpy(dst + d, hex, 8);
    d += 8;
    memcpy(dst + d, "\xC2\xBB", 2); /* » */
    d += 2;
    pos = me;
  }
  memcpy(dst + d, text + pos, len - pos);
  d += len - pos;
  dst[d] = '\0';

  *out = dst;
  *n_masked = masks.n;
  e = ASNGN_OK;
  goto done;

oom:
  e = ASNGN_ERR_NOMEM;
done:
  free(consumed.v);
  free(masks.v);
  return e;
}
