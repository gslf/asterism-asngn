/*
 * detail.c — the detail controller.
 *
 * The answer budget is an explicit three-valued level (TERSE, NORMAL,
 * RICH); AUTO is only ever an input meaning "let someone else decide".
 * Selection order: explicit user override (/detail or in-message cue) ▷
 * budget-pressure bias ▷ classifier vote ▷ detail.default. The cap
 * is a hard stop enforced by the answer pass; this module only chooses the
 * level, its token cap, and the style directive injected into the system
 * zone.
 *
 * MIT License — per aspera ad astra.
 */

#include <string.h>

#include "asngn_internal.h"

/* ── caps ────────────────────────────────────────────────────────────── */

/* Token cap for a level, from configuration. AUTO should never reach the
 * answer pass; it maps to normal_tokens defensively. */
int asngn_detail_cap(asngn_ctx *c, asngn_detail d) {
  switch (d) {
    case ASNGN_DETAIL_TERSE:  return c->cfg.terse_tokens;
    case ASNGN_DETAIL_RICH:   return c->cfg.rich_tokens;
    case ASNGN_DETAIL_NORMAL: return c->cfg.normal_tokens;
    case ASNGN_DETAIL_AUTO:
    default:                  return c->cfg.normal_tokens;
  }
}

/* ── style directives (verbatim) ──────────────────────────────────────── */

const char *asngn_detail_directive(asngn_detail d) {
  switch (d) {
    case ASNGN_DETAIL_TERSE:
      return "Answer directly. No preamble, no recap, no closing summary.";
    case ASNGN_DETAIL_RICH:
      return "Answer thoroughly, with structure and examples where useful.";
    case ASNGN_DETAIL_NORMAL:
    case ASNGN_DETAIL_AUTO: /* defensive: AUTO reads as NORMAL */
    default:
      return "Answer completely but economically; expand only what the "
             "question needs.";
  }
}

const char *asngn_detail_name(asngn_detail d) {
  switch (d) {
    case ASNGN_DETAIL_TERSE:  return "terse";
    case ASNGN_DETAIL_NORMAL: return "normal";
    case ASNGN_DETAIL_RICH:   return "rich";
    case ASNGN_DETAIL_AUTO:
    default:                  return "auto";
  }
}

/* ── in-message cues ─────────────────────────────────────────────────── */

/* Case-insensitive (ASCII) substring search; cues are matched as plain
 * substrings, not whole words — "word-ish" matching is deliberate, so
 * multi-word cues like "step by step" match anywhere in the sentence. */
static const char *detail_ci_strstr(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  for (; *hay != '\0'; hay++) {
    size_t i;
    for (i = 0; i < nl; i++) {
      char a = hay[i], b = needle[i];
      if (a == '\0') return NULL;
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
    }
    if (i == nl) return hay;
  }
  return NULL;
}

/* Cue table (first matching table wins; TERSE cues are checked first):
 *
 *   TERSE  "briefly"  "one line"  "tl;dr"
 *   RICH   "in detail"  "thoroughly"  "step by step"
 *
 * Returns ASNGN_DETAIL_AUTO when no cue is present (no override). */
asngn_detail asngn_detail_cue(const char *message) {
  static const char *const terse_cues[] = {
    "briefly", "one line", "tl;dr", NULL
  };
  static const char *const rich_cues[] = {
    "in detail", "thoroughly", "step by step", NULL
  };
  size_t i;

  if (message == NULL) return ASNGN_DETAIL_AUTO;
  for (i = 0; terse_cues[i] != NULL; i++)
    if (detail_ci_strstr(message, terse_cues[i]) != NULL)
      return ASNGN_DETAIL_TERSE;
  for (i = 0; rich_cues[i] != NULL; i++)
    if (detail_ci_strstr(message, rich_cues[i]) != NULL)
      return ASNGN_DETAIL_RICH;
  return ASNGN_DETAIL_AUTO;
}

/* ── effective level (selection order + pressure levers) ──────────────── */

/*
 * 1. Start: user override when explicit (!= AUTO); otherwise
 *    detail.default when explicit; otherwise the classifier vote
 *    (which is never AUTO — a defensive AUTO reads as NORMAL).
 * 2. Spend pressure may remove excess verbosity from SIMPLE turns only.
 *    It is accounting guidance, not permission to under-resource hard work:
 *    MODERATE/COMPLEX tasks retain the detail justified by their evidence.
 * 3. The result is a concrete level, never AUTO.
 */
asngn_detail asngn_detail_effective(asngn_ctx *c, asngn_detail user_override,
                                    asngn_detail classifier_vote,
                                    asngn_class klass, double pressure) {
  asngn_detail d;

  if (user_override != ASNGN_DETAIL_AUTO) return user_override;

  d = (c->cfg.detail_default != ASNGN_DETAIL_AUTO) ? c->cfg.detail_default
                                                   : classifier_vote;
  if (d == ASNGN_DETAIL_AUTO) d = ASNGN_DETAIL_NORMAL; /* defensive */

  if (klass == ASNGN_CLASS_SIMPLE && pressure >= c->cfg.warn_at) {
    if (d == ASNGN_DETAIL_RICH)
      d = ASNGN_DETAIL_NORMAL;
    else if (d == ASNGN_DETAIL_NORMAL && pressure >= 1.0)
      d = ASNGN_DETAIL_TERSE;
  }
  return d;
}
