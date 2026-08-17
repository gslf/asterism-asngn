/*
 * route.c — turn classification and the tier ladder.
 *
 * Before routing, each turn is classified along three axes — CLASS
 * (reasoning demand), DETAIL (recommended answer size), MODE
 * (whether the step loop is needed at all) — by a deterministic heuristic,
 * a nano-model micro-pass, or a conservative merge of both, per
 * routing.classifier. Classification never fails the turn: any model-pass
 * failure degrades to the heuristic.
 *
 * The tier ladder for escalation is simply the pool declaration
 * order restricted to non-embedding entries: by convention the
 * default pool declares generative models cheapest-first (nano, light,
 * std, ...), so "one tier up" is "next generative pool entry".
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "xcdn.h"

/* ── tiny ASCII helpers (locale-independent, unsigned-char safe) ──────── */

static int route_is_alpha(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int route_is_digit(char ch) { return ch >= '0' && ch <= '9'; }

static int route_is_alnum(char ch) {
  return route_is_alpha(ch) || route_is_digit(ch);
}

static char route_lower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int route_is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* Case-insensitive (ASCII) substring search. */
static const char *route_ci_strstr(const char *hay, const char *needle) {
  size_t nl;
  if (!hay || !needle || needle[0] == '\0') return hay;
  nl = strlen(needle);
  for (; *hay != '\0'; hay++) {
    size_t i;
    for (i = 0; i < nl; i++) {
      if (hay[i] == '\0') return NULL;
      if (route_lower(hay[i]) != route_lower(needle[i])) break;
    }
    if (i == nl) return hay;
  }
  return NULL;
}

/* ── heuristic features ──────────────────────────────────────────────── */

/* Imperative verbs recognized at sentence starts. English plus a few
 * Italian ones (asngn is bilingual by default). Matching is
 * case-insensitive with a right word boundary (next char not a letter),
 * so "crea" does not fire inside "create" — "create" itself matches first
 * as its own entry. */
static const char *const route_verbs[] = {
  "fix",     "write",     "create", "rename",  "delete", "refactor",
  "implement", "build",   "run",    "install", "update", "esegui",
  "scrivi",  "crea",      NULL
};

/* astools tool-name prefixes; a mention strongly suggests tool work. */
static const char *const route_tools[] = {
  "fs.", "grep.", "git.", "proc.", "edit.", "env.", "sys.", NULL
};

/* True when an imperative verb opens the message or any sentence.
 * "Sentence start" = first non-space byte of the message, or the first
 * non-space byte after '.', '!', '?', ';' or a newline. */
static bool route_has_imperative(const char *msg) {
  bool at_start = true;
  size_t i;
  for (i = 0; msg[i] != '\0'; i++) {
    char ch = msg[i];
    if (route_is_space(ch)) {
      if (ch == '\n') at_start = true;
      continue; /* whitespace never clears a pending sentence start */
    }
    if (at_start && route_is_alpha(ch)) {
      size_t v;
      for (v = 0; route_verbs[v] != NULL; v++) {
        size_t k;
        const char *verb = route_verbs[v];
        for (k = 0; verb[k] != '\0'; k++)
          if (route_lower(msg[i + k]) != verb[k]) break;
        if (verb[k] == '\0' && !route_is_alpha(msg[i + k])) return true;
      }
    }
    at_start = (ch == '.' || ch == '!' || ch == '?' || ch == ';');
  }
  return false;
}

/* True when the message mentions a file path or a known tool name.
 * A "file path" is any whitespace-delimited token containing both '/'
 * and '.' (src/main.c, ./run.sh). Tool prefixes match
 * case-sensitively with a left word boundary (start or non-alnum), so
 * "refs." does not count as "fs.". Substring matching past that boundary
 * is deliberate: this is a routing hint, not a parser. */
static bool route_mentions_path_or_tool(const char *msg) {
  size_t i = 0, t;
  while (msg[i] != '\0') {
    bool slash = false, dot = false;
    while (route_is_space(msg[i])) i++;
    while (msg[i] != '\0' && !route_is_space(msg[i])) {
      if (msg[i] == '/') slash = true;
      if (msg[i] == '.') dot = true;
      i++;
    }
    if (slash && dot) return true;
  }
  for (t = 0; route_tools[t] != NULL; t++) {
    const char *p = msg;
    while ((p = strstr(p, route_tools[t])) != NULL) {
      if (p == msg || !route_is_alnum(p[-1])) return true;
      p++;
    }
  }
  return false;
}

/* Math detection: count the ASCII math symbols = + * / ^ and the UTF-8
 * sequences for ∑ (E2 88 91) and √ (E2 88 9A), plus decimal digits.
 * "Math" is at least 2 symbols, at least 4 symbol+digit bytes in total,
 * and a symbol+digit density of at least 5% of the byte length — so prose
 * that merely contains a date or one equals sign does not trip it. */
static bool route_has_math(const char *msg, size_t len) {
  size_t i, sym = 0, dig = 0;
  for (i = 0; i < len; i++) {
    char ch = msg[i];
    if (ch == '=' || ch == '+' || ch == '*' || ch == '/' || ch == '^')
      sym++;
    else if (route_is_digit(ch))
      dig++;
    else if ((unsigned char)ch == 0xE2 && i + 2 < len &&
             (unsigned char)msg[i + 1] == 0x88 &&
             ((unsigned char)msg[i + 2] == 0x91 ||   /* ∑ */
              (unsigned char)msg[i + 2] == 0x9A)) {  /* √ */
      sym++;
      i += 2;
    }
  }
  return sym >= 2 && sym + dig >= 4 && (sym + dig) * 20 >= len;
}

static size_t route_count_questions(const char *msg) {
  size_t n = 0;
  for (; *msg != '\0'; msg++)
    if (*msg == '?') n++;
  return n;
}

/* ── heuristic classification ────────────────────────────────────────── */

/*
 * Rule table (evaluated top to bottom per axis; the first matching row
 * wins — COMPLEX before SIMPLE so escalation history is conservative):
 *
 *  CLASS ┌────────────────────────────────────────────────────────────┐
 *        │ COMPLEX  when len > 900, or a ``` fence is present, or     │
 *        │          (math and len > 300), or recent_escalation.       │
 *        │ SIMPLE   when len < 240 and no fence and no math and       │
 *        │          at most 1 question mark.                          │
 *        │ MODERATE otherwise.                                        │
 *  MODE  ├────────────────────────────────────────────────────────────┤
 *        │ PLAN     when tools are available this turn and the        │
 *        │          message has an imperative verb at a sentence      │
 *        │          start, or mentions a file path / tool name, or    │
 *        │          contains a fence.                                 │
 *        │ DIRECT   otherwise (no tools ⇒ always DIRECT).             │
 *  DETAIL├────────────────────────────────────────────────────────────┤
 *        │ RICH     when len > 600, or fence, or the message asks     │
 *        │          for depth ("explain", "spiega", "in detail",      │
 *        │          "in dettaglio", case-insensitive substrings) —    │
 *        │          a depth ask wins even on a short message (        │
 *        │          explicit cues override DETAIL).                   │
 *        │ TERSE    when len < 80 and no fence.                       │
 *        │ NORMAL   otherwise.                                        │
 *        └────────────────────────────────────────────────────────────┘
 *
 * len is the byte length of the UTF-8 message. Pure and deterministic:
 * no config, no clock, no allocation.
 */
void asngn_route_heuristic(const char *message, bool tools_available,
                           bool recent_escalation,
                           asngn_route_profile *out) {
  size_t len, questions;
  bool fence, math, imperative, pathtool;

  if (out == NULL) return;
  if (message == NULL) message = "";

  len = strlen(message);
  fence = strstr(message, "```") != NULL;
  math = route_has_math(message, len);
  questions = route_count_questions(message);
  imperative = route_has_imperative(message);
  pathtool = route_mentions_path_or_tool(message);

  if (len > 900 || fence || (math && len > 300) || recent_escalation)
    out->klass = ASNGN_CLASS_COMPLEX;
  else if (len < 240 && !math && questions <= 1)
    out->klass = ASNGN_CLASS_SIMPLE;
  else
    out->klass = ASNGN_CLASS_MODERATE;

  if (tools_available && (imperative || pathtool || fence))
    out->mode = ASNGN_MODE_PLAN;
  else
    out->mode = ASNGN_MODE_DIRECT;

  if (len > 600 || fence ||
      route_ci_strstr(message, "explain") != NULL ||
      route_ci_strstr(message, "spiega") != NULL ||
      route_ci_strstr(message, "in detail") != NULL ||
      route_ci_strstr(message, "in dettaglio") != NULL)
    out->detail = ASNGN_DETAIL_RICH;
  else if (len < 80)
    out->detail = ASNGN_DETAIL_TERSE;
  else
    out->detail = ASNGN_DETAIL_NORMAL;
}

/* ── the nano micro-pass ─────────────────────────────────────────────── */

/* Truncation limit for the user prompt handed to the router model. */
#define ROUTE_MSG_MAX 2000

static const char ROUTE_SYS[] =
    "You classify one user message for an assistant. "
    "Answer with exactly one line.";

/* Largest prefix of at most ROUTE_MSG_MAX bytes that does not split a
 * UTF-8 sequence (continuation bytes are 10xxxxxx). */
static size_t route_utf8_prefix(const char *s, size_t len) {
  size_t n = len < (size_t)ROUTE_MSG_MAX ? len : (size_t)ROUTE_MSG_MAX;
  if (n == len) return n;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

/* Tolerant parse of "CLASS <c> | DETAIL <d> | MODE <m>": the grammar
 * guarantees the shape, but this is defense in depth — plain
 * strstr on the keyword tokens, all three axes required. */
static bool route_parse_line(const char *line, asngn_route_profile *out) {
  if (line == NULL) return false;

  if (strstr(line, "SIMPLE") != NULL)        out->klass = ASNGN_CLASS_SIMPLE;
  else if (strstr(line, "MODERATE") != NULL) out->klass = ASNGN_CLASS_MODERATE;
  else if (strstr(line, "COMPLEX") != NULL)  out->klass = ASNGN_CLASS_COMPLEX;
  else return false;

  if (strstr(line, "TERSE") != NULL)         out->detail = ASNGN_DETAIL_TERSE;
  else if (strstr(line, "NORMAL") != NULL)   out->detail = ASNGN_DETAIL_NORMAL;
  else if (strstr(line, "RICH") != NULL)     out->detail = ASNGN_DETAIL_RICH;
  else return false;

  if (strstr(line, "PLAN") != NULL)          out->mode = ASNGN_MODE_PLAN;
  else if (strstr(line, "DIRECT") != NULL)   out->mode = ASNGN_MODE_DIRECT;
  else return false;

  return true;
}

/* One grammar-constrained router-model pass. false on any failure — the
 * caller falls back to the heuristic and the turn never fails here. */
static bool route_model_pass(asngn_ctx *c, const char *message,
                             asngn_turn_state *t, asngn_route_profile *out,
                             size_t *aux_tokens) {
  int slot, tin = 0, tout = 0;
  char *gbnf = NULL, *text = NULL, *msg = NULL;
  const char *user;
  size_t len, cut;
  bool ok;

  slot = asngn_models_slot_for_role(c, ASNGN_ROLE_ROUTER);
  if (slot < 0) return false;
  if (asngn_grammar_classify(&gbnf) != ASNGN_OK) return false;

  len = strlen(message);
  cut = route_utf8_prefix(message, len);
  if (cut < len) {
    msg = asngn_strndup(message, cut);
    if (msg == NULL) {
      free(gbnf);
      return false;
    }
    user = msg;
  } else {
    user = message;
  }

  ok = asngn_models_generate(c, slot, ASNGN_TASK_CLASSIFY, ROUTE_SYS, user,
                             gbnf, 0, NULL, NULL, &t->cancel, &text, &tin,
                             &tout) == ASNGN_OK;
  free(gbnf);
  free(msg);
  if (ok && aux_tokens != NULL && tout > 0) *aux_tokens += (size_t)tout;
  ok = ok && route_parse_line(text, out);
  free(text);
  return ok;
}

/* ── telemetry (kind "classify") ──────────────────────────────────────── */

static const char *route_class_name(asngn_class k) {
  switch (k) {
    case ASNGN_CLASS_SIMPLE:   return "simple";
    case ASNGN_CLASS_MODERATE: return "moderate";
    case ASNGN_CLASS_COMPLEX:  return "complex";
    default:                   return "moderate";
  }
}

static const char *route_mode_name(asngn_mode m) {
  return m == ASNGN_MODE_PLAN ? "plan" : "direct";
}

/* Emit the classify event: data { class, detail, mode, source }. A build
 * failure degrades to an empty data record — telemetry never fails the
 * caller. */
static void route_emit(asngn_ctx *c, asngn_session *s,
                       const asngn_route_profile *p, const char *source) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node = NULL;
  asngn_buf buf;
  bool ok = obj != NULL;

  asngn_buf_init(&buf);
  ok = ok && asngn_xobj_put(obj, "class",
                            xcdn_value_string(route_class_name(p->klass)));
  ok = ok && asngn_xobj_put(obj, "detail",
                            xcdn_value_string(asngn_detail_name(p->detail)));
  ok = ok && asngn_xobj_put(obj, "mode",
                            xcdn_value_string(route_mode_name(p->mode)));
  ok = ok && asngn_xobj_put(obj, "source", xcdn_value_string(source));
  if (ok) {
    node = xcdn_node_new(obj);
    if (node != NULL) obj = NULL;
  }
  if (node != NULL && asngn_xnode_write(node, false, &buf) == ASNGN_OK &&
      buf.len > 0)
    asngn_tele_emit(c, "classify", NULL, NULL, s->slug, 0, buf.data);
  else
    asngn_tele_emit(c, "classify", NULL, NULL, s->slug, 0, NULL);
  if (node != NULL) xcdn_node_free(node);
  if (obj != NULL) xcdn_value_free(obj);
  asngn_buf_free(&buf);
}

/* ── full classification ─────────────────────────────────────────────── */

asngn_err asngn_route_classify(asngn_ctx *c, asngn_session *s,
                               const char *message, asngn_turn_state *t,
                               asngn_route_profile *out,
                               size_t *aux_tokens) {
  asngn_route_profile heur, model;
  const char *source = "heuristic";
  bool tools_available, recent_escalation;

  if (c == NULL || s == NULL || t == NULL || out == NULL)
    return ASNGN_ERR_INVALID;
  if (message == NULL) message = "";

  tools_available = c->astools_ok && !t->opts.no_tools;
  /* TODO: recent escalation history should be read from the last
   * few ledger entries of the session; deliberately false until the loop
   * writes them — the heuristic stays conservative without it. */
  recent_escalation = false;

  asngn_route_heuristic(message, tools_available, recent_escalation, &heur);
  *out = heur;

  switch (c->cfg.classifier) {
    case ASNGN_CLASSIFIER_HEURISTIC:
      break;
    case ASNGN_CLASSIFIER_MODEL:
      /* Model verdict alone; heuristic only as the failure fallback. */
      if (route_model_pass(c, message, t, &model, aux_tokens)) {
        if (!tools_available) model.mode = ASNGN_MODE_DIRECT; /* gate */
        *out = model;
        source = "model";
      }
      break;
    case ASNGN_CLASSIFIER_HYBRID:
    default:
      /* Conservative merge: higher CLASS wins; MODE PLAN wins
       * over DIRECT (still gated on tool availability); DETAIL is the
       * model's vote unless the heuristic says RICH — i.e. the max of
       * the two in the order TERSE < NORMAL < RICH (the enum order). */
      if (route_model_pass(c, message, t, &model, aux_tokens)) {
        out->klass = model.klass > heur.klass ? model.klass : heur.klass;
        out->mode = (model.mode == ASNGN_MODE_PLAN && tools_available)
                        ? ASNGN_MODE_PLAN
                        : heur.mode;
        out->detail =
            model.detail > heur.detail ? model.detail : heur.detail;
        source = "hybrid";
      }
      break;
  }

  route_emit(c, s, out, source);
  return ASNGN_OK;
}

/* ── tier ladder ─────────────────────────────────────────────────────── */

/* The ladder is the pool declaration order restricted to non-embedding
 * entries. By convention the default pool is declared
 * cheapest-first (nano, light, std — the embedder is skipped), so
 * declaration order and capability order coincide; a config that declares
 * generative models out of order gets the ladder it declared. */

int asngn_route_tier_up(asngn_ctx *c, int slot) {
  size_t i;
  if (c == NULL || slot < 0 || (size_t)slot >= c->cfg.pool_n) return -1;
  for (i = (size_t)slot + 1; i < c->cfg.pool_n; i++)
    if (!c->cfg.pool[i].embedding) return (int)i;
  return -1;
}

int asngn_route_tier_down(asngn_ctx *c, int slot) {
  size_t i;
  if (c == NULL || slot <= 0 || (size_t)slot >= c->cfg.pool_n) return -1;
  for (i = (size_t)slot; i-- > 0;)
    if (!c->cfg.pool[i].embedding) return (int)i;
  return -1;
}
