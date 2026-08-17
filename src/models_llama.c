/*
 * models_llama.c — llama.cpp model backend.
 *
 * One backend instance per pool entry, serving generate() or embed()
 * depending on cfg->embedding (plus count_tokens() either way; the
 * other function pointer stays NULL). Generation applies the model's
 * embedded chat template (chatml fallback), attaches a per-call GBNF
 * grammar sampler when given, and streams token pieces through
 * token_cb. Embedding output is mean-pooled by llama.cpp
 * (LLAMA_POOLING_TYPE_MEAN) and L2-normalized here in double.
 *
 * Cancellation: *cancel is polled between prompt-eval chunks and at the
 * top of every decode iteration; in addition the per-call cancel
 * pointer is installed as ggml's abort callback on the llama context,
 * so long individual llama_decode calls abort early too (CPU backend
 * only, per llama.h).
 *
 * Thread model: models.c serializes calls per instance with the slot
 * mutex; nothing here assumes more than one call in flight per
 * instance. count_tokens touches only the vocab and is thread-safe.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <string.h>

#ifdef ASNGN_WITH_LLAMA

#include <limits.h>
#include <math.h>
#include <stdlib.h>

/* llama.h/ggml.h are not pedantic-C99-clean (anonymous unions, typedef
 * redefinitions); include them with those diagnostics off. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if !defined(ASNGN_NO_THREADS)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

/* ---- process-wide llama bootstrap --------------------------------------- */

/* llama.cpp logs to stderr by default. The library must never write to
 * stdout/stderr on its own initiative, and this global callback has no
 * asngn_ctx to forward into asngn_log, so it deliberately drops every
 * record. asngn reports load/inference failures through its own error
 * and log channels. */
static void mll_log_silent(enum ggml_log_level level, const char *text,
                           void *ud) {
  (void)level;
  (void)text;
  (void)ud;
}

static void mll_boot(void) {
  llama_log_set(mll_log_silent, NULL);
  llama_backend_init();
}

#if defined(ASNGN_NO_THREADS)

static void mll_backend_init(void) {
  static bool done = false;
  if (!done) {
    done = true;
    mll_boot();
  }
}

#elif defined(_WIN32)

static BOOL CALLBACK mll_boot_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  mll_boot();
  return TRUE;
}

static void mll_backend_init(void) {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(&once, mll_boot_cb, NULL, NULL);
}

#else /* POSIX threads */

static pthread_once_t mll_once = PTHREAD_ONCE_INIT;

static void mll_backend_init(void) {
  pthread_once(&mll_once, mll_boot);
}

#endif

/* ---- backend state ------------------------------------------------------ */

#define MLL_N_BATCH 512   /* prompt evaluation chunk size (generative)   */
#define MLL_EMBED_CTX 512 /* default embedding window when cfg->ctx <= 0 */

typedef struct {
  struct llama_model *model;
  const struct llama_vocab *vocab;
  struct llama_context *lctx; /* one call at a time (slot mutex)         */
  const char *chat_template;  /* model-owned; NULL = chatml fallback     */
  int n_ctx;
  int n_batch;
  int dim;                    /* embedding models only                   */
  bool embedding;
  bool encoder_only;
  /* Per-call cancel pointer for the ggml abort callback: installed at
   * the top of generate() and cleared on exit, guarded by cancel_mu
   * because the ggml worker threads read it mid-decode. */
  os_mutex cancel_mu;
  volatile int *cancel_flag;
} mll_ud;

static bool mll_abort_cb(void *data) {
  mll_ud *u = (mll_ud *)data;
  volatile int *flag;

  os_mutex_lock(&u->cancel_mu);
  flag = u->cancel_flag;
  os_mutex_unlock(&u->cancel_mu);
  return flag != NULL && *flag != 0;
}

static void mll_set_cancel(mll_ud *u, volatile int *cancel) {
  os_mutex_lock(&u->cancel_mu);
  u->cancel_flag = cancel;
  os_mutex_unlock(&u->cancel_mu);
}

static bool mll_cancelled(volatile int *cancel) {
  return cancel != NULL && *cancel != 0;
}

/* Tokenize text into a malloc'd array using the negative-return resize
 * convention (first call sizes, second call fills). *out_tok is NULL
 * when the text yields zero tokens. */
static asngn_err mll_tokenize(const struct llama_vocab *vocab,
                              const char *text, bool add_special,
                              bool parse_special, llama_token **out_tok,
                              int32_t *out_n) {
  int32_t len, n, got;
  llama_token *tok;

  *out_tok = NULL;
  *out_n = 0;
  len = (int32_t)strlen(text);
  n = llama_tokenize(vocab, text, len, NULL, 0, add_special, parse_special);
  if (n == INT32_MIN) return ASNGN_ERR_MODEL;
  if (n < 0) n = -n;
  if (n == 0) return ASNGN_OK;
  tok = (llama_token *)malloc((size_t)n * sizeof *tok);
  if (tok == NULL) return ASNGN_ERR_NOMEM;
  got = llama_tokenize(vocab, text, len, tok, n, add_special, parse_special);
  if (got < 0) {
    free(tok);
    return ASNGN_ERR_MODEL;
  }
  *out_tok = tok;
  *out_n = got;
  return ASNGN_OK;
}

/* Detokenize one token, append it to the answer buffer and stream it
 * through token_cb (NUL-terminated piece bytes). Pieces may split
 * multi-byte UTF-8 sequences across callback invocations — acceptable
 * for v1; consumers reassemble. Special tokens render as nothing
 * (special=false): the grammar never produces them and EOG stops
 * generation before this is reached. */
static asngn_err mll_emit_piece(const struct llama_vocab *vocab,
                                llama_token tok, asngn_buf *b,
                                asngn_token_fn token_cb, void *token_ud) {
  char small[160];
  char *big;
  int32_t n, got;
  asngn_err e;

  n = llama_token_to_piece(vocab, tok, small, (int32_t)sizeof small - 1, 0,
                           false);
  if (n >= 0) {
    small[n] = '\0';
    e = asngn_buf_append(b, small, (size_t)n);
    if (e == ASNGN_OK && token_cb != NULL && n > 0) token_cb(small, token_ud);
    return e;
  }
  if (n == INT32_MIN) return ASNGN_ERR_MODEL;
  big = (char *)malloc((size_t)-n + 1);
  if (big == NULL) return ASNGN_ERR_NOMEM;
  got = llama_token_to_piece(vocab, tok, big, -n, 0, false);
  if (got < 0) {
    free(big);
    return ASNGN_ERR_MODEL;
  }
  big[got] = '\0';
  e = asngn_buf_append(b, big, (size_t)got);
  if (e == ASNGN_OK && token_cb != NULL && got > 0) token_cb(big, token_ud);
  free(big);
  return e;
}

/* Render system + user through the chat template with the assistant
 * prompt appended, using the negative-return / resize-and-retry
 * convention. Unsupported model templates fall back to chatml (tmpl
 * NULL). *out is malloc'd. */
static asngn_err mll_apply_template(const char *tmpl,
                                    const char *system_prompt,
                                    const char *user_prompt, char **out) {
  struct llama_chat_message msgs[2];
  int32_t need, got;
  char *buf;

  msgs[0].role = "system";
  msgs[0].content = system_prompt;
  msgs[1].role = "user";
  msgs[1].content = user_prompt;

  need = llama_chat_apply_template(tmpl, msgs, 2, true, NULL, 0);
  if (need < 0 && tmpl != NULL) {
    tmpl = NULL; /* chatml fallback */
    need = llama_chat_apply_template(tmpl, msgs, 2, true, NULL, 0);
  }
  if (need < 0) return ASNGN_ERR_MODEL;
  buf = (char *)malloc((size_t)need + 1);
  if (buf == NULL) return ASNGN_ERR_NOMEM;
  got = llama_chat_apply_template(tmpl, msgs, 2, true, buf,
                                  (int32_t)(need + 1));
  if (got < 0 || got > need) {
    free(buf);
    return ASNGN_ERR_MODEL;
  }
  buf[got] = '\0';
  *out = buf;
  return ASNGN_OK;
}

/* ---- generation --------------------------------------------------------- */

static asngn_err mll_generate(void *ud, const char *system_prompt,
                              const char *user_prompt, const char *gbnf,
                              const asngn_gen_params *p,
                              asngn_token_fn token_cb, void *token_ud,
                              volatile int *cancel, char **out_text,
                              int *out_tokens_in, int *out_tokens_out) {
  mll_ud *u = (mll_ud *)ud;
  char *prompt = NULL;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  struct llama_sampler *chain = NULL;
  asngn_buf outbuf;
  asngn_err e;
  int32_t i, n_past;
  int produced, limit;

  *out_text = NULL;
  *out_tokens_in = 0;
  *out_tokens_out = 0;
  asngn_buf_init(&outbuf);
  mll_set_cancel(u, cancel);

  if (mll_cancelled(cancel)) {
    e = ASNGN_ERR_CANCELLED;
    goto out;
  }

  e = mll_apply_template(u->chat_template,
                         system_prompt != NULL ? system_prompt : "",
                         user_prompt != NULL ? user_prompt : "", &prompt);
  if (e != ASNGN_OK) goto out;

  /* add_special true so BOS-requiring vocabs (Llama-3-class instruct
   * models) get BOS — chat templates do not emit it themselves and
   * add_bos=false vocabs (e.g. Qwen) are unaffected; parse_special true
   * so role markers become their special tokens. Matches the upstream
   * convention of add_special on the first prompt segment. */
  e = mll_tokenize(u->vocab, prompt, true, true, &tok, &n_tok);
  if (e != ASNGN_OK) goto out;
  if (n_tok == 0 || n_tok >= u->n_ctx) {
    e = ASNGN_ERR_MODEL; /* empty or context-overflowing prompt */
    goto out;
  }
  *out_tokens_in = (int)n_tok;

  llama_memory_clear(llama_get_memory(u->lctx), true);

  /* Fresh sampler chain per call: grammar state is per-generation, and
   * the chain shape follows the sampling params — grammar first (when
   * constrained), then greedy for temp <= 0, else top_p / temp / dist. */
  chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
  if (chain == NULL) {
    e = ASNGN_ERR_NOMEM;
    goto out;
  }
  if (gbnf != NULL) {
    struct llama_sampler *grammar =
        llama_sampler_init_grammar(u->vocab, gbnf, "root");
    if (grammar == NULL) {
      e = ASNGN_ERR_MODEL; /* GBNF failed to parse */
      goto out;
    }
    llama_sampler_chain_add(chain, grammar);
  }
  if (p->temp <= 0.0) {
    struct llama_sampler *greedy = llama_sampler_init_greedy();
    if (greedy == NULL) {
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, greedy);
  } else {
    struct llama_sampler *sm;
    if (p->top_p > 0.0) {
      sm = llama_sampler_init_top_p((float)p->top_p, 1);
      if (sm == NULL) {
        e = ASNGN_ERR_NOMEM;
        goto out;
      }
      llama_sampler_chain_add(chain, sm);
    }
    sm = llama_sampler_init_temp((float)p->temp);
    if (sm == NULL) {
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, sm);
    sm = llama_sampler_init_dist(LLAMA_DEFAULT_SEED);
    if (sm == NULL) {
      e = ASNGN_ERR_NOMEM;
      goto out;
    }
    llama_sampler_chain_add(chain, sm);
  }

  for (i = 0; i < n_tok; i += u->n_batch) {
    int32_t chunk = n_tok - i < u->n_batch ? n_tok - i : u->n_batch;
    struct llama_batch batch;
    if (mll_cancelled(cancel)) {
      e = ASNGN_ERR_CANCELLED;
      goto out;
    }
    batch = llama_batch_get_one(tok + i, chunk);
    if (llama_decode(u->lctx, batch) != 0) {
      e = mll_cancelled(cancel) ? ASNGN_ERR_CANCELLED : ASNGN_ERR_MODEL;
      goto out;
    }
  }

  limit = p->max_tokens > 0 ? p->max_tokens : INT_MAX;
  n_past = n_tok;
  produced = 0;
  while (produced < limit) {
    llama_token t;
    if (mll_cancelled(cancel)) {
      e = ASNGN_ERR_CANCELLED;
      goto out;
    }
    t = llama_sampler_sample(chain, u->lctx, -1);
    if (llama_vocab_is_eog(u->vocab, t)) break;
    e = mll_emit_piece(u->vocab, t, &outbuf, token_cb, token_ud);
    if (e != ASNGN_OK) goto out;
    produced++;
    if (produced >= limit || n_past >= u->n_ctx) break;
    {
      struct llama_batch batch = llama_batch_get_one(&t, 1);
      if (llama_decode(u->lctx, batch) != 0) {
        e = mll_cancelled(cancel) ? ASNGN_ERR_CANCELLED : ASNGN_ERR_MODEL;
        goto out;
      }
    }
    n_past++;
  }

  *out_tokens_out = produced;
  *out_text = asngn_buf_detach(&outbuf); /* strdup("") when empty */
  if (*out_text == NULL) {
    e = ASNGN_ERR_NOMEM;
    goto out;
  }
  e = ASNGN_OK;

out:
  mll_set_cancel(u, NULL);
  if (chain != NULL) llama_sampler_free(chain);
  free(tok);
  free(prompt);
  asngn_buf_free(&outbuf);
  if (e != ASNGN_OK && *out_text != NULL) {
    free(*out_text);
    *out_text = NULL;
  }
  return e;
}

/* ---- token counting ----------------------------------------------------- */

static int mll_count_tokens(void *ud, const char *text) {
  mll_ud *u = (mll_ud *)ud;
  int32_t n;

  if (text == NULL) return 0;
  n = llama_tokenize(u->vocab, text, (int32_t)strlen(text), NULL, 0, false,
                     false);
  if (n == INT32_MIN) return -1;
  return n < 0 ? (int)-n : (int)n;
}

/* ---- embedding ---------------------------------------------------------- */

static asngn_err mll_embed(void *ud, const char *text, float *out) {
  mll_ud *u = (mll_ud *)ud;
  llama_token *tok = NULL;
  int32_t n_tok = 0;
  const float *emb;
  double norm;
  asngn_err e;
  int i, rc;

  if (text == NULL) text = "";
  e = mll_tokenize(u->vocab, text, true, false, &tok, &n_tok);
  if (e == ASNGN_OK && n_tok == 0) {
    /* Some vocabs tokenize "" to nothing even with add_special; embed a
     * single space so the call still produces a vector. */
    e = mll_tokenize(u->vocab, " ", true, false, &tok, &n_tok);
    if (e == ASNGN_OK && n_tok == 0) e = ASNGN_ERR_MODEL;
  }
  if (e != ASNGN_OK) {
    free(tok);
    return e;
  }
  if (n_tok > u->n_ctx)
    n_tok = u->n_ctx; /* truncate: whole-input embedding is best effort */

  /* Reset any sequence state left by the previous call (NULL-safe for
   * memory-less encoder contexts). */
  llama_memory_clear(llama_get_memory(u->lctx), true);

  /* llama_batch_get_one: seq 0, auto positions; with embeddings enabled
   * every token is an output, which mean pooling requires. */
  {
    struct llama_batch batch = llama_batch_get_one(tok, n_tok);
    rc = u->encoder_only ? llama_encode(u->lctx, batch)
                         : llama_decode(u->lctx, batch);
  }
  free(tok);
  if (rc != 0) return ASNGN_ERR_MODEL;

  emb = llama_get_embeddings_seq(u->lctx, 0);
  if (emb == NULL) return ASNGN_ERR_MODEL;

  norm = 0.0;
  for (i = 0; i < u->dim; i++) norm += (double)emb[i] * (double)emb[i];
  norm = sqrt(norm);
  if (norm > 0.0) {
    for (i = 0; i < u->dim; i++) out[i] = (float)((double)emb[i] / norm);
  } else {
    for (i = 0; i < u->dim; i++) out[i] = 0.0f;
  }
  return ASNGN_OK;
}

/* ---- lifecycle ---------------------------------------------------------- */

static void mll_destroy(void *ud) {
  mll_ud *u = (mll_ud *)ud;

  if (u == NULL) return;
  if (u->lctx != NULL) llama_free(u->lctx);
  if (u->model != NULL) llama_model_free(u->model);
  os_mutex_destroy(&u->cancel_mu);
  free(u);
}

asngn_err asngn_model_llama_create(asngn_ctx *c, const asngn_pool_entry *ent,
                                   asngn_model_iface *out) {
  struct llama_model_params mparams;
  struct llama_context_params cparams;
  mll_ud *u;
  asngn_err e;
  int hw, threads, n_ctx;

  memset(out, 0, sizeof *out);
  if (ent == NULL || ent->path == NULL || ent->path[0] == '\0')
    return asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s': path is not set",
                        ent != NULL ? ent->id : "?");

  mll_backend_init();

  u = (mll_ud *)calloc(1, sizeof *u);
  if (u == NULL) return asngn_seterr(c, ASNGN_ERR_NOMEM, "out of memory");
  os_mutex_init(&u->cancel_mu);
  u->embedding = ent->embedding;

  mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;
  u->model = llama_model_load_from_file(ent->path, mparams);
  if (u->model == NULL) {
    e = asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s': failed to load %s",
                     ent->id, ent->path);
    goto fail;
  }
  u->vocab = llama_model_get_vocab(u->model);
  if (u->vocab == NULL) {
    e = asngn_seterr(c, ASNGN_ERR_MODEL, "model '%s': no vocab in %s",
                     ent->id, ent->path);
    goto fail;
  }

  hw = os_hardware_threads();
  threads = ent->threads > 0 ? ent->threads : 4;
  if (threads > hw) threads = hw;

  cparams = llama_context_default_params();
  cparams.n_threads = threads;
  cparams.n_threads_batch = threads;
  /* Cooperative cancellation inside long llama_decode calls: the
   * installed per-call cancel flag aborts the graph compute (CPU
   * backend only; the per-token checks in mll_generate cover the
   * rest). */
  cparams.abort_callback = mll_abort_cb;
  cparams.abort_callback_data = u;

  if (u->embedding) {
    u->dim = (int)llama_model_n_embd_out(u->model);
    if (u->dim <= 0) {
      e = asngn_seterr(c, ASNGN_ERR_MODEL,
                       "model '%s': no usable embedding output", ent->id);
      goto fail;
    }
    /* The pool entry's dim sizes every caller's output buffer; a
     * mismatch with the actual weights would corrupt memory. */
    if (ent->dim > 0 && ent->dim != u->dim) {
      e = asngn_seterr(c, ASNGN_ERR_MODEL,
                       "model '%s': embedding dim %d does not match "
                       "configured dim %d",
                       ent->id, u->dim, ent->dim);
      goto fail;
    }
    u->encoder_only = llama_model_has_encoder(u->model) &&
                      !llama_model_has_decoder(u->model);
    n_ctx = ent->ctx > 0 ? ent->ctx : MLL_EMBED_CTX;
    cparams.n_ctx = (uint32_t)n_ctx;
    cparams.n_batch = (uint32_t)n_ctx;
    cparams.n_ubatch = (uint32_t)n_ctx; /* pooling: input in one ubatch */
    cparams.embeddings = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
  } else {
    u->chat_template = llama_model_chat_template(u->model, NULL);
    n_ctx = ent->ctx > 0 ? ent->ctx : 4096;
    cparams.n_ctx = (uint32_t)n_ctx;
    cparams.n_batch = MLL_N_BATCH;
    cparams.n_ubatch = MLL_N_BATCH;
  }

  u->lctx = llama_init_from_model(u->model, cparams);
  if (u->lctx == NULL) {
    e = asngn_seterr(c, ASNGN_ERR_MODEL,
                     "model '%s': failed to create context", ent->id);
    goto fail;
  }
  u->n_ctx = (int)llama_n_ctx(u->lctx);
  u->n_batch = (int)llama_n_batch(u->lctx);
  if (u->n_batch <= 0) u->n_batch = MLL_N_BATCH;
  /* Embedding truncation bound: the whole input must fit both the
   * context and one logical batch (llama may round n_ctx up past the
   * request). */
  if (u->embedding && u->n_batch < u->n_ctx) u->n_ctx = u->n_batch;

  out->ud = u;
  out->count_tokens = mll_count_tokens;
  out->destroy = mll_destroy;
  if (u->embedding)
    out->embed = mll_embed;
  else
    out->generate = mll_generate;

  if (u->embedding)
    asngn_log(c, ASNGN_LOG_INFO, "model",
              "loaded '%s' (embedding, dim=%d, n_ctx=%d, threads=%d, %s)",
              ent->id, u->dim, u->n_ctx, threads,
              u->encoder_only ? "encoder" : "decoder");
  else
    asngn_log(c, ASNGN_LOG_INFO, "model",
              "loaded '%s' (n_ctx=%d, threads=%d, params=%lluM, "
              "template=%s)",
              ent->id, u->n_ctx, threads,
              (unsigned long long)(llama_model_n_params(u->model) /
                                   1000000u),
              u->chat_template != NULL ? "model" : "chatml");
  return ASNGN_OK;

fail:
  mll_destroy(u);
  memset(out, 0, sizeof *out);
  return e;
}

#else /* !ASNGN_WITH_LLAMA */

asngn_err asngn_model_llama_create(asngn_ctx *c, const asngn_pool_entry *e,
                                   asngn_model_iface *out) {
  (void)e;
  memset(out, 0, sizeof *out);
  return asngn_seterr(c, ASNGN_ERR_MODEL,
                      "libasngn built without llama.cpp support");
}

#endif /* ASNGN_WITH_LLAMA */
