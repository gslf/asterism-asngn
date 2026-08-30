/*
 * judge.c — answer validation. One grammar-constrained
 * pass on the judge role: the model sees the user request, the gathered
 * evidence, and the candidate answer, and emits exactly one line
 * "SCORE <0-10> | <justification>". Judge tokens are accounted as aux.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* verbatim. */
static const char judge_system[] =
    "You review one assistant answer. You will see the user request, the "
    "evidence the assistant gathered, and the answer.\n"
    "\n"
    "Rate the answer for correctness, completeness, and staying on topic.\n"
    "Be strict about factual errors and ignored parts of the request; do "
    "not punish brevity by itself.\n"
    "\n"
    "Answer with exactly one line:\n"
    "SCORE <0-10> | <justification of at most ten words>";

#define JUDGE_EVIDENCE_MAX 32768
#define JUDGE_ANSWER_MAX 49152

/* Append at most `cap` bytes of src, backing off to a UTF-8 boundary and
 * appending a "…" marker when truncated. */
static asngn_err append_capped(asngn_buf *b, const char *src, size_t cap) {
  size_t    n = strlen(src);
  asngn_err e;
  if (n <= cap) return asngn_buf_append(b, src, n);
  while (cap > 0 && ((unsigned char)src[cap] & 0xC0) == 0x80) cap--;
  e = asngn_buf_append(b, src, cap);
  if (e != ASNGN_OK) return e;
  return asngn_buf_appends(b, "\xE2\x80\xA6"); /* … */
}

/* Parse "SCORE <n> | <critique>"; defense in depth over the grammar. */
static bool parse_score_line(const char *txt, int *out_n,
                             const char **out_crit, size_t *out_crit_len) {
  const char *p, *crit, *end;
  int n;
  if (!txt || strncmp(txt, "SCORE ", 6) != 0) return false;
  p = txt + 6;
  if (p[0] == '1' && p[1] == '0') {
    n = 10;
    p += 2;
  } else if (p[0] >= '0' && p[0] <= '9') {
    n = p[0] - '0';
    p += 1;
  } else {
    return false;
  }
  if (p[0] >= '0' && p[0] <= '9') return false; /* 11..99, 100... */
  while (*p == ' ' || *p == '\t') p++;
  if (*p != '|') return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  crit = p;
  end = strchr(p, '\n');
  if (!end) end = p + strlen(p);
  while (end > crit &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
    end--;
  *out_n = n;
  *out_crit = crit;
  *out_crit_len = (size_t)(end - crit);
  return true;
}

asngn_err asngn_judge_run(asngn_ctx *c, asngn_session *s,
                          struct asngn_turn_state *t, const char *request,
                          const char *evidence, const char *answer,
                          int *out_score, char **out_critique,
                          size_t *aux_tokens) {
  asngn_buf   ub;
  char       *gbnf = NULL, *txt = NULL;
  char        data[64];
  const char *crit = NULL;
  size_t      crit_len = 0;
  int         slot, tin = 0, tout = 0, score = 0;
  asngn_err   e;

  if (!c) return ASNGN_ERR_INVALID;
  if (!s || !t || !request || !answer || !out_score)
    return asngn_seterr(c, ASNGN_ERR_INVALID, "judge: missing argument");
  if (out_critique) *out_critique = NULL;

  slot = asngn_models_slot_for_role(c, ASNGN_ROLE_JUDGE);
  if (slot < 0)
    return asngn_seterr(c, ASNGN_ERR_MODEL,
                        "judge role has no available model");

  asngn_buf_init(&ub);
  e = asngn_buf_appends(&ub, "Request:\n");
  if (e == ASNGN_OK) e = asngn_buf_appends(&ub, request);
  if (e == ASNGN_OK) e = asngn_buf_appends(&ub, "\n\nEvidence:\n");
  if (e == ASNGN_OK) {
    if (evidence && evidence[0] != '\0')
      e = append_capped(&ub, evidence, JUDGE_EVIDENCE_MAX);
    else
      e = asngn_buf_appends(&ub, "(none)");
  }
  if (e == ASNGN_OK) e = asngn_buf_appends(&ub, "\n\nAnswer:\n");
  if (e == ASNGN_OK) e = append_capped(&ub, answer, JUDGE_ANSWER_MAX);
  if (e == ASNGN_OK) e = asngn_grammar_judge(&gbnf);
  if (e != ASNGN_OK) {
    asngn_buf_free(&ub);
    free(gbnf);
    return e;
  }

  if (t->deadline_mono > 0 &&
      asngn_clock_mono_ms(&c->clock) >= t->deadline_mono) {
    free(gbnf);
    asngn_buf_free(&ub);
    return ASNGN_ERR_TIMEOUT;
  }
  e = asngn_models_generate(c, slot, ASNGN_TASK_JUDGE, judge_system, ub.data,
                            gbnf, 0, t->deadline_mono, NULL, NULL, &t->cancel,
                            &txt, &tin,
                            &tout);
  free(gbnf);
  asngn_buf_free(&ub);
  if (e != ASNGN_OK) return e;
  if (aux_tokens && tout > 0) *aux_tokens += (size_t)tout;

  if (!parse_score_line(txt, &score, &crit, &crit_len)) {
    e = asngn_seterr(c, ASNGN_ERR_PROTOCOL,
                     "judge emitted an unparseable line: \"%.80s\"",
                     txt ? txt : "(null)");
    free(txt);
    return e;
  }

  *out_score = score;
  if (out_critique && crit_len > 0) {
    *out_critique = asngn_strndup(crit, crit_len);
    if (!*out_critique) {
      free(txt);
      return ASNGN_ERR_NOMEM;
    }
  }
  free(txt);

  snprintf(data, sizeof data, "{score: %d, tokens: %d}", score, tout);
  asngn_tele_emit(c, "judge", NULL, NULL, s->slug, 0, data);
  return ASNGN_OK;
}
