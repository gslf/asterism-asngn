/*
 * steps.c — step names, step ownership, and the defense-in-depth parser
 * over the action-object step grammar.
 *
 * A decision pass emits one single-line schema-constrained action
 * object (fixed key order, quoted values without escapes; the call
 * input embeds the astools call production raw):
 *
 *   {action: "call", why: "…", input: <ref>.<cmd> {…}, success: "…",
 *    fallback: "…"}
 *
 * The per-turn grammar already constrains what the model can emit; the
 * plan gate re-validates every line anyway. This parser is strict on
 * structure — required keys per action, no unknown or duplicate keys,
 * no string escapes — and flexible on whitespace. The CALL input gets
 * a light split only (ref / command / args object); the authoritative
 * CALL parse is astools_call_parse on the synthesized call line in the
 * control loop.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- names and ownership ------------------------------------------------- */

const char *asngn_step_name(asngn_step_kind k) {
  switch (k) {
    case ASNGN_STEP_CALL:    return "call";
    case ASNGN_STEP_RECALL:  return "recall";
    case ASNGN_STEP_OPEN:    return "open";
    case ASNGN_STEP_THINK:   return "think";
    case ASNGN_STEP_CLARIFY: return "clarify";
    case ASNGN_STEP_ANSWER:  return "answer";
  }
  return "unknown";
}

void asngn_step_free(asngn_step *st) {
  if (!st) return;
  free(st->text);
  free(st->why);
  free(st->success);
  free(st->fallback);
  free(st->call_ref);
  free(st->call_cmd);
  free(st->call_args);
  memset(st, 0, sizeof(*st));
}

/* ---- parsing helpers ----------------------------------------------------- */

static void skip_ws(const char **p, const char *end) {
  while (*p < end && (**p == ' ' || **p == '\t')) (*p)++;
}

static bool span_eq(const char *p, size_t n, const char *lit) {
  return strlen(lit) == n && memcmp(p, lit, n) == 0;
}

/* Quoted value without escapes: '"' payload '"'. A backslash or an
 * embedded newline is a protocol violation (the grammar's tchar
 * excludes both). The payload is duplicated with a byte cap on a UTF-8
 * boundary — the grammar bounds it already; the cap is defense in
 * depth against non-grammar sources. */
static asngn_err parse_quoted(asngn_ctx *c, const char **p, const char *end,
                              const char *what, size_t cap, char **out) {
  const char *q, *r;
  size_t n;

  if (*p >= end || **p != '"')
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: %s needs a quoted value", what);
  q = *p + 1;
  for (r = q; r < end && *r != '"'; r++) {
    if (*r == '\\' || *r == '\n' || *r == '\r')
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                          "step: %s value contains a forbidden character",
                          what);
  }
  if (r >= end)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: %s value is missing its closing quote", what);
  n = (size_t)(r - q);
  if (n == 0)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: %s value is empty",
                        what);
  if (n > cap) {
    n = cap;
    while (n > 0 && ((unsigned char)q[n] & 0xC0) == 0x80)
      n--; /* back off to a UTF-8 boundary */
    if (n == 0)
      return asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: %s value is empty",
                          what);
  }
  *out = asngn_strndup(q, n);
  if (!*out) return ASNGN_ERR_NOMEM;
  *p = r + 1;
  return ASNGN_OK;
}

/* Raw call value "<ref>.<cmd> {<args>}": the ref token runs to the
 * first whitespace or '{'; the command is the part after its LAST '.',
 * so versioned refs split correctly ("fs@1.2.0.read" -> "fs@1.2.0" +
 * "read"). The args object is the first balanced {...} region, scanned
 * with quote awareness ('"' toggles a string, '\\' escapes inside one). */
static asngn_err parse_call_value(asngn_ctx *c, const char **p,
                                  const char *end, asngn_step *out) {
  const char *tok, *tokend, *dot = NULL, *ob, *r, *cb = NULL;
  int depth = 0;
  bool instr = false, esc = false;
  const char *q = *p;

  tok = q;
  while (q < end && *q != ' ' && *q != '\t' && *q != '{') q++;
  tokend = q;
  for (r = tok; r < tokend; r++)
    if (*r == '.') dot = r;
  if (tok == tokend || !dot || dot == tok || dot + 1 == tokend)
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: call input needs a <tool>.<command> "
                        "reference");
  ob = q;
  while (ob < end && (*ob == ' ' || *ob == '\t')) ob++;
  if (ob >= end || *ob != '{')
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "step: call input is missing its {...} args "
                        "object");
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
                        "step: call args object is unbalanced");
  out->call_ref = asngn_strndup(tok, (size_t)(dot - tok));
  out->call_cmd = asngn_strndup(dot + 1, (size_t)(tokend - (dot + 1)));
  out->call_args = asngn_strndup(ob, (size_t)(cb - ob) + 1);
  if (!out->call_ref || !out->call_cmd || !out->call_args)
    return ASNGN_ERR_NOMEM;
  *p = cb + 1;
  return ASNGN_OK;
}

/* "B<n>", n >= 1, nothing else in the payload. */
static asngn_err parse_handle(asngn_ctx *c, const char *s, int *out_n) {
  long n = 0;
  int digits = 0;
  if (s != NULL && *s == 'B') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (digits < 9) n = n * 10 + (*s - '0');
      digits++;
      s++;
    }
    if (digits >= 1 && digits <= 9 && *s == '\0' && n >= 1) {
      *out_n = (int)n;
      return ASNGN_OK;
    }
  }
  return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                      "step: open input needs a handle B<n> with n >= 1");
}

static bool action_kind(const char *name, asngn_step_kind *out) {
  if (strcmp(name, "call") == 0)    { *out = ASNGN_STEP_CALL;    return true; }
  if (strcmp(name, "recall") == 0)  { *out = ASNGN_STEP_RECALL;  return true; }
  if (strcmp(name, "open") == 0)    { *out = ASNGN_STEP_OPEN;    return true; }
  if (strcmp(name, "think") == 0)   { *out = ASNGN_STEP_THINK;   return true; }
  if (strcmp(name, "clarify") == 0) { *out = ASNGN_STEP_CLARIFY; return true; }
  if (strcmp(name, "answer") == 0)  { *out = ASNGN_STEP_ANSWER;  return true; }
  return false;
}

/* ---- step parser -------------------------------------------- */

asngn_err asngn_step_parse(asngn_ctx *c, const char *line, asngn_step *out) {
  const char *p, *end;
  asngn_err e;
  char *action = NULL;
  bool have_call_input = false;

  if (!out) return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (!line) return asngn_seterr(c, ASNGN_ERR_INVALID, "step: NULL line");

  p = line;
  while (*p == ' ' || *p == '\t') p++;
  end = p + strlen(p);
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                     end[-1] == '\n'))
    end--;
  if (end == p || *p != '{') {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: not an action object");
    goto fail;
  }
  p++;
  skip_ws(&p, end);

  /* first key must be `action` */
  if ((size_t)(end - p) < 6 || memcmp(p, "action", 6) != 0) {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                     "step: the object must start with the action key");
    goto fail;
  }
  p += 6;
  skip_ws(&p, end);
  if (p >= end || *p != ':') {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: action needs a value");
    goto fail;
  }
  p++;
  skip_ws(&p, end);
  e = parse_quoted(c, &p, end, "action", 16, &action);
  if (e != ASNGN_OK) goto fail;
  if (!action_kind(action, &out->kind)) {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: unknown action \"%s\"",
                     action);
    goto fail;
  }

  /* remaining keys: `, key: value` until '}' */
  for (;;) {
    const char *k;
    size_t kn;
    skip_ws(&p, end);
    if (p < end && *p == '}') {
      p++;
      break;
    }
    if (p >= end || *p != ',') {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: action object is unterminated");
      goto fail;
    }
    p++;
    skip_ws(&p, end);
    k = p;
    while (p < end && *p >= 'a' && *p <= 'z') p++;
    kn = (size_t)(p - k);
    skip_ws(&p, end);
    if (kn == 0 || p >= end || *p != ':') {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL, "step: malformed key");
      goto fail;
    }
    p++;
    skip_ws(&p, end);
    if (span_eq(k, kn, "why") && out->why == NULL) {
      e = parse_quoted(c, &p, end, "why", ASNGN_STEP_META_MAX, &out->why);
    } else if (span_eq(k, kn, "success") && out->success == NULL) {
      e = parse_quoted(c, &p, end, "success", ASNGN_STEP_META_MAX,
                       &out->success);
    } else if (span_eq(k, kn, "fallback") && out->fallback == NULL) {
      e = parse_quoted(c, &p, end, "fallback", ASNGN_STEP_META_MAX,
                       &out->fallback);
    } else if (span_eq(k, kn, "input") && out->text == NULL &&
               !have_call_input) {
      if (out->kind == ASNGN_STEP_CALL) {
        e = parse_call_value(c, &p, end, out);
        have_call_input = (e == ASNGN_OK);
      } else {
        e = parse_quoted(c, &p, end, "input", ASNGN_STEP_TEXT_MAX,
                         &out->text);
      }
    } else {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: unknown or repeated key \"%.*s\"", (int)kn, k);
    }
    if (e != ASNGN_OK) goto fail;
  }
  skip_ws(&p, end);
  if (p != end) {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                     "step: trailing text after the action object");
    goto fail;
  }

  /* required fields per action */
  switch (out->kind) {
  case ASNGN_STEP_ANSWER:
    if (out->why != NULL || out->text != NULL || out->success != NULL ||
        out->fallback != NULL) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: answer takes no other fields");
      goto fail;
    }
    break;
  case ASNGN_STEP_THINK:
    if (out->text == NULL || out->why != NULL || out->success != NULL ||
        out->fallback != NULL) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: think needs exactly {action, input}");
      goto fail;
    }
    break;
  case ASNGN_STEP_CLARIFY:
  case ASNGN_STEP_OPEN:
    if (out->text == NULL || out->why == NULL || out->success != NULL ||
        out->fallback != NULL) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: %s needs exactly {action, why, input}",
                       asngn_step_name(out->kind));
      goto fail;
    }
    if (out->kind == ASNGN_STEP_OPEN) {
      e = parse_handle(c, out->text, &out->blob_n);
      if (e != ASNGN_OK) goto fail;
    }
    break;
  case ASNGN_STEP_RECALL:
    if (out->text == NULL || out->why == NULL || out->success == NULL ||
        out->fallback == NULL) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: recall needs {action, why, input, success, "
                       "fallback}");
      goto fail;
    }
    break;
  case ASNGN_STEP_CALL:
    if (!have_call_input || out->why == NULL || out->success == NULL ||
        out->fallback == NULL) {
      e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                       "step: call needs {action, why, input, success, "
                       "fallback}");
      goto fail;
    }
    break;
  }

  free(action);
  return ASNGN_OK;

fail:
  free(action);
  asngn_step_free(out);
  return e;
}
