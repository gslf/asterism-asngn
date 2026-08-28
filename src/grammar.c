/*
 * grammar.c — per-turn step-protocol GBNF plus the
 * classify and judge micro-grammars.
 *
 * A decision pass emits one schema-constrained action object on a
 * single line (fixed key order, no string escapes) — the GBNF encoding
 * of the JSON-schema constraint a llama.cpp server would enforce:
 *
 *   {action: "call", why: "…", input: <astools-call>, success: "…",
 *    fallback: "…"}
 *
 * The merged grammar keeps asngn's own productions (root, step, call,
 * recall, open, think, clarify, answer, handle, text, meta, tchar) and
 * grafts the astools export below them: the astools root line is
 * dropped, its "call" rule is renamed to "astools-call", and the
 * remainder is appended verbatim — the grafted production is embedded
 * raw (xCDN args, unquoted) as the value of the call action's `input`
 * key. The astools shared terminals (str, char, int, num, ws, obj,
 * t-*) do not collide with our rule names. The rename is a token scan —
 * a rule name is a maximal run of [a-zA-Z0-9-] — that leaves quoted
 * literals, character classes, and comments untouched.
 *
 * Length note: `text` / `meta` are bounded to ASNGN_STEP_TEXT_MAX /
 * ASNGN_STEP_META_MAX chars via {m,n} repetition, so at the limit the
 * only legal continuation is the closing quote and the sampler
 * completes the object instead of rambling into the decide max_tokens
 * cap. The cap still bounds CALL objects (astools' grafted productions
 * stay unbounded); an output it truncates lacks the newline root
 * requires and is rejected as malformed by the control loop;
 * asngn_step_parse re-caps payloads at the same limits as defense in
 * depth. tchar excludes '"' and '\\' so payloads can never escape
 * their quotes or forge a nested object.
 *
 * Output is deterministic: byte-identical for identical inputs.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <string.h>

/* ---- grafting helpers ---------------------------------------------------- */

static bool rule_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '-';
}

/* Does the astools text define a bare "call" rule beyond its first (root)
 * line? Without one there is nothing to graft — the CALL alternative is
 * dropped rather than emitting a grammar with an undefined rule. */
static bool has_call_rule(const char *gbnf) {
  const char *p = gbnf;
  while (*p != '\0' && *p != '\n') p++; /* skip the astools root line */
  while (*p == '\n') p++;
  while (*p != '\0') {
    const char *q = p;
    while (*q == ' ' || *q == '\t') q++;
    if (strncmp(q, "call", 4) == 0 && !rule_char(q[4])) {
      q += 4;
      while (*q == ' ' || *q == '\t') q++;
      if (strncmp(q, "::=", 3) == 0) {
        q += 3;
        while (*q == ' ' || *q == '\t') q++;
        return strncmp(q, "\"\"", 2) != 0;
      }
    }
    while (*p != '\0' && *p != '\n') p++;
    while (*p == '\n') p++;
  }
  return false;
}

/* Append the astools grammar with its first line (its own root rule)
 * dropped and every rule-name token `call` renamed to `astools-call`.
 * Quoted literals ("..."), character classes ([...]) and # comments are
 * copied verbatim — a "call" inside them is grammar text, not a rule
 * reference. */
static asngn_err graft_astools(asngn_buf *b, const char *gbnf) {
  const char *p = gbnf;
  asngn_err e = ASNGN_OK;

  while (*p != '\0' && *p != '\n') p++;
  if (*p == '\n') p++;

  while (*p != '\0' && e == ASNGN_OK) {
    char ch = *p;
    if (ch == '"' || ch == '[') { /* literal / character class: verbatim */
      char close = (ch == '"') ? '"' : ']';
      const char *start = p++;
      while (*p != '\0' && *p != close) {
        if (*p == '\\' && p[1] != '\0') p++;
        p++;
      }
      if (*p == close) p++;
      e = asngn_buf_append(b, start, (size_t)(p - start));
    } else if (ch == '#') { /* comment to end of line: verbatim */
      const char *start = p;
      while (*p != '\0' && *p != '\n') p++;
      e = asngn_buf_append(b, start, (size_t)(p - start));
    } else if (rule_char(ch)) { /* rule-name token */
      const char *start = p;
      while (rule_char(*p)) p++;
      if ((size_t)(p - start) == 4 && memcmp(start, "call", 4) == 0)
        e = asngn_buf_appends(b, "astools-call");
      else
        e = asngn_buf_append(b, start, (size_t)(p - start));
    } else {
      e = asngn_buf_appendc(b, ch);
      p++;
    }
  }
  if (e != ASNGN_OK) return e;
  /* the astools export ends with exactly one '\n'; normalize anyway */
  if (b->len == 0 || b->data[b->len - 1] != '\n')
    e = asngn_buf_appendc(b, '\n');
  return e;
}

/* ---- step grammar ------------------------------------- */

asngn_err asngn_grammar_steps(asngn_ctx *c, bool with_call, bool with_recall,
                               bool with_think, size_t blobs_n,
                               const char *astools_gbnf, char **out) {
  asngn_buf b;
  asngn_err e = ASNGN_OK;
  bool call_on, open_on;
  size_t i;

  if (!out) return ASNGN_ERR_INVALID;
  *out = NULL;

  call_on = with_call && astools_gbnf != NULL && has_call_rule(astools_gbnf);
  open_on = blobs_n > 0;

  asngn_buf_init(&b);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "root      ::= step \"\\n\"\n");
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "step      ::= ");
  if (e == ASNGN_OK && call_on) e = asngn_buf_appends(&b, "call | ");
  if (e == ASNGN_OK && with_recall) e = asngn_buf_appends(&b, "recall | ");
  if (e == ASNGN_OK && open_on) e = asngn_buf_appends(&b, "open | ");
  if (e == ASNGN_OK && with_think) e = asngn_buf_appends(&b, "think | ");
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "clarify | answer\n");
  if (e == ASNGN_OK && call_on)
    e = asngn_buf_appends(&b,
                          "call      ::= \"{action: \\\"call\\\", why: "
                          "\\\"\" meta \"\\\", input: \" astools-call "
                          "\", success: \\\"\" meta \"\\\", fallback: "
                          "\\\"\" meta \"\\\"}\"\n");
  if (e == ASNGN_OK && with_recall)
    e = asngn_buf_appends(&b,
                          "recall    ::= \"{action: \\\"recall\\\", why: "
                          "\\\"\" meta \"\\\", input: \\\"\" text "
                          "\"\\\", success: \\\"\" meta \"\\\", fallback: "
                          "\\\"\" meta \"\\\"}\"\n");
  if (e == ASNGN_OK && open_on)
    e = asngn_buf_appends(&b,
                          "open      ::= \"{action: \\\"open\\\", why: "
                          "\\\"\" meta \"\\\", input: \\\"\" handle "
                          "\"\\\"}\"\n");
  if (e == ASNGN_OK && with_think)
    e = asngn_buf_appends(&b,
                          "think     ::= \"{action: \\\"think\\\", input: "
                          "\\\"\" text \"\\\"}\"\n");
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                           "clarify   ::= \"{action: \\\"clarify\\\", why: "
                          "\\\"\" meta \"\\\", input: \\\"\" text "
                          "\"\\\"}\"\n"
                          "answer    ::= \"{action: \\\"answer\\\"}\"\n");
  if (e == ASNGN_OK && open_on) {
    e = asngn_buf_appends(&b, "handle    ::= ");
    for (i = 1; e == ASNGN_OK && i <= blobs_n; i++) {
      if (i > 1) e = asngn_buf_appends(&b, " | ");
      if (e == ASNGN_OK) e = asngn_buf_printf(&b, "\"B%zu\"", i);
    }
    if (e == ASNGN_OK) e = asngn_buf_appendc(&b, '\n');
  }
  if (e == ASNGN_OK)
    e = asngn_buf_printf(&b, "text      ::= tchar{1,%d}\n"
                             "meta      ::= tchar{1,%d}\n"
                             "tchar     ::= [^\\\"\\\\\\x0A\\x0D]\n",
                         ASNGN_STEP_TEXT_MAX, ASNGN_STEP_META_MAX);
  if (e == ASNGN_OK && call_on) e = graft_astools(&b, astools_gbnf);

  if (e != ASNGN_OK) {
    asngn_buf_free(&b);
    return asngn_seterr(c, e, "grammar: step grammar assembly failed");
  }
  *out = asngn_buf_detach(&b);
  if (!*out)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "grammar: out of memory");
  return ASNGN_OK;
}

/* ---- classify micro-grammar --------------------------------- */

asngn_err asngn_grammar_classify(char **out) {
  static const char grammar[] =
      "root ::= \"CLASS \" (\"SIMPLE\" | \"MODERATE\" | \"COMPLEX\")"
      " \" | DETAIL \" (\"TERSE\" | \"NORMAL\" | \"RICH\")"
      " \" | MODE \" (\"DIRECT\" | \"PLAN\")"
      " \" | TASK \" (\"CHAT\" | \"LOOKUP\" | \"EXPLAIN\" | \"EDIT\" | "
      "\"BUILD\" | \"GENERATE\" | \"REFACTOR\" | \"DEBUG\") \"\\n\"\n";
  if (!out) return ASNGN_ERR_INVALID;
  *out = asngn_strdup(grammar);
  return *out ? ASNGN_OK : ASNGN_ERR_NOMEM;
}

/* ---- judge micro-grammar ------------------------------ */

asngn_err asngn_grammar_judge(char **out) {
  static const char grammar[] =
      "root ::= \"SCORE \" (\"10\" | [0-9]) \" | \" jtext \"\\n\"\n"
      "jtext ::= jchar (jchar)*\n"
      "jchar ::= [^|\\x0A\\x0D]\n";
  if (!out) return ASNGN_ERR_INVALID;
  *out = asngn_strdup(grammar);
  return *out ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
