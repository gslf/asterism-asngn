/*
 * fakes.h — deterministic test doubles for libasngn.
 *
 * Fake embedder: bag-of-words, dim 16. The text is split on ASCII
 * whitespace; each word is lowercased (ASCII A-Z only) and adds 1.0 to
 * vec[asngn_fnv1a64(word) % 16]; the vector is L2-normalized (an empty
 * text stays all-zero). Shared words => high cosine, disjoint words => 0
 * (modulo bucket collisions).
 *
 * Fake model: a queue of canned reply strings behind the asngn_model_iface
 * backend vtable. generate() captures the prompts/grammar it was
 * given, pops the next reply (or the answer action object when the
 * queue is empty),
 * streams the whole reply through token_cb in one call, and reports token
 * counts with the byte/4 heuristic (min 1, matching asngn_token_heuristic).
 * *cancel set before work => ASNGN_ERR_CANCELLED. embed() is the fake
 * embedder above (dim FAKE_EMBED_DIM).
 *
 * Fake clock: settable unix seconds with a derived monotonic-ms track for
 * deterministic time travel.
 *
 * Ownership: the vtables handed to asngn_open_with carry no-op destroy
 * hooks — the test owns the fake structs and may inspect them after
 * asngn_close.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_FAKES_H
#define ASNGN_FAKES_H

#include "asngn_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_EMBED_DIM 16

/* The fake embedding computed directly, for expected-value math. */
void fake_embed_text(const char *text, float out[FAKE_EMBED_DIM]);

typedef struct {
  char **replies;
  size_t n, cap, next;
  char *last_system;  /* copy of the most recent system prompt  */
  char *last_user;    /* copy of the most recent user prompt    */
  char *last_grammar; /* copy of the most recent GBNF (or NULL) */
  int calls;          /* generate() invocations                 */
} fake_model;

void fake_model_init(fake_model *fm);
void fake_model_dispose(fake_model *fm);
/* Queue one scripted reply (copied). 1 = ok, 0 = out of memory. */
int fake_model_push(fake_model *fm, const char *reply);
asngn_model_iface fake_model_iface(fake_model *fm);

typedef struct {
  long long now;     /* wall clock, unix seconds UTC */
  int64_t   mono_ms; /* monotonic milliseconds       */
} fake_clock;

void fake_clock_set(fake_clock *fc, long long unix_now); /* mono = now*1000 */
void fake_clock_advance(fake_clock *fc, long long secs);
asngn_clock fake_clock_make(fake_clock *fc);

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_FAKES_H */
