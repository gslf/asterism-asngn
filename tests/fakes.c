/* fakes.c — deterministic test doubles for libasngn. See fakes.h. */

#include "asngn_internal.h"
#include "fakes.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake embedder ------------------------------------------------------ */

static int fake_is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == '\v' || ch == '\f';
}

void fake_embed_text(const char *text, float out[FAKE_EMBED_DIM]) {
  const char *p = text ? text : "";
  double norm = 0.0;
  size_t i;
  for (i = 0; i < FAKE_EMBED_DIM; i++) out[i] = 0.0f;
  while (*p) {
    char word[128];
    size_t w = 0;
    while (*p && fake_is_space(*p)) p++;
    while (*p && !fake_is_space(*p)) {
      char ch = *p++;
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      if (w + 1 < sizeof word) word[w++] = ch;
    }
    if (w > 0) out[asngn_fnv1a64(word, w) % FAKE_EMBED_DIM] += 1.0f;
  }
  for (i = 0; i < FAKE_EMBED_DIM; i++)
    norm += (double)out[i] * (double)out[i];
  if (norm > 0.0) {
    norm = sqrt(norm);
    for (i = 0; i < FAKE_EMBED_DIM; i++)
      out[i] = (float)((double)out[i] / norm);
  }
}

/* ---- fake model --------------------------------------------------------- */

/* Byte/4 heuristic, min 1 (matches asngn_token_heuristic). */
static int fake_count(const char *text) {
  size_t len = text ? strlen(text) : 0;
  size_t n = len / 4;
  return n < 1 ? 1 : (int)n;
}

static void fake_noop_destroy(void *ud) { (void)ud; }

void fake_model_init(fake_model *fm) { memset(fm, 0, sizeof *fm); }

void fake_model_dispose(fake_model *fm) {
  size_t i;
  if (!fm) return;
  for (i = 0; i < fm->n; i++) free(fm->replies[i]);
  free(fm->replies);
  free(fm->last_system);
  free(fm->last_user);
  free(fm->last_grammar);
  memset(fm, 0, sizeof *fm);
}

int fake_model_push(fake_model *fm, const char *reply) {
  char *copy = asngn_strdup(reply);
  if (!copy) return 0;
  if (fm->n == fm->cap) {
    size_t ncap = fm->cap ? fm->cap * 2 : 8;
    char **nr = (char **)realloc(fm->replies, ncap * sizeof *nr);
    if (!nr) {
      free(copy);
      return 0;
    }
    fm->replies = nr;
    fm->cap = ncap;
  }
  fm->replies[fm->n++] = copy;
  return 1;
}

static asngn_err fake_model_generate(void *ud, const char *system_prompt,
                                     const char *user_prompt,
                                     const char *gbnf,
                                     const asngn_gen_params *p,
                                     asngn_token_fn token_cb, void *token_ud,
                                     volatile int *cancel,
                                     char **out_text, int *out_tokens_in,
                                     int *out_tokens_out) {
  fake_model *fm = (fake_model *)ud;
  const char *reply = "ANSWER\n";
  (void)p;
  if (!fm || !out_text) return ASNGN_ERR_INVALID;
  if (cancel && *cancel) return ASNGN_ERR_CANCELLED;
  free(fm->last_system);
  fm->last_system = asngn_strdup(system_prompt);
  free(fm->last_user);
  fm->last_user = asngn_strdup(user_prompt);
  free(fm->last_grammar);
  fm->last_grammar = asngn_strdup(gbnf);
  fm->calls++;
  if (fm->next < fm->n) reply = fm->replies[fm->next++];
  *out_text = asngn_strdup(reply);
  if (!*out_text) return ASNGN_ERR_NOMEM;
  if (token_cb) token_cb(reply, token_ud);
  if (out_tokens_in)
    *out_tokens_in = fake_count(system_prompt) + fake_count(user_prompt);
  if (out_tokens_out) *out_tokens_out = fake_count(reply);
  return ASNGN_OK;
}

static int fake_model_count_tokens(void *ud, const char *text) {
  (void)ud;
  return fake_count(text);
}

static int fake_model_count_prompt_tokens(void *ud, const char *system_text,
                                          const char *user_text) {
  (void)ud;
  /* Eight tokens model the fixed two-message template and assistant
   * generation marker used by the real backend. */
  return fake_count(system_text) + fake_count(user_text) + 8;
}

static asngn_err fake_model_embed(void *ud, const char *text, float *out) {
  (void)ud;
  if (!text || !out) return ASNGN_ERR_INVALID;
  fake_embed_text(text, out);
  return ASNGN_OK;
}

asngn_model_iface fake_model_iface(fake_model *fm) {
  asngn_model_iface it;
  memset(&it, 0, sizeof it);
  it.ud = fm;
  it.generate = fake_model_generate;
  it.count_tokens = fake_model_count_tokens;
  it.count_prompt_tokens = fake_model_count_prompt_tokens;
  it.embed = fake_model_embed;
  it.destroy = fake_noop_destroy;
  return it;
}

/* ---- fake clock --------------------------------------------------------- */

static asngn_time fake_clock_now_cb(void *ud) {
  return (asngn_time)((const fake_clock *)ud)->now;
}

static int64_t fake_clock_mono_cb(void *ud) {
  return ((const fake_clock *)ud)->mono_ms;
}

void fake_clock_set(fake_clock *fc, long long unix_now) {
  fc->now = unix_now;
  fc->mono_ms = (int64_t)unix_now * 1000;
}

void fake_clock_advance(fake_clock *fc, long long secs) {
  fc->now += secs;
  fc->mono_ms += (int64_t)secs * 1000;
}

asngn_clock fake_clock_make(fake_clock *fc) {
  asngn_clock c;
  c.ud = fc;
  c.now = fake_clock_now_cb;
  c.mono_ms = fake_clock_mono_cb;
  return c;
}
