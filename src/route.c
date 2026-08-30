/*
 * route.c — evidence-scored turn classification and the tier ladder.
 *
 * Before routing, each turn is classified along three axes — CLASS
 * (reasoning demand), DETAIL (recommended answer size), MODE
 * (whether the step loop is needed at all) — by a deterministic
 * evidence-scored heuristic, a nano-model micro-pass, or a conservative
 * merge of both, per routing.classifier. Classification never fails the
 * turn: any model-pass failure degrades to the heuristic.
 *
 * CLASS is an additive score over evidence, not a byte count: the task
 * kind read off the message (chat … debug), the tool families it
 * implies, the programming language involved, the size of the bound
 * workspace, the session's recent reliability (ledger window), and the
 * eval-suite calibration when one has been recorded. Message shape
 * (fences, math, length, question density) still contributes, but as
 * one signal among several — a hard problem stated in one short line
 * routes on what it asks for, not on how many bytes it took to ask.
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

/* Whole-word case-insensitive hit: both neighbors non-alphanumeric.
 * Entries may contain spaces ("stack trace") — interior boundaries are
 * the entry's own. */
static bool route_word(const char *msg, const char *needle) {
  const char *p = msg;
  size_t nl = strlen(needle);
  while ((p = route_ci_strstr(p, needle)) != NULL) {
    if ((p == msg || !route_is_alnum(p[-1])) && !route_is_alnum(p[nl]))
      return true;
    p++;
  }
  return false;
}

/* First whole-word hit from a NULL-terminated list. */
static bool route_word_any(const char *msg, const char *const *list) {
  size_t i;
  for (i = 0; list[i] != NULL; i++)
    if (route_word(msg, list[i])) return true;
  return false;
}

/* ── message features ────────────────────────────────────────────────── */

/* Imperative verbs recognized at sentence starts. Matching is
 * case-insensitive with a right word boundary (next char not a letter),
 * so an entry never fires inside a longer word. */
static const char *const route_verbs[] = {
  "fix",     "write",     "create", "rename",  "delete",  "refactor",
  "implement", "build",   "run",    "install", "update",  "list",
  "read",    "show",      "open",   "search",  "find",    "save",
  "copy",    "move",      "remove", "edit",    "use",     NULL
};

/* astools tool-name prefixes; a mention selects that family directly. */
static const struct { const char *prefix; unsigned family; } route_tools[] = {
  {"fs.", ASNGN_TOOLF_FS},     {"grep.", ASNGN_TOOLF_GREP},
  {"git.", ASNGN_TOOLF_GIT},   {"proc.", ASNGN_TOOLF_PROC},
  {"edit.", ASNGN_TOOLF_EDIT}, {"env.", ASNGN_TOOLF_PROC},
  {"sys.", ASNGN_TOOLF_PROC},  {NULL, 0}
};

/* Workspace nouns (whole-word, case-insensitive). With tools available,
 * a question about files or the workspace needs the step loop even
 * without an imperative ("what files are in the root?") — answering
 * it direct can only hallucinate. Over-calling PLAN costs one decision
 * pass; under-calling it costs a wrong answer. */
static const char *const route_nouns[] = {
  "file",      "files",  "workspace",
  "directory", "folder", "folders",   "repo",     "repository",
  NULL
};

static bool route_has_noun(const char *msg) {
  return route_word_any(msg, route_nouns);
}

static bool route_has_local_anchor(const char *msg) {
  static const char *const anchors[] = {
      "workspace", "this repo", "current repo", "local repo",
      "this directory", "current directory", NULL};
  size_t i;
  for (i = 0; anchors[i] != NULL; i++)
    if (route_ci_strstr(msg, anchors[i]) != NULL) return true;
  return false;
}

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

/* True when the message mentions a file path: any whitespace-delimited
 * token containing both '/' and '.' (src/main.c, ./run.sh). */
static bool route_mentions_path(const char *msg) {
  size_t i = 0;
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
  return false;
}

/* Tool families named explicitly ("fs.read", "grep."). Prefixes match
 * case-sensitively with a left word boundary (start or non-alnum), so
 * "refs." does not count as "fs.". Substring matching past that boundary
 * is deliberate: this is a routing hint, not a parser. */
static unsigned route_explicit_tools(const char *msg) {
  unsigned mask = 0;
  size_t t;
  for (t = 0; route_tools[t].prefix != NULL; t++) {
    const char *p = msg;
    while ((p = strstr(p, route_tools[t].prefix)) != NULL) {
      if (p == msg || !route_is_alnum(p[-1])) {
        mask |= route_tools[t].family;
        break;
      }
      p++;
    }
  }
  return mask;
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

/* ── task kind ───────────────────────────────────────────────────────── */

/* Keyword families, whole-word case-insensitive. A message can
 * hit several; detection resolves by fixed priority (a message that both
 * fixes and builds is a debug turn — the fix is the demanding part). */

static const char *const kw_debug[] = {
  "fix",      "bug",       "bugs",     "debug",     "error",   "errors",
  "crash",    "segfault",  "leak",
  "traceback", "stack trace", "stacktrace", "exception",
  "fails",    "failing",   "failed",
  "broken",   "diagnose",  "regression", NULL
};

static const char *const kw_refactor[] = {
  "refactor", "restructure",
  "simplify", "cleanup",     "clean up",
  "rewrite",  "extract", NULL
};

/* BUILD fires on a strong toolchain word alone, or a run verb paired
 * with a runnable object. */
static const char *const kw_build_strong[] = {
  "compile", "recompile", "build", "rebuild",
  "cmake",   "ctest",   "makefile",  "gcc",       "clang", "msvc",
  "cargo",   "pytest",  "unittest",  "link",  NULL
};
static const char *const kw_run_verb[] = {
  "run", "execute", NULL
};
static const char *const kw_run_obj[] = {
  "test", "tests", "suite", "testsuite", "binary",
  "program", "script", "benchmark", NULL
};

static const char *const kw_gen_verb[] = {
  "write", "create", "implement",
  "generate", "add", "develop", NULL
};
static const char *const kw_code_obj[] = {
  "function", "functions", "class",
  "method",   "test",  "tests",
  "script",   "module",   "library", "api",
  "endpoint", "parser",   "cli",      "struct",
  "program",  "code",     NULL
};

static const char *const kw_edit_verb[] = {
  "edit", "change", "update",
  "rename", "delete", "remove",
  "move", "replace", NULL
};

static const char *const kw_explain[] = {
  "explain", "describe",
  "summarize", "summarise", "what does",
  "how does", "why", "analyze",
  "analyse", "review", "compare", NULL
};

static const char *const kw_interrogative[] = {
  "what", "which", "when", "where", "who", "how", NULL
};

static const char *const kw_search_verb[] = {
  "search", "grep", "find", "locate", NULL
};

static const char *const kw_git[] = {
  "git", "commit", "branch", "diff", "merge", "rebase", "stash",
  "checkout", NULL
};

static const char *route_detect_lang(const char *msg);

/* A generation verb needs code evidence to route GENERATE: a code-object
 * noun, or a programming language named in the message. The language
 * covers the open-ended objects no noun list can ("write a calculator in
 * c++", "write a sudoku in python") — naming the language is what says
 * the deliverable is code. */
static asngn_route_task route_detect_task(const char *msg, bool fence,
                                          size_t questions) {
  bool target = route_mentions_path(msg) || route_has_noun(msg) ||
                route_word_any(msg, kw_code_obj);
  if (route_word_any(msg, kw_debug))    return ASNGN_RTASK_DEBUG;
  if (route_word_any(msg, kw_refactor)) return ASNGN_RTASK_REFACTOR;
  if (route_word_any(msg, kw_gen_verb) &&
      (route_word_any(msg, kw_code_obj) || route_detect_lang(msg)[0] != '\0'))
    return ASNGN_RTASK_GENERATE;
  if (route_word_any(msg, kw_build_strong) ||
      (route_word_any(msg, kw_run_verb) && route_word_any(msg, kw_run_obj)))
    return ASNGN_RTASK_BUILD;
  if (route_word_any(msg, kw_edit_verb) && target)
    return ASNGN_RTASK_EDIT;
  if (route_word_any(msg, kw_explain))  return ASNGN_RTASK_EXPLAIN;
  if (questions > 0 || route_word_any(msg, kw_interrogative))
    return ASNGN_RTASK_LOOKUP;
  (void)fence;
  return ASNGN_RTASK_CHAT;
}

/* A short follow-up that asks where/what a previously mentioned artifact is
 * must not inherit the previous turn's mutating intent.  This was the cause
 * of "give me the full path" being classified as GENERATE and entering a
 * fresh fs.write loop.  Keep the list deliberately about reference/status
 * questions; operational requests in other languages remain the semantic
 * router model's responsibility. */
static bool route_is_reference_followup(const asngn_session *s,
                                        const char *message,
                                        const asngn_route_profile *heur) {
  static const char *const cues[] = {
      "full path", "absolute path", "path of", "where is", "where are",
      "which file", "what file", "location of", "dove", "percorso",
      "posizione", "qual e il path", "qual e il percorso", NULL};
  size_t i;
  bool prior_assistant = false;

  if (s == NULL || message == NULL || heur == NULL ||
      (heur->task != ASNGN_RTASK_LOOKUP &&
       heur->task != ASNGN_RTASK_CHAT))
    return false;
  for (i = s->log_n; i > 0; i--) {
    const asngn_turn *tr = &s->log[i - 1];
    if (strcmp(tr->role, "assistant") == 0) {
      prior_assistant = true;
      break;
    }
  }
  if (!prior_assistant) return false;
  for (i = 0; cues[i] != NULL; i++)
    if (route_ci_strstr(message, cues[i]) != NULL) return true;
  return false;
}

const char *asngn_route_task_name(asngn_route_task k) {
  switch (k) {
    case ASNGN_RTASK_CHAT:     return "chat";
    case ASNGN_RTASK_LOOKUP:   return "lookup";
    case ASNGN_RTASK_EXPLAIN:  return "explain";
    case ASNGN_RTASK_EDIT:     return "edit";
    case ASNGN_RTASK_BUILD:    return "build";
    case ASNGN_RTASK_GENERATE: return "generate";
    case ASNGN_RTASK_REFACTOR: return "refactor";
    case ASNGN_RTASK_DEBUG:    return "debug";
    default:                   return "chat";
  }
}

/* ── implied tool families ───────────────────────────────────────────── */

static unsigned route_task_tools(asngn_route_task task) {
  switch (task) {
    case ASNGN_RTASK_DEBUG:
      return ASNGN_TOOLF_FS | ASNGN_TOOLF_GREP | ASNGN_TOOLF_PROC |
             ASNGN_TOOLF_EDIT;
    case ASNGN_RTASK_REFACTOR:
      return ASNGN_TOOLF_FS | ASNGN_TOOLF_GREP | ASNGN_TOOLF_EDIT;
    case ASNGN_RTASK_GENERATE:
      /* New code is not complete merely because bytes were written. */
      return ASNGN_TOOLF_FS | ASNGN_TOOLF_EDIT | ASNGN_TOOLF_PROC;
    case ASNGN_RTASK_BUILD:
      return ASNGN_TOOLF_PROC;
    case ASNGN_RTASK_EDIT:
      return ASNGN_TOOLF_FS | ASNGN_TOOLF_EDIT;
    default:
      return 0;
  }
}

/* This is an execution invariant, not language classification: once the
 * semantic classifier says the task is operational, a tool-capable engine
 * must enter the action loop even if a small model inconsistently voted
 * DIRECT on the separate MODE axis. */
static bool route_task_requires_tools(asngn_route_task task) {
  return task == ASNGN_RTASK_GENERATE || task == ASNGN_RTASK_EDIT ||
         task == ASNGN_RTASK_BUILD || task == ASNGN_RTASK_DEBUG ||
         task == ASNGN_RTASK_REFACTOR;
}

/* The semantic pass may recognize intent that the English-biased heuristic
 * missed, but it must not erase an operational or informational intent that
 * deterministic evidence already established.  In particular, a noisy
 * router must never turn GENERATE into CHAT and thereby bypass the artifact
 * and verification gates. */
static asngn_route_task route_merge_task(asngn_route_task heur,
                                         asngn_route_task model) {
  if (heur == ASNGN_RTASK_CHAT) return model;
  if (model == ASNGN_RTASK_CHAT) return heur;
  if (route_task_requires_tools(heur) &&
      !route_task_requires_tools(model))
    return heur;
  return model;
}

/* Families the message itself evidences (explicit names, paths, search
 * and git vocabulary). Kept apart from route_task_tools so the CLASS
 * multi-tool bonus never double-counts the task kind's own weight. */
static unsigned route_msg_tools(const char *msg) {
  unsigned mask = route_explicit_tools(msg);
  if (route_mentions_path(msg) || route_has_noun(msg)) mask |= ASNGN_TOOLF_FS;
  if (route_word_any(msg, kw_search_verb)) mask |= ASNGN_TOOLF_GREP;
  if (route_word_any(msg, kw_git)) mask |= ASNGN_TOOLF_GIT;
  return mask;
}

static int route_popcount(unsigned mask) {
  int n = 0;
  for (; mask != 0; mask >>= 1) n += (int)(mask & 1u);
  return n;
}

static void route_tools_string(unsigned mask, char *out, size_t cap) {
  static const struct { unsigned f; const char *name; } names[] = {
      {ASNGN_TOOLF_FS, "fs"},     {ASNGN_TOOLF_GREP, "grep"},
      {ASNGN_TOOLF_GIT, "git"},   {ASNGN_TOOLF_PROC, "proc"},
      {ASNGN_TOOLF_EDIT, "edit"}, {0, NULL}};
  size_t i, off = 0;
  out[0] = '\0';
  for (i = 0; names[i].name != NULL; i++) {
    if ((mask & names[i].f) == 0) continue;
    off += (size_t)snprintf(out + off, cap > off ? cap - off : 0, "%s%s",
                            off > 0 ? "+" : "", names[i].name);
  }
  if (out[0] == '\0') snprintf(out, cap, "none");
}

/* ── language ────────────────────────────────────────────────────────── */

/* Systems languages weigh one point on CLASS for code-shaped tasks:
 * builds, memory discipline and toolchain friction are real work the
 * byte length of the request never shows. */
static bool route_lang_systems(const char *lang) {
  static const char *const systems[] = {"c", "cpp", "rust", "zig", NULL};
  size_t i;
  if (lang == NULL) return false;
  for (i = 0; systems[i] != NULL; i++)
    if (strcmp(lang, systems[i]) == 0) return true;
  return false;
}

/* Language named in the message; falls back to "" (caller then uses the
 * repository's dominant language). "C" and "Go" are matched
 * case-sensitively as standalone words — lowercase "c" and "go" are
 * ordinary English words. */
static const char *route_detect_lang(const char *msg) {
  static const char *const names[][2] = {
      {"python", "python"}, {"javascript", "js"}, {"typescript", "ts"},
      {"java", "java"},     {"ruby", "ruby"},     {"php", "php"},
      {"bash", "shell"},    {"golang", "go"},     {"rust", "rust"},
      {"c++", "cpp"},       {"cpp", "cpp"},       {"c#", "csharp"},
      {"csharp", "csharp"}, {"kotlin", "kotlin"}, {"swift", "swift"},
      {"zig", "zig"},       {NULL, NULL}};
  size_t i;
  const char *p;
  for (i = 0; names[i][0] != NULL; i++)
    if (route_word(msg, names[i][0])) return names[i][1];
  for (p = msg; *p != '\0'; p++) {
    if ((*p == 'C' || *p == 'G') && (p == msg || !route_is_alnum(p[-1]))) {
      if (*p == 'C' && !route_is_alnum(p[1]) && p[1] != '+' && p[1] != '#')
        return "c";
      if (*p == 'G' && p[1] == 'o' && !route_is_alnum(p[2])) return "go";
    }
  }
  return "";
}

/* ── evidence-scored classification ──────────────────────────────────── */

/*
 * CLASS is an additive score; the axes and their weights:
 *
 *   task kind        chat 0 · lookup 1 · explain 1 · edit 2 · build 2
 *                    generate 3 · refactor 3 · debug 3
 *   message shape    fence +2 · math +1 · len>900 +2 (else >300 +1)
 *                    ≥3 question marks +1
 *   tools evidenced  ≥3 families named by the message itself +1
 *                    (multi-tool orchestration; task-implied families
 *                    are already priced into the task base)
 *   language         systems language (c/cpp/rust/zig) on a code task +1
 *   repo size        (PLAN turns only) ≥200 files +1 · ≥2000 files +2
 *   history          any escalation in the window +1
 *                    ≥2 unreliable turns (judge/feedback) +1
 *   eval suite       (PLAN turns only) recorded success < 0.5 +1
 *
 *   score ≤1 SIMPLE · ≤3 MODERATE · else COMPLEX
 *
 * MODE is PLAN when tools are available and the message carries work
 * evidence: an imperative at a sentence start, a path/tool mention, a
 * fence, a workspace noun with a local anchor, or a code-shaped task
 * kind (edit/build/generate/refactor/debug). DIRECT otherwise; no tools
 * ⇒ always DIRECT.
 *
 * DETAIL is RICH on an explicit depth ask, a fence, len>600, a COMPLEX
 * verdict, or a generate task (code output is bulky); TERSE under 80
 * bytes otherwise; NORMAL in between.
 *
 * Pure and deterministic: no config, no clock, no allocation.
 */
void asngn_route_heuristic(const char *message,
                           const asngn_route_evidence *ev,
                           asngn_route_profile *out) {
  static const int task_base[] = {0, 1, 1, 2, 2, 3, 3, 3};
  static const asngn_route_evidence ev_zero = {0};
  size_t len, questions;
  bool fence, math, imperative, path, anchor_noun, tasky, code_task;
  unsigned msg_mask;
  const char *lang;
  int score;

  if (out == NULL) return;
  if (message == NULL) message = "";
  if (ev == NULL) ev = &ev_zero;

  len = strlen(message);
  fence = strstr(message, "```") != NULL;
  math = route_has_math(message, len);
  questions = route_count_questions(message);
  imperative = route_has_imperative(message);
  path = route_mentions_path(message);
  anchor_noun = route_has_noun(message) && route_has_local_anchor(message);

  out->task = route_detect_task(message, fence, questions);
  msg_mask = route_msg_tools(message);
  out->toolmask = msg_mask | route_task_tools(out->task);
  tasky = out->task == ASNGN_RTASK_EDIT || out->task == ASNGN_RTASK_BUILD ||
          out->task == ASNGN_RTASK_GENERATE ||
          out->task == ASNGN_RTASK_REFACTOR ||
          out->task == ASNGN_RTASK_DEBUG;
  code_task = tasky;

  if (ev->tools_available &&
      (imperative || path || route_explicit_tools(message) != 0 || fence ||
       anchor_noun || tasky))
    out->mode = ASNGN_MODE_PLAN;
  else
    out->mode = ASNGN_MODE_DIRECT;

  lang = route_detect_lang(message);
  if (lang[0] == '\0') lang = ev->repo_language;

  score = task_base[out->task];
  if (fence) score += 2;
  if (math) score += 1;
  if (len > 900) score += 2;
  else if (len > 300) score += 1;
  if (questions >= 3) score += 1;
  if (route_popcount(msg_mask) >= 3) score += 1;
  if (code_task && route_lang_systems(lang)) score += 1;
  if (out->mode == ASNGN_MODE_PLAN) {
    if (ev->repo_files >= 2000) score += 2;
    else if (ev->repo_files >= 200) score += 1;
    if (ev->has_eval && ev->eval_success < 0.5) score += 1;
  }
  if (ev->escalated > 0) score += 1;
  if (ev->unreliable >= 2) score += 1;

  if (score <= 1)      out->klass = ASNGN_CLASS_SIMPLE;
  else if (score <= 3) out->klass = ASNGN_CLASS_MODERATE;
  else                 out->klass = ASNGN_CLASS_COMPLEX;

  if (fence || len > 600 || out->klass == ASNGN_CLASS_COMPLEX ||
      out->task == ASNGN_RTASK_GENERATE ||
      route_ci_strstr(message, "explain") != NULL ||
      route_ci_strstr(message, "in detail") != NULL)
    out->detail = ASNGN_DETAIL_RICH;
  else if (len < 80)
    out->detail = ASNGN_DETAIL_TERSE;
  else
    out->detail = ASNGN_DETAIL_NORMAL;
}

/* ── evidence collection ─────────────────────────────────────────────── */

/* Ledger window examined for reliability signals. */
#define ROUTE_LEDGER_WINDOW 8

/* Load calibration/quality.xcdn once per context. Written by the quality
 * eval suite (tests/quality/run_quality.py); absence is the common case
 * and means "no evidence", never an error. Agent thread only. */
static void route_calib_probe(asngn_ctx *c) {
  char *dir, *path = NULL;
  struct xcdn_document *docp = NULL;
  xcdn_document_t *doc;
  size_t i;

  if (c->calib.probed) return;
  c->calib.probed = true;
  dir = os_path_join(c->root, "calibration");
  if (dir != NULL) {
    path = os_path_join(dir, "quality.xcdn");
    free(dir);
  }
  if (path == NULL) return;
  if (asngn_stream_load(c, path, "calibration", &docp) != ASNGN_OK) {
    free(path);
    return;
  }
  free(path);
  doc = (xcdn_document_t *)docp;
  if (doc == NULL) return;
  for (i = 0; i < doc->values_len; i++) {
    xcdn_node_t *node = doc->values[i];
    const xcdn_value_t *obj = node != NULL ? node->value : NULL;
    double d;
    int64_t n;
    if (!xcdn_node_has_tag(node, "quality")) continue;
    if (obj == NULL || obj->type != XCDN_VAL_OBJECT) continue;
    if (asngn_xnum(asngn_xfield(obj, "task_success_rate"), &d) &&
        d >= 0.0 && d <= 1.0) {
      c->calib.present = true;
      c->calib.success = d;
    }
    if (asngn_xint(asngn_xfield(obj, "tasks"), &n)) c->calib.tasks = n;
    if (asngn_xint(asngn_xfield(obj, "guard_trips"), &n))
      c->calib.guard_trips = n;
  }
  xcdn_document_free(doc);
  if (c->calib.present)
    asngn_log(c, ASNGN_LOG_INFO, "route",
              "eval calibration: success %.2f over %lld tasks",
              c->calib.success, (long long)c->calib.tasks);
}

void asngn_route_evidence_collect(asngn_ctx *c, asngn_session *s,
                                  const asngn_turn_state *t,
                                  asngn_route_evidence *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof *out);
  if (c == NULL) return;
  out->tools_available = c->astools_ok && (t == NULL || !t->opts.no_tools);
  if (c->repo_stats.loaded) {
    out->repo_files = c->repo_stats.files;
    out->repo_bytes = c->repo_stats.bytes;
    snprintf(out->repo_language, sizeof out->repo_language, "%s",
             c->repo_stats.language);
  }
  route_calib_probe(c);
  out->has_eval = c->calib.present;
  out->eval_success = c->calib.success;
  if (s != NULL) {
    size_t first =
        s->led_n > ROUTE_LEDGER_WINDOW ? s->led_n - ROUTE_LEDGER_WINDOW : 0;
    size_t i;
    for (i = first; i < s->led_n; i++) {
      const asngn_ledger_entry *e = &s->led[i];
      out->window++;
      if (e->escalations > 0) out->escalated++;
      if ((e->has_judge &&
           e->judge * 10.0 < (double)c->cfg.judge_threshold) ||
          (e->has_user_fb && e->user_fb < 0))
        out->unreliable++;
    }
  }
}

/* ── the nano micro-pass ─────────────────────────────────────────────── */

/* Truncation limit for the user prompt handed to the router model. */
#define ROUTE_MSG_MAX 16384

static const char ROUTE_SYS[] =
    "Classify one user request for an assistant. Return the constrained "
    "CLASS/DETAIL/MODE/TASK line only. TASK is GENERATE when new code or "
    "files must be created, EDIT for changing existing files, BUILD for "
    "running builds/tests/programs, DEBUG for diagnosing or fixing a "
    "failure, REFACTOR for structural code changes, EXPLAIN for conceptual "
    "analysis, LOOKUP for factual inspection, otherwise CHAT. Choose PLAN "
    "whenever fulfilling the request requires inspecting or changing the "
    "machine/workspace or invoking a tool; choose DIRECT only when tools "
    "are unnecessary. Classify only the action requested by CURRENT MESSAGE. "
    "Conversation context is supplied only to resolve references: a follow-up "
    "asking for a path, status, explanation, or prior result is LOOKUP or "
    "EXPLAIN, never a repetition of the earlier mutation. Classify by meaning "
    "in any language, not keywords.";

/* Largest prefix of at most ROUTE_MSG_MAX bytes that does not split a
 * UTF-8 sequence (continuation bytes are 10xxxxxx). */
static size_t route_utf8_cut(const char *s, size_t len, size_t limit) {
  size_t n = len < limit ? len : limit;
  if (n == len) return n;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

static size_t route_utf8_prefix(const char *s, size_t len) {
  return route_utf8_cut(s, len, (size_t)ROUTE_MSG_MAX);
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

  /* TASK was added after the original three-axis protocol. Keep accepting
   * old scripted providers, but real constrained backends now supply it so
   * normal routing is semantic rather than driven by keyword tables. */
  if (strstr(line, "TASK GENERATE") != NULL)
    out->task = ASNGN_RTASK_GENERATE;
  else if (strstr(line, "TASK REFACTOR") != NULL)
    out->task = ASNGN_RTASK_REFACTOR;
  else if (strstr(line, "TASK EXPLAIN") != NULL)
    out->task = ASNGN_RTASK_EXPLAIN;
  else if (strstr(line, "TASK LOOKUP") != NULL)
    out->task = ASNGN_RTASK_LOOKUP;
  else if (strstr(line, "TASK DEBUG") != NULL)
    out->task = ASNGN_RTASK_DEBUG;
  else if (strstr(line, "TASK BUILD") != NULL)
    out->task = ASNGN_RTASK_BUILD;
  else if (strstr(line, "TASK EDIT") != NULL)
    out->task = ASNGN_RTASK_EDIT;
  else if (strstr(line, "TASK CHAT") != NULL)
    out->task = ASNGN_RTASK_CHAT;

  return true;
}

/* One grammar-constrained router-model pass. The user prompt carries an
 * evidence header (task, tools, language, repo scale, history) ahead of
 * the message so the nano model votes on the same evidence the heuristic
 * scored. A configured model classifier is part of the quality contract:
 * failure is propagated instead of being hidden by an heuristic fallback. */
static asngn_err route_model_pass(asngn_ctx *c, asngn_session *s,
                                  const char *message,
                                  const asngn_route_evidence *ev,
                                  const asngn_route_profile *heur,
                                  asngn_turn_state *t,
                                  asngn_route_profile *out,
                                  size_t *aux_tokens) {
  int slot, tin = 0, tout = 0;
  char *gbnf = NULL, *text = NULL;
  char tools[32];
  asngn_buf up;
  size_t len, cut;
  asngn_err e;

  slot = asngn_models_slot_for_role(c, ASNGN_ROLE_ROUTER);
  if (slot < 0)
    return asngn_seterr(c, ASNGN_ERR_MODEL,
                        "configured model classifier is unavailable");
  e = asngn_grammar_classify(&gbnf);
  if (e != ASNGN_OK) return e;

  len = strlen(message);
  cut = route_utf8_prefix(message, len);
  route_tools_string(heur->toolmask, tools, sizeof tools);
  asngn_buf_init(&up);
  if (asngn_buf_printf(
          &up,
          "evidence: heuristic_task=%s hinted_tools=%s tools_available=%s "
          "lang=%s repo_files=%zu "
          "recent_escalations=%d\n",
          asngn_route_task_name(heur->task), tools,
          ev->tools_available ? "yes" : "no",
          ev->repo_language[0] != '\0' ? ev->repo_language : "unknown",
          ev->repo_files, ev->escalated) != ASNGN_OK) {
    asngn_buf_free(&up);
    free(gbnf);
    return ASNGN_ERR_NOMEM;
  }
  /* Give the semantic router the immediately preceding exchange.  The
   * in-flight user message is already the last transcript item, so exclude
   * it and cap each historical item: routing needs reference resolution,
   * not a second full conversation prompt. */
  if (s != NULL && s->log_n > 1) {
    size_t end = s->log_n - 1;
    size_t first = end > 2 ? end - 2 : 0;
    size_t i;
    if (asngn_buf_appends(&up, "conversation_context:\n") != ASNGN_OK) {
      asngn_buf_free(&up);
      free(gbnf);
      return ASNGN_ERR_NOMEM;
    }
    for (i = first; i < end; i++) {
      const asngn_turn *tr = &s->log[i];
      size_t hn = route_utf8_cut(tr->text, strlen(tr->text), 2048u);
      if (asngn_buf_printf(&up, "%s: ", tr->role) != ASNGN_OK ||
          asngn_buf_append(&up, tr->text, hn) != ASNGN_OK ||
          asngn_buf_appendc(&up, '\n') != ASNGN_OK) {
        asngn_buf_free(&up);
        free(gbnf);
        return ASNGN_ERR_NOMEM;
      }
    }
  }
  if (asngn_buf_appends(&up, "current_message:\n") != ASNGN_OK ||
      asngn_buf_append(&up, message, cut) != ASNGN_OK) {
    asngn_buf_free(&up);
    free(gbnf);
    return ASNGN_ERR_NOMEM;
  }

  {
    if (t->deadline_mono > 0 &&
        asngn_clock_mono_ms(&c->clock) >= t->deadline_mono) {
      free(gbnf);
      asngn_buf_free(&up);
      return ASNGN_ERR_TIMEOUT;
    }
    e = asngn_models_generate(c, slot, ASNGN_TASK_CLASSIFY, ROUTE_SYS,
                              up.data, gbnf, 0, t->deadline_mono, NULL, NULL,
                              &t->cancel, &text, &tin, &tout);
  }
  free(gbnf);
  asngn_buf_free(&up);
  if (e != ASNGN_OK) {
    free(text);
    return e;
  }
  if (aux_tokens != NULL && tout > 0) *aux_tokens += (size_t)tout;
  if (!route_parse_line(text, out)) {
    free(text);
    return asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                        "model classifier violated its output schema");
  }
  free(text);
  return ASNGN_OK;
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

/* Emit the classify event: data { class, detail, mode, task, tools,
 * source } plus the evidence weighed. A build failure degrades to an
 * empty data record — telemetry never fails the caller. */
static void route_emit(asngn_ctx *c, asngn_session *s,
                       const asngn_route_profile *p,
                       const asngn_route_evidence *ev, const char *source) {
  xcdn_value_t *obj = xcdn_value_object();
  xcdn_node_t *node = NULL;
  asngn_buf buf;
  char tools[32];
  bool ok = obj != NULL;

  asngn_buf_init(&buf);
  route_tools_string(p->toolmask, tools, sizeof tools);
  ok = ok && asngn_xobj_put(obj, "class",
                            xcdn_value_string(route_class_name(p->klass)));
  ok = ok && asngn_xobj_put(obj, "detail",
                            xcdn_value_string(asngn_detail_name(p->detail)));
  ok = ok && asngn_xobj_put(obj, "mode",
                            xcdn_value_string(route_mode_name(p->mode)));
  ok = ok && asngn_xobj_put(obj, "task",
                            xcdn_value_string(asngn_route_task_name(p->task)));
  ok = ok && asngn_xobj_put(obj, "tools", xcdn_value_string(tools));
  ok = ok && asngn_xobj_put(obj, "repo_files",
                            xcdn_value_int((int64_t)ev->repo_files));
  ok = ok && asngn_xobj_put(obj, "escalated",
                            xcdn_value_int((int64_t)ev->escalated));
  ok = ok && asngn_xobj_put(obj, "unreliable",
                            xcdn_value_int((int64_t)ev->unreliable));
  ok = ok && asngn_xobj_put(obj, "eval",
                            ev->has_eval
                                ? xcdn_value_float(ev->eval_success)
                                : xcdn_value_null());
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
  asngn_route_evidence ev;
  const char *source = "heuristic";
  asngn_err e;

  if (c == NULL || s == NULL || t == NULL || out == NULL)
    return ASNGN_ERR_INVALID;
  if (message == NULL) message = "";

  asngn_route_evidence_collect(c, s, t, &ev);
  t->evidence = ev;

  asngn_route_heuristic(message, &ev, &heur);
  *out = heur;

  switch (c->cfg.classifier) {
    case ASNGN_CLASSIFIER_HEURISTIC:
      break;
    case ASNGN_CLASSIFIER_MODEL:
      /* A semantic model may add capability, particularly for non-English
       * requests, but it may not erase deterministic evidence.  Treat the
       * heuristic as a quality/capability floor instead of allowing a noisy
       * router pass to turn a complex rich coding task into terse output. */
      model = heur;
      e = route_model_pass(c, s, message, &ev, &heur, t, &model,
                           aux_tokens);
      if (e != ASNGN_OK) return e;
      {
        out->klass = model.klass > heur.klass ? model.klass : heur.klass;
        out->detail =
            model.detail > heur.detail ? model.detail : heur.detail;
        out->task = route_merge_task(heur.task, model.task);
        out->toolmask = heur.toolmask | route_task_tools(heur.task) |
                        route_task_tools(model.task);
        out->mode = !ev.tools_available
                        ? ASNGN_MODE_DIRECT
                        : ((model.mode == ASNGN_MODE_PLAN ||
                            heur.mode == ASNGN_MODE_PLAN ||
                            route_task_requires_tools(out->task))
                               ? ASNGN_MODE_PLAN
                               : ASNGN_MODE_DIRECT);
        source = "model";
      }
      break;
    case ASNGN_CLASSIFIER_HYBRID:
    default:
      /* Conservative merge: higher CLASS wins; MODE PLAN wins
       * over DIRECT (still gated on tool availability); DETAIL is the
       * model's vote unless the heuristic says RICH — i.e. the max of
       * the two in the order TERSE < NORMAL < RICH (the enum order). */
      model = heur;
      e = route_model_pass(c, s, message, &ev, &heur, t, &model,
                           aux_tokens);
      if (e != ASNGN_OK) return e;
      {
        out->klass = model.klass > heur.klass ? model.klass : heur.klass;
        out->task = route_merge_task(heur.task, model.task);
        out->toolmask = heur.toolmask | route_task_tools(heur.task) |
                        route_task_tools(model.task);
        out->mode = !ev.tools_available
                        ? ASNGN_MODE_DIRECT
                        : ((model.mode == ASNGN_MODE_PLAN ||
                            heur.mode == ASNGN_MODE_PLAN ||
                            route_task_requires_tools(out->task))
                               ? ASNGN_MODE_PLAN
                               : ASNGN_MODE_DIRECT);
        out->detail =
            model.detail > heur.detail ? model.detail : heur.detail;
        source = "hybrid";
      }
      break;
  }

  if (route_is_reference_followup(s, message, &heur)) {
    out->task = ASNGN_RTASK_LOOKUP;
    out->mode = ASNGN_MODE_DIRECT;
    out->klass = heur.klass;
    out->detail = heur.detail;
    out->toolmask = heur.toolmask;
    source = "followup";
  }

  route_emit(c, s, out, &ev, source);
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
