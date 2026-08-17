/*
 * steps.c — step names, step ownership, and the defense-in-depth parser
 * over the step grammar.
 *
 * The per-turn grammar already constrains what the model can emit; the
 * plan gate re-validates every line anyway. CALL lines get a light split
 * only (ref / command / args object) — the authoritative CALL parse is
 * astools_call_parse on the full line in the control loop.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* Text payloads are one line of 1-300 chars. */
#define ASNGN_STEP_TEXT_MAX 300

/* ---- names and ownership ------------------------------------------------- */

const char *asngn_step_name(asngn_step_kind k) {
  switch (k) {
    case ASNGN_STEP_CALL:    return "CALL";
    case ASNGN_STEP_RECALL:  return "RECALL";
    case ASNGN_STEP_OPEN:    return "OPEN";
    case ASNGN_STEP_THINK:   return "THINK";
    case ASNGN_STEP_CLARIFY: return "CLARIFY";
    case ASNGN_STEP_ANSWER:  return "ANSWER";
  }
  return "UNKNOWN";
}

void asngn_step_free(asngn_step *st) {
  if (!st) return;
  free(st->text);
  free(st->call_ref);
  free(st->call_cmd);
  free(st->call_args);
  memset(st, 0, sizeof(*st));
}

/* ---- parsing helpers ----------------------------------------------------- */

static bool span_is(const char *p, const char *end, const char *lit) {
  size_t n = strlen(lit);
  return (size_t)(end - p) == n && memcmp(p, lit, n) == 0;
}

/* Match `kw` at p followed by a separator (space, tab, or '|'); returns
 * the position after the keyword, or NULL. */
static const char *match_kw(const char *p, const char *end, const char *kw) {
  size_t n = strlen(kw);
  if ((size_t)(end - p) < n || memcmp(p, kw, n) != 0) return NULL;
  p += n;
  if (p < end && *p != ' ' && *p != '\t' && *p != '|') return NULL;
  return p;
}

/* "RECALL | q" / "THINK | note" / "CLARIFY | q": flexible spaces around
 * the '|', non-empty single-line payload, capped at ASNGN_STEP_TEXT_MAX
 * bytes on a UTF-8 boundary. */
static asngn_err parse_text_step(asngn_ctx *c, const char *q, const char *end,
                                 asngn_step_kind kind, asngn_step *out) {
  const char *r;
  size_t n;

  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (q >= end || *q != '|')
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: %s needs \" | \" and a payload",
                        asngn_step_name(kind));
  q++;
  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (q >= end)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: %s payload is empty",
                        asngn_step_name(kind));
  for (r = q; r < end; r++)
    if (*r == '\n' || *r == '\r')
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                          "step: %s payload spans multiple lines",
                          asngn_step_name(kind));
  n = (size_t)(end - q);
  if (n > ASNGN_STEP_TEXT_MAX) {
    n = ASNGN_STEP_TEXT_MAX;
    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80)
      n--; /* back off to a UTF-8 boundary */
    if (n == 0)
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: %s payload is empty",
                          asngn_step_name(kind));
  }
  out->text = asngn_strndup(q, n);
  if (!out->text) return ASNGN_ERR_NOMEM;
  out->kind = kind;
  return ASNGN_OK;
}

/* "OPEN B<n>", n >= 1, nothing after the handle. */
static asngn_err parse_open(asngn_ctx *c, const char *q, const char *end,
                            asngn_step *out) {
  long n = 0;
  int digits = 0;

  while (q < end && (*q == ' ' || *q == '\t')) q++;
  if (q < end && *q == 'B') {
    q++;
    while (q < end && *q >= '0' && *q <= '9') {
      if (digits < 9) n = n * 10 + (*q - '0');
      digits++;
      q++;
    }
    if (digits >= 1 && digits <= 9 && q == end && n >= 1) {
      out->kind = ASNGN_STEP_OPEN;
      out->blob_n = (int)n;
      return ASNGN_OK;
    }
  }
  return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                      "step: OPEN needs a handle B<n> with n >= 1");
}

/* "CALL <ref>.<cmd> {<args>}": light split only. The ref token runs
 * to the first whitespace or '{'; the command is the part after its LAST
 * '.', so versioned refs split correctly ("fs@1.2.0.read" -> "fs@1.2.0" +
 * "read"). The args object is the first balanced {...} region, scanned
 * with quote awareness ('"' toggles a string, '\\' escapes inside one);
 * trailing text after it is tolerated here and judged by the
 * authoritative parser. */
static asngn_err parse_call(asngn_ctx *c, const char *q, const char *end,
                            asngn_step *out) {
  const char *tok, *tokend, *dot = NULL, *ob, *r, *cb = NULL;
  int depth = 0;
  bool instr = false, esc = false;

  while (q < end && (*q == ' ' || *q == '\t')) q++;
  tok = q;
  while (q < end && *q != ' ' && *q != '\t' && *q != '{') q++;
  tokend = q;
  for (r = tok; r < tokend; r++)
    if (*r == '.') dot = r;
  if (tok == tokend || !dot || dot == tok || dot + 1 == tokend)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: CALL needs a <tool>.<command> reference");
  ob = q;
  while (ob < end && *ob != '{') ob++;
  if (ob == end)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: CALL is missing its {...} args object");
  for (r = ob; r < end; r++) {
    char ch = *r;
    if (instr) {
      if (esc)
        esc = false;
      else if (ch == '\\')
        esc = true;
      else if (ch == '"')
        instr = false;
    } else if (ch == '"') {
      instr = true;
    } else if (ch == '{') {
      depth++;
    } else if (ch == '}') {
      depth--;
      if (depth == 0) {
        cb = r;
        break;
      }
    }
  }
  if (!cb)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: CALL args object is unbalanced");
  out->call_ref = asngn_strndup(tok, (size_t)(dot - tok));
  out->call_cmd = asngn_strndup(dot + 1, (size_t)(tokend - (dot + 1)));
  out->call_args = asngn_strndup(ob, (size_t)(cb - ob) + 1);
  if (!out->call_ref || !out->call_cmd || !out->call_args) {
    asngn_step_free(out);
    return ASNGN_ERR_NOMEM;
  }
  out->kind = ASNGN_STEP_CALL;
  return ASNGN_OK;
}

/* ---- step parser -------------------------------------------- */

asngn_err asngn_step_parse(asngn_ctx *c, const char *line, asngn_step *out) {
  const char *p, *end, *q;

  if (!out) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (!line) return asngn_seterr(c, ASNGN_ERR_INVALID, "step: NULL line");

  p = line;
  while (*p == ' ' || *p == '\t') p++;
  end = p + strlen(p);
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                     end[-1] == '\n'))
    end--;
  if (end == p)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "unrecognized step line");

  if (span_is(p, end, "ANSWER")) {
    out->kind = ASNGN_STEP_ANSWER;
    return ASNGN_OK;
  }
  if ((size_t)(end - p) > 5 && memcmp(p, "OPEN", 4) == 0 &&
      (p[4] == ' ' || p[4] == '\t'))
    return parse_open(c, p + 5, end, out);
  if ((size_t)(end - p) > 5 && memcmp(p, "CALL", 4) == 0 &&
      (p[4] == ' ' || p[4] == '\t'))
    return parse_call(c, p + 5, end, out);
  if ((q = match_kw(p, end, "RECALL")) != NULL)
    return parse_text_step(c, q, end, ASNGN_STEP_RECALL, out);
  if ((q = match_kw(p, end, "THINK")) != NULL)
    return parse_text_step(c, q, end, ASNGN_STEP_THINK, out);
  if ((q = match_kw(p, end, "CLARIFY")) != NULL)
    return parse_text_step(c, q, end, ASNGN_STEP_CLARIFY, out);

  return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "unrecognized step line");
}
