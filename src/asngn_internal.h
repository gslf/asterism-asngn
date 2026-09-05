/*
 * asngn_internal.h — internal contracts of libasngn.
 *
 * One header for every internal module, mirroring the sibling discipline
 * (asper_internal.h / astools_internal.h). Tests include this directly.
 *
 * Locking protocol:
 *   - c->lock      rwlock: sessions table, stats, budgets.
 *   - s->lock      rwlock: one per session (transcript, summary, pins).
 *   - c->models_mu registry mutex for the model pool; each slot has its
 *                  own mutex, taken while a call runs on that instance.
 *   - c->cache_mu  rwlock for the semantic + tool caches.
 *   - c->tele_mu   telemetry ring; never held while taking other locks.
 *   - c->log_mu, c->err_mu as in the siblings.
 *   Registry lock is released before taking session locks. Commit accounting
 *   may take c->lock under s->lock; cache_mu precedes models_mu. tele_mu/log_mu
 *   are leaves.
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_INTERNAL_H
#define ASNGN_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "asngn.h"
#include "asmodel.h"
#include "os.h"

struct asper_ctx;
struct astools_ctx;

/* ── growable buffer (util.c) ─────────────────────────────────────────── */

typedef struct {
  char  *data; /* always NUL-terminated when len > 0 or after init */
  size_t len;
  size_t cap;
} asngn_buf;

void      asngn_buf_init(asngn_buf *b);
void      asngn_buf_free(asngn_buf *b);
asngn_err asngn_buf_append(asngn_buf *b, const void *data, size_t len);
asngn_err asngn_buf_appends(asngn_buf *b, const char *s);
asngn_err asngn_buf_appendc(asngn_buf *b, char ch);
asngn_err asngn_buf_printf(asngn_buf *b, const char *fmt, ...);
/* Detach the NUL-terminated string; buffer resets to empty. NULL on OOM
 * or empty buffer (returns a fresh strdup("") for empty). */
char     *asngn_buf_detach(asngn_buf *b);

/* ── small utilities (util.c) ─────────────────────────────────────────── */

char *asngn_strdup(const char *s);            /* NULL-safe: NULL -> NULL   */
char *asngn_strndup(const char *s, size_t n);
/* Session/project slug: [a-z0-9][a-z0-9-]{0,63}. */
bool  asngn_slug_valid(const char *s);
/* UTF-8 validation over the whole buffer. */
bool  asngn_utf8_valid(const char *s, size_t len);
/* Strip control characters except \n and \t, in place; returns new len. */
size_t asngn_strip_controls(char *s, size_t len);
/* Case-insensitive ASCII compare. */
int   asngn_strcasecmp(const char *a, const char *b);
bool  asngn_str_has_prefix(const char *s, const char *prefix);
/* FNV-1a 64-bit over bytes. */
uint64_t asngn_fnv1a64(const void *data, size_t len);
/* Trim trailing text after the last sentence boundary (. ! ? followed by
 * space/EOL, or a newline); returns the trimmed length (never grows).
 * Falls back to the full length when no boundary exists. */
size_t asngn_sentence_trim(const char *s, size_t len);

/* ── time and clock (time.c) ──────────────────────────────────────────── */

typedef long long asngn_time; /* unix seconds, UTC */

typedef struct {
  void      *ud;
  asngn_time (*now)(void *ud);     /* wall clock, unix seconds UTC */
  int64_t    (*mono_ms)(void *ud); /* monotonic milliseconds       */
} asngn_clock;

asngn_time  asngn_clock_now(const asngn_clock *clk);
int64_t     asngn_clock_mono_ms(const asngn_clock *clk);
asngn_clock asngn_clock_system(void);

/* RFC 3339 UTC: "YYYY-MM-DDTHH:MM:SSZ" (20 chars + NUL). */
void  asngn_time_format_rfc3339(asngn_time t, char out[21]);
bool  asngn_time_parse_rfc3339(const char *s, asngn_time *out);
/* ISO 8601 duration (PnW | PnD | PTnHnMnS mixes; months/years and
 * fractions rejected), result in seconds; must be positive. */
bool  asngn_duration_parse(const char *s, int64_t *out_seconds);

/* ── uuid (uuid.c) ────────────────────────────────────────────────────── */

/* Random (v4) UUID, lowercase hex, 36 chars + NUL. */
void asngn_uuid_v4(char out[37]);
bool asngn_uuid_valid(const char *s);
/* 36-char text <-> 16 raw bytes. */
bool asngn_uuid_to_bytes(const char *s, uint8_t out[16]);
void asngn_uuid_from_bytes(const uint8_t in[16], char out[37]);

/* ── sha256 (sha256.c) ────────────────────────────────────────────────── */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t  buffer[64];
  size_t   buflen;
} asngn_sha256_ctx;

void asngn_sha256_init(asngn_sha256_ctx *ctx);
void asngn_sha256_update(asngn_sha256_ctx *ctx, const void *data, size_t len);
void asngn_sha256_final(asngn_sha256_ctx *ctx, uint8_t out[32]);
void asngn_sha256(const void *data, size_t len, uint8_t out[32]);
asngn_err asngn_sha256_file(const char *path, uint8_t out[32]);
/* Lowercase hex of the first n bytes (n <= 32), out needs 2n+1. */
void asngn_sha256_hex(const uint8_t hash[32], size_t n, char *out);

/* ── error reporting (api.c) ──────────────────────────────────────────── */

/* Set the per-context error message and return e (tail-call friendly). */
asngn_err asngn_seterr(asngn_ctx *c, asngn_err e, const char *fmt, ...);

/* ── logging (log.c) ──────────────────────────────────────────────────── */

asngn_err asngn_log_open(asngn_ctx *c);  /* no-op when logging.path unset */
void      asngn_log_close(asngn_ctx *c);
/* subsys: session context cache route loop model safety judge telemetry
 * tui mcp asper astools. Callback sink gets every record; the file
 * sink is gated by cfg.log_level. Never fails the caller. */
void asngn_log(asngn_ctx *c, int level, const char *subsys,
               const char *fmt, ...);

/* ── xCDN helpers (xutil.c) ───────────────────────────────────────────── */
/* Checked builders over xcdn-c's void mutators (which cannot report OOM):
 * each helper pre-grows with realloc, and consumes/frees its argument on
 * failure, so durable writes are never silently dropped. */

struct xcdn_value;
struct xcdn_node;
struct xcdn_document;

bool asngn_xobj_put(struct xcdn_value *obj, const char *key,
                    struct xcdn_value *val);            /* wraps in a node */
bool asngn_xobj_put_node(struct xcdn_value *obj, const char *key,
                         struct xcdn_node *node);
bool asngn_xarr_push(struct xcdn_value *arr, struct xcdn_value *val);
bool asngn_xnode_tag(struct xcdn_node *node, const char *name);
bool asngn_xdoc_push(struct xcdn_document *doc, struct xcdn_node *node);

/* Field access: NULL-safe, type-strict. */
const struct xcdn_value *asngn_xfield(const struct xcdn_value *obj,
                                      const char *key);
const char *asngn_xstr(const struct xcdn_value *v);   /* STRING only      */
bool asngn_xint(const struct xcdn_value *v, int64_t *out);
bool asngn_xnum(const struct xcdn_value *v, double *out); /* INT or FLOAT */
bool asngn_xbool(const struct xcdn_value *v, bool *out);
bool asngn_xtime(const struct xcdn_value *v, asngn_time *out); /* DATETIME */
bool asngn_xdur(const struct xcdn_value *v, int64_t *out_s);   /* DURATION */
bool asngn_xuuid(const struct xcdn_value *v, char out[37]);    /* UUID     */

/* Serialize one node (borrowed) wrapped in a temporary document. */
asngn_err asngn_xnode_write(const struct xcdn_node *node, bool pretty,
                            asngn_buf *out);

/* ── append streams with the torn-tail rule (stream.c) ────────────────── */

/* An append stream is a FILE* opened "ab" plus its path; every append is
 * one compact single-line value + '\n', flushed, with offset rollback on
 * partial writes (the asper journal discipline). */
typedef struct {
  FILE *fp;
  char *path;
  bool  sync; /* fsync per append */
} asngn_stream;

asngn_err asngn_stream_open(asngn_ctx *c, asngn_stream *st,
                            const char *path, bool sync);
void      asngn_stream_close(asngn_stream *st);
/* line need not end in '\n'; one is added. */
asngn_err asngn_stream_append(asngn_ctx *c, asngn_stream *st,
                              const char *line, size_t len);

/* Read a whole stream file applying the torn-tail rule: on a parse
 * failure confined to the final value the file is truncated to the last
 * good offset with a WARN; a parse error before the tail is fatal
 * (ASNGN_ERR_PARSE). Missing file: *out_doc = NULL, ASNGN_OK. Caller
 * frees *out_doc with xcdn_document_free. `what` names the file in logs. */
asngn_err asngn_stream_load(asngn_ctx *c, const char *path, const char *what,
                            struct xcdn_document **out_doc);

/* Atomic replace: write <path>.tmp, fsync, close, rename over path. */
asngn_err asngn_write_atomic(asngn_ctx *c, const char *path,
                             const char *data, size_t len);

/* ── configuration (config.c) ─────────────────────────────────────────── */

typedef enum { ASNGN_CLASSIFIER_HEURISTIC = 0, ASNGN_CLASSIFIER_MODEL,
               ASNGN_CLASSIFIER_HYBRID } asngn_classifier_mode;
typedef enum { ASNGN_JUDGE_OFF = 0, ASNGN_JUDGE_LIGHT,
               ASNGN_JUDGE_FULL } asngn_judge_mode;
typedef enum { ASNGN_CONFIRM_PROMPT = 0, ASNGN_CONFIRM_DENY,
               ASNGN_CONFIRM_ALLOW } asngn_confirm_mode;
typedef enum { ASNGN_SCOPE_SESSION = 0, ASNGN_SCOPE_GLOBAL } asngn_cache_scope;
typedef enum { ASNGN_CATALOG_INDEX = 0, ASNGN_CATALOG_SUMMARY,
               ASNGN_CATALOG_FULL } asngn_catalog_level;
typedef enum { ASNGN_PROFILE_GENERAL = 0, ASNGN_PROFILE_CODING }
    asngn_profile;
typedef enum { ASNGN_THEME_ASTERISM = 0, ASNGN_THEME_PLAIN } asngn_theme;
typedef enum { ASNGN_TRUECOLOR_AUTO = 0, ASNGN_TRUECOLOR_ON,
               ASNGN_TRUECOLOR_OFF } asngn_truecolor;

#define ASNGN_MAX_POOL 8

typedef struct {
  char  id[32];
  char *path;       /* GGUF path, owned */
  asmodel_backend backend;
  char *base_url;
  char *remote_model;
  char *api_key_env;
  asmodel_remote_provider remote_provider;
  int   ctx;
  int   threads;
  bool  embedding;
  int   dim;
  int   gpu_layers; /* layers offloaded to VRAM; -1 = all (default), 0 =
                       CPU only. No-op in CPU-only llama builds. */
  size_t ram_mb, vram_mb;
  bool warm, kv_cache;
} asngn_pool_entry;

typedef struct {
  double temp;
  double top_p;
  int    max_tokens;      /* 0 = task default */
  double repeat_penalty;  /* <= 1.0 = off; breaks greedy phrase loops */
} asngn_sampling;

typedef struct {
  /* engine */
  char *root;                /* engine root (resolved), owned */
  char *base_prompt;
  asngn_profile profile;
  /* models */
  asngn_pool_entry pool[ASNGN_MAX_POOL];
  size_t pool_n;
  char role_router[32], role_planner[32], role_generator[32],
       role_compressor[32], role_adapter[32], role_judge[32],
       role_embedder[32];
  int scheduler_workers, queue_capacity;
  int  max_resident;
  int  max_ram_mb, max_vram_mb;
  asngn_sampling s_classify, s_decide, s_draft, s_answer, s_compress,
                 s_adapt, s_judge;
  /* routing */
  asngn_classifier_mode classifier;
  int max_escalations;
  /* detail */
  asngn_detail detail_default; /* AUTO = classifier decides */
  int terse_tokens, normal_tokens, rich_tokens;
  /* context */
  int memory_checkpoint_tokens, memory_history_tokens, working_tokens;
  int safety_margin; /* reserved beyond requested completion tokens */
  int digest_threshold_chars, digest_tokens;
  int pinned_max;
  /* cache */
  bool cache_enable;
  asngn_cache_scope cache_scope;
  double hit_threshold, adapt_threshold;
  int64_t cache_ttl_s;
  int cache_max_entries;
  bool tool_cache;
  int64_t tool_ttl_s;
  int tool_max_entries;
  /* safety */
  int max_steps, max_tool_calls, think_limit;
  int64_t turn_deadline_s, stall_timeout_s;
  asngn_confirm_mode autoconfirm;
  bool redact_context;
  /* validation */
  asngn_judge_mode judge;
  int judge_threshold; /* of 10 */
  /* budgets */
  int64_t session_tokens, daily_tokens; /* 0 = unlimited */
  double warn_at;
  /* telemetry */
  int tele_ring;
  char *tele_path;   /* nullable */
  int tele_rotate_kb, tele_max_files;
  /* logging */
  char *log_path;    /* nullable */
  int log_level;     /* ASNGN_LOG_* */
  int log_max_size_kb, log_max_files;
  bool log_sync;
  /* tui */
  asngn_theme theme;
  asngn_truecolor truecolor;
  int fps_cap;
  char sidebar[8];   /* "trace" "stats" "memory" "tools" "cache" */
  /* integration */
  bool asper_enable;
  char *asper_root, *asper_config;   /* owned; root relative to engine root */
  bool astools_enable;
  char *astools_root, *astools_workspace, *astools_config;
  asngn_catalog_level catalog_level;
  int catalog_chars;
  /* mcp */
  asngn_confirm_mode mcp_autoconfirm;
} asngn_config;

void      asngn_config_defaults(asngn_config *cfg);
asngn_err asngn_config_load(asngn_ctx *c, asngn_config *cfg,
                            const char *path); /* NULL = defaults only */
void      asngn_config_free(asngn_config *cfg);

/* ── model runtime (models.c, models_llama.c) ─────────────────────────── */

typedef struct {
  double temp;
  double top_p;           /* <= 0: greedy chain without top_p */
  int    max_tokens;
  double repeat_penalty;  /* <= 1.0 = off */
  asmodel_reasoning_mode reasoning;
  int reasoning_budget;
  bool require_constraint;
  int64_t deadline_ms;   /* maximum duration for this inference; 0 = none */
} asngn_gen_params;

/* Backend vtable: scripted fakes and llama.cpp sit behind the same
 * interface. generate() applies the model's chat template to the
 * (system, user) pair; gbnf NULL = unconstrained; token_cb may be NULL;
 * *cancel is polled at least once per produced token; out_tokens_in /
 * out_tokens_out report exact prompt/generated token counts. */
typedef struct asngn_model_iface {
  void *ud;
  asngn_err (*generate)(void *ud, const char *system_prompt,
                        const char *user_prompt, const char *gbnf,
                        const asngn_gen_params *p,
                        asngn_token_fn token_cb, void *token_ud,
                        volatile int *cancel,
                        char **out_text, int *out_tokens_in,
                        int *out_tokens_out);
  int  (*count_tokens)(void *ud, const char *text); /* < 0 on error */
  /* Exact chat-template-aware prompt count; optional for injected legacy
   * backends, where the engine uses a conservative fallback. */
  int  (*count_prompt_tokens)(void *ud, const char *system_prompt,
                              const char *user_prompt);
  asngn_err (*embed)(void *ud, const char *text, float *out); /* dim floats,
                         L2-normalized; only on embedding models */
  const char *(*last_error)(void *ud); /* optional backend diagnostic */
  int (*last_generation_info)(void *ud, asmodel_generation_info *out);
  void (*destroy)(void *ud);
} asngn_model_iface;

typedef struct {
  asngn_pool_entry  cfg;      /* copy of the pool entry                  */
  asngn_model_iface iface;    /* valid when state == LOADED or injected  */
  bool              injected; /* fake provided at open; never unloaded   */
  bool              loaded;
  int64_t           last_used_ms;
  os_mutex          mu;       /* held while a call runs on the instance  */
} asngn_model_slot;

typedef enum { ASNGN_ROLE_ROUTER = 0, ASNGN_ROLE_PLANNER, ASNGN_ROLE_GENERATOR,
               ASNGN_ROLE_COMPRESSOR, ASNGN_ROLE_ADAPTER, ASNGN_ROLE_JUDGE,
               ASNGN_ROLE_EMBEDDER, ASNGN_ROLE_COUNT } asngn_role;
const char *asngn_role_name(asngn_role r);

/* Task kinds drive default sampling. */
typedef enum { ASNGN_TASK_CLASSIFY = 0, ASNGN_TASK_DECIDE, ASNGN_TASK_DRAFT,
               ASNGN_TASK_ANSWER, ASNGN_TASK_COMPRESS, ASNGN_TASK_ADAPT,
               ASNGN_TASK_JUDGE } asngn_task_kind;
const char *asngn_task_name(asngn_task_kind t);

asngn_err asngn_models_init(asngn_ctx *c);
void      asngn_models_shutdown(asngn_ctx *c);
/* Resolve a role to its pool slot index; -1 if unmapped. */
int       asngn_models_slot_for_role(asngn_ctx *c, asngn_role role);
/* Load every role-mapped model now (background worker, at open): the
 * first turn then starts hot instead of paying the loads lazily. */
void      asngn_models_warm(asngn_ctx *c);
/* Slot for an explicit pool id (escalation walks the tier ladder). */
int       asngn_models_slot_for_id(asngn_ctx *c, const char *id);
/* Run one generation on a role's model: lazy-loads, takes the instance
 * mutex, applies sampling defaults for `task` overlaid with cfg.sampling,
 * emits a model_call telemetry event, and accounts tokens. */
asngn_err asngn_models_generate(asngn_ctx *c, int slot, asngn_task_kind task,
                                const char *system_prompt,
                                const char *user_prompt, const char *gbnf,
                                int max_tokens_override,
                                int64_t deadline_mono,
                                asngn_token_fn token_cb, void *token_ud,
                                volatile int *cancel,
                                char **out_text, int *out_tokens_in,
                                int *out_tokens_out);
/* Count tokens with the tokenizer of `slot`'s model; heuristic
 * (UTF-8 bytes / 4, min 1) when the slot is unavailable (fallback for
 * degraded contexts only). */
int       asngn_models_count_tokens(asngn_ctx *c, int slot, const char *text);
int       asngn_models_count_prompt(asngn_ctx *c, int slot,
                                    const char *system_prompt,
                                    const char *user_prompt);
/* Embed with the embedder role; out has embedder dim floats. */
asngn_err asngn_models_embed(asngn_ctx *c, const char *text, float *out);
int       asngn_models_embed_dim(asngn_ctx *c); /* <= 0 when unavailable */
/* SHA-256 of the embedder weights file (zeroes when unavailable). */
void      asngn_models_embed_hash(asngn_ctx *c, uint8_t out[32]);

/* llama.cpp backend factory (models_llama.c; stub without ASNGN_WITH_LLAMA). */
asngn_err asngn_model_llama_create(asngn_ctx *c, const asngn_pool_entry *e,
                                   asngn_model_iface *out);
asngn_err asngn_model_openai_create(asngn_ctx *c,
                                    const asngn_pool_entry *e,
                                    asngn_model_iface *out);
asngn_err asngn_shared_models_init(asngn_ctx *c);
void asngn_shared_models_shutdown(asngn_ctx *c);

/* ── token estimation (tokens.c) ──────────────────────────────────────── */

int asngn_token_heuristic(const char *text); /* UTF-8 bytes / 4, min 1 */

/* ── session store (session.c) ────────────────────────────────────────── */

typedef struct {
  size_t      n;          /* turn ordinal, 1-based                  */
  char        workspace[1024], commit[65], project[65];
  char        turn_id[37]; /* transaction UUID, shared by user and assistant */
  char        event_id[37]; /* authoritative Asper source event id */
  char        role[10];   /* "user" | "assistant"                   */
  char       *text;       /* owned                                  */
  asngn_time  at;
  bool        pinned;
  /* route summary, assistant turns only (empty strings otherwise)   */
  char        klass[12], detail[8], mode[8], tier[16], cache[8];
  int         steps;
} asngn_turn;

typedef struct {
  char   *id;        /* invocation id (uuid), owned                 */
  char   *label;     /* "fs.read" etc., owned                       */
  char    object_ref[72]; /* authoritative Asper sha256 reference    */
  size_t  size;      /* full text size in bytes                     */
  size_t  slice_off; /* next OPEN offset                            */
} asngn_blob;

struct asngn_session {
  asngn_ctx  *ctx;
  char        slug[65];
  char       *dir;          /* sessions/<slug> absolute, owned      */
  os_rwlock   lock;
  char *active_file, *objective;
  void *code_index;
  /* manifest */
  asngn_time  created_at;
  size_t      turns;        /* committed turn pairs counter          */
  uint64_t    world_epoch;
  char       *project;      /* active Asper project or NULL          */
  asngn_workspace_info workspace; /* immutable session binding       */
  bool        workspace_loaded;
  bool        redact_context;
  asngn_usage_mode usage_mode;
  asngn_security_profile security_profile;
  /* process-local view of Asper source events */
  asngn_turn *log;
  size_t      log_n, log_cap;
  /* turn-local handles for Asper content-addressed objects */
  asngn_blob *blobs;
  size_t      blobs_n, blobs_cap;
  /* appenders */
  asngn_stream ledger_st;
  asngn_stream journal_st;
  bool recovery_required;
  size_t interrupted_turns, uncertain_actions;
  /* session allowlist: "tool.command" entries confirmed "always"    */
  char      **allow;
  size_t      allow_n;
  /* resident ledger (small; drives stats, QpT, feedback, export) */
  struct asngn_ledger_entry_s *led;
  size_t      led_n, led_cap;
  int64_t     spent_tokens;  /* session lifetime total               */
  /* turn serialization */
  bool        busy;          /* a turn runs                           */
  asngn_task *running;       /* borrowed pointer to the active task  */
  size_t      last_answer_turn; /* for /more and feedback            */
  bool        last_capped;
  char       *last_user_msg; /* owned; for /more continuation        */
  char       *last_answer;   /* owned; for /more continuation        */
};

asngn_err asngn_session_load(asngn_ctx *c, const char *slug,
                             asngn_session **out); /* create if missing */
asngn_err asngn_session_workspace_activate(asngn_session *s);
void      asngn_session_free(asngn_session *s);
asngn_err asngn_session_save_manifest(asngn_session *s);
/* Append one turn to Asper and the in-memory operational view. */
asngn_err asngn_session_append_turn(asngn_session *s, const asngn_turn *t);
asngn_err asngn_session_add_blob(asngn_session *s, const char *invocation_id,
                                 const char *label, const char *text,
                                 size_t len, asngn_blob **out);
void      asngn_session_clear_blobs(asngn_session *s); /* delete files  */

/* ── workspace (workspace.c) ──────────────────────────────────────────── */

/* Scale and dominant language of the workspace tree, collected for free
 * during the fingerprint walk of asngn_workspace_refresh. */
typedef struct {
  size_t files;         /* files visited by the fingerprint walk      */
  size_t bytes;         /* their cumulative size                      */
  char   language[16];  /* dominant code language by file count; ""   */
  bool   loaded;
} asngn_repo_stats;

asngn_err asngn_workspace_init(asngn_ctx *c, const asngn_open_params *p);
asngn_err asngn_workspace_refresh(asngn_ctx *c);
asngn_err asngn_workspace_info_init(asngn_ctx *c, const char *root,
                                    asngn_workspace_info *out);
asngn_err asngn_workspace_info_refresh(asngn_ctx *c,
                                       asngn_workspace_info *workspace);
void      asngn_workspace_hash(asngn_ctx *c, uint8_t out[32]);

/* ── ledger (ledger.c) ────────────────────────────────────────────────── */

typedef struct asngn_ledger_entry_s {
  char       turn_id[37];
  size_t     turn;
  asngn_time at;
  char       klass[12], detail[8], mode[8], tier[16], cache[8];
  int        escalations;
  size_t     pt_system, pt_memory, pt_catalog, pt_summary, pt_verbatim,
             pt_working;
  size_t     gt_decision, gt_answer, gt_aux;
  size_t     sv_cache, sv_digest;
  double     judge;        /* [0,1]; valid when has_judge            */
  bool       has_judge;
  int        user_fb;      /* -1 / +1; valid when has_user_fb        */
  bool       has_user_fb;
  bool       capped;
  uint64_t   duration_ms;
} asngn_ledger_entry;

asngn_err asngn_ledger_append(asngn_session *s, const asngn_ledger_entry *e);
/* Load ledger totals into the session (spent tokens, QpT window). */
asngn_err asngn_ledger_replay(asngn_session *s);
/* Rewrite entry `turn`'s quality.user (feedback after commit). */
asngn_err asngn_ledger_set_feedback(asngn_session *s, size_t turn,
                                    int signal);
double    asngn_qpt(double q, size_t total_tokens);
size_t    asngn_ledger_total_tokens(const asngn_ledger_entry *e);

/* ── telemetry (telemetry.c) ──────────────────────────────────────────── */

typedef struct {
  char **items;   /* serialized #asngn_event values, owned          */
  size_t cap, n, head;
} asngn_tele_ring;

asngn_err asngn_tele_init(asngn_ctx *c);
void      asngn_tele_shutdown(asngn_ctx *c);
/* Emit one event: data_xcdn is a compact xCDN object (borrowed) or NULL
 * for {}. span/parent NULL = omitted. Never fails the caller. */
void asngn_tele_emit(asngn_ctx *c, const char *kind, const char *span,
                     const char *parent, const char *session, size_t turn,
                     const char *data_xcdn);
/* Copy of the most recent n events, oldest first. */
asngn_err asngn_tele_tail(asngn_ctx *c, size_t n, char ***out, size_t *out_n);
void      asngn_tele_flush(asngn_ctx *c); /* file sink batch + rotation  */

/* ── context engine (context.c) ───────────────────────────────────────── */

typedef struct {
  char  *system_text;  /* zones 1-4: system+memory+catalog+summary   */
  char  *user_text;    /* zones 5-6: verbatim+working+instruction    */
  size_t tok_system, tok_memory, tok_catalog, tok_summary, tok_verbatim,
         tok_working;
} asngn_prompt;

void asngn_prompt_free(asngn_prompt *p);

struct asngn_turn_state; /* below */

/* Assemble the zoned prompt for one model call. `instruction` is the
 * task-specific trailer (step protocol prompt or answer directive);
 * `count_slot` selects the tokenizer used for budgets. Deterministic:
 * byte-identical for identical inputs. */
asngn_err asngn_context_assemble(asngn_ctx *c, asngn_session *s,
                                 struct asngn_turn_state *t,
                                 const char *base_override,
                                 const char *instruction,
                                 int count_slot, asngn_prompt *out);
asngn_err asngn_context_validate(asngn_ctx *c, int count_slot,
                                 const asngn_prompt *prompt,
                                 int output_reserve);
asngn_err asngn_context_validate_text(asngn_ctx *c, int count_slot,
                                      const char *system_text,
                                      const char *user_text,
                                      int output_reserve);

/* ── digestion and blobs (digest.c) ───────────────────────────────────── */

/* Digest `text` (a tool result / recall answer) when it exceeds
 * digest_threshold_chars: writes the blob, returns the digested working
 * line "[B<n> · label · size · digested] ..." in *out (owned). When under
 * threshold, *out is NULL (caller uses the original). */
asngn_err asngn_digest_item(asngn_ctx *c, asngn_session *s,
                            struct asngn_turn_state *t, const char *label,
                            const char *text, size_t len,
                            size_t *saved_tokens, size_t *aux_tokens,
                            char **out);
/* Next OPEN slice of blob b, at most max_chars; NULL when exhausted. */
asngn_err asngn_digest_open_slice(asngn_ctx *c, asngn_session *s,
                                  asngn_blob *b, size_t max_chars,
                                  char **out);

/* ── embedding cache (embedcache.c) ───────────────────────────────────── */
/* cache/embeddings.bin — magic "ASNG", little-endian, layout identical to
 * Asper's: u32 version=1, u32 dim, u64 count, 32 B model hash, then per
 * entry 16 B uuid + u64 content fnv + dim f32 (L2-normalized). */

#define ASNGN_EMBED_MAGIC   "ASNG"
#define ASNGN_EMBED_VERSION 1u

asngn_err asngn_embedcache_load(asngn_ctx *c);  /* degrades to rebuild   */
asngn_err asngn_embedcache_save(asngn_ctx *c);

/* ── semantic cache (cache.c) ─────────────────────────────────────────── */

typedef struct {
  char        id[37];
  asngn_cache_scope scope;
  char       *session;   /* owned, NULL when scope global            */
  char       *project;   /* owned, nullable                          */
  uint64_t    epoch;
  char       *query;     /* owned                                    */
  char       *answer;    /* owned                                    */
  char        detail[8];
  char        tier[16];
  size_t      gen_tokens;
  bool        tools_used;
  char      **tools;     /* tool.command labels when tools_used      */
  size_t      tools_n;
  asngn_time  created_at, last_hit;
  size_t      hit_count;
  int64_t     ttl_s;
  float      *vec;       /* owned, embedder dim, L2-normalized; may
                            be NULL until (re)embedded               */
  bool        touched;   /* pending #cache_touch                     */
} asngn_cache_entry;

typedef enum { ASNGN_CACHE_MISS = 0, ASNGN_CACHE_HIT,
               ASNGN_CACHE_ADAPT } asngn_cache_outcome;

typedef struct {
  asngn_cache_outcome outcome;
  double              cos;
  float              *query_vec; /* owned by probe result             */
  /* owned copies of the matched entry, taken under cache_mu so the
   * caller never touches cache memory (HIT/ADAPT only; NULL on MISS) */
  char               *query;
  char               *answer;
  char                detail[8];
  char                tier[16];
  size_t              gen_tokens;
} asngn_cache_probe_result;

asngn_err asngn_cache_init(asngn_ctx *c);      /* load + TTL sweep      */
void      asngn_cache_shutdown(asngn_ctx *c);  /* compact + free        */
/* Probe for `query` in session s (scope/project partitioning, epoch and
 * tools_used rules). adapt_bias lowers adapt_threshold. */
asngn_err asngn_cache_probe(asngn_ctx *c, asngn_session *s,
                            const char *query, double adapt_bias,
                            asngn_cache_probe_result *out);
void      asngn_cache_probe_free(asngn_cache_probe_result *p);
/* Closest tools_used entry for plan hints; *out_line owned or NULL. */
asngn_err asngn_cache_plan_hint(asngn_ctx *c, asngn_session *s,
                                const float *query_vec, char **out_line);
asngn_err asngn_cache_insert(asngn_ctx *c, asngn_session *s,
                             const char *query, const float *vec,
                             const char *answer, const char *detail,
                             const char *tier, size_t gen_tokens,
                             bool tools_used, char **tools, size_t tools_n);
asngn_err asngn_cache_clear_scope(asngn_ctx *c, const char *scope);
asngn_err asngn_cache_sweep(asngn_ctx *c);     /* TTL sweep             */
asngn_err asngn_cache_compact(asngn_ctx *c);   /* rewrite semantic.xcdn */
size_t    asngn_cache_count(asngn_ctx *c);

/* ── tool-result cache (toolcache.c) ──────────────────────────────────── */

typedef struct {
  uint8_t     key[32];   /* sha256(tool id + version + command + args) */
  char       *result_xcdn; /* owned                                    */
  asngn_time  at;
  int64_t     last_used_ms;
} asngn_toolcache_entry;

asngn_err asngn_toolcache_init(asngn_ctx *c);
void      asngn_toolcache_shutdown(asngn_ctx *c);
bool      asngn_toolcache_get(asngn_ctx *c, const uint8_t key[32],
                              char **out_result);
void      asngn_toolcache_put(asngn_ctx *c, const uint8_t key[32],
                              const char *result_xcdn);
void      asngn_toolcache_clear(asngn_ctx *c); /* world-epoch bump      */

/* ── routing (route.c) ────────────────────────────────────────────────── */

typedef enum { ASNGN_CLASS_SIMPLE = 0, ASNGN_CLASS_MODERATE,
               ASNGN_CLASS_COMPLEX } asngn_class;
typedef enum { ASNGN_MODE_DIRECT = 0, ASNGN_MODE_PLAN } asngn_mode;

/* Task kind read off the message, ordered by increasing demand: the
 * classifier's CLASS base score is a table over this enum. */
typedef enum {
  ASNGN_RTASK_CHAT = 0,  /* smalltalk, no ask                        */
  ASNGN_RTASK_LOOKUP,    /* factual question                         */
  ASNGN_RTASK_EXPLAIN,   /* explain / summarize / describe           */
  ASNGN_RTASK_EDIT,      /* targeted change to named material        */
  ASNGN_RTASK_BUILD,     /* compile / run tests / execute            */
  ASNGN_RTASK_GENERATE,  /* write new code or tests                  */
  ASNGN_RTASK_REFACTOR,  /* restructure existing code                */
  ASNGN_RTASK_DEBUG      /* diagnose and fix a failure               */
} asngn_route_task;

/* astools tool families the message implies (bitmask). */
enum {
  ASNGN_TOOLF_FS   = 1u << 0,
  ASNGN_TOOLF_GREP = 1u << 1,
  ASNGN_TOOLF_GIT  = 1u << 2,
  ASNGN_TOOLF_PROC = 1u << 3,
  ASNGN_TOOLF_EDIT = 1u << 4
};

/* Evidence the classifier weighs beyond the message text itself.
 * Collected by asngn_route_evidence_collect; a zeroed struct is valid
 * (no tools, no history, no workspace, no calibration). */
typedef struct {
  bool   tools_available;
  /* session history (ledger window, most recent entries) */
  int    window;            /* entries examined                       */
  int    escalated;         /* entries with escalations > 0           */
  int    unreliable;        /* judge below threshold or negative fb   */
  /* workspace scale */
  size_t repo_files;
  size_t repo_bytes;
  char   repo_language[16]; /* dominant code language; "" unknown     */
  /* eval-suite calibration (calibration/quality.xcdn)                */
  bool   has_eval;
  double eval_success;      /* task success rate in [0,1]             */
} asngn_route_evidence;

typedef struct {
  asngn_class      klass;
  asngn_detail     detail;  /* classifier vote (never AUTO on output) */
  asngn_mode       mode;
  asngn_route_task task;    /* heuristic verdict (evidence axis)      */
  unsigned         toolmask;/* implied ASNGN_TOOLF_* families         */
} asngn_route_profile;

/* Fill evidence from the engine: ledger window, repo stats, eval
 * calibration. s and t may be NULL (degrades to message-only). */
void asngn_route_evidence_collect(asngn_ctx *c, asngn_session *s,
                                  const struct asngn_turn_state *t,
                                  asngn_route_evidence *out);
/* Evidence-scored classification (pure; unit-tested against the table).
 * ev == NULL behaves as a zeroed evidence struct. */
void asngn_route_heuristic(const char *message,
                           const asngn_route_evidence *ev,
                           asngn_route_profile *out);
/* Full classification per routing.classifier (may run the nano pass).
 * aux_tokens accumulates classifier generation. */
asngn_err asngn_route_classify(asngn_ctx *c, asngn_session *s,
                               const char *message,
                               struct asngn_turn_state *t,
                               asngn_route_profile *out,
                               size_t *aux_tokens);
const char *asngn_route_task_name(asngn_route_task k); /* "chat" ...  */
/* Tier ladder: index of the slot one tier above/below `slot` following
 * pool declaration order over generative models; -1 when none. */
int asngn_route_tier_up(asngn_ctx *c, int slot);
int asngn_route_tier_down(asngn_ctx *c, int slot);

/* ── detail controller (detail.c) ─────────────────────────────────────── */

/* Effective level: user override ▷ pressure bias ▷ classifier ▷ default. */
asngn_detail asngn_detail_effective(asngn_ctx *c, asngn_detail user_override,
                                    asngn_detail classifier_vote,
                                    asngn_class klass, double pressure);
int          asngn_detail_cap(asngn_ctx *c, asngn_detail d);
const char  *asngn_detail_directive(asngn_detail d);
const char  *asngn_detail_name(asngn_detail d);   /* "terse" ...        */
/* In-message cues ("briefly", "in detail", ...) -> override or AUTO. */
asngn_detail asngn_detail_cue(const char *message);

/* ── budget pressure (api.c) ──────────────────────────────────────────── */

/* max(session_spent/session_budget, daily_spent/daily_budget); 0 when
 * both budgets are unlimited. s may be NULL (daily component only). */
double asngn_pressure(asngn_ctx *c, asngn_session *s);

/* Rolling QpT over the session's last 20 turns (ledger.c). */
double asngn_session_qpt(const asngn_session *s);

/* ── step protocol (steps.c, grammar.c) ───────────────────────────────── */

/* A decision pass emits one single-line schema-constrained action
 * object (JSON/xCDN-style, fixed key order, no string escapes):
 *
 *   {action: "call", why: "…", input: <tool>.<cmd> {…},
 *    success: "…", fallback: "…"}
 *   {action: "recall", why: "…", input: "…", success: "…", fallback: "…"}
 *   {action: "open", why: "…", input: "B<n>"}
 *   {action: "think", input: "…"}
 *   {action: "clarify", why: "…", input: "…"}
 *   {action: "answer"}
 *
 * Input payloads (THINK note, RECALL/CLARIFY question) are at most
 * ASNGN_STEP_TEXT_MAX bytes; the meta fields (why, success, fallback)
 * at most ASNGN_STEP_META_MAX. The step grammar bounds its `text` and
 * `meta` rules to the same counts, so at the limit the sampler is
 * forced onto the closing quote instead of rambling into the decide
 * max_tokens cap. */
#define ASNGN_STEP_TEXT_MAX 2048
#define ASNGN_STEP_META_MAX 512

typedef enum { ASNGN_STEP_CALL = 0, ASNGN_STEP_RECALL, ASNGN_STEP_OPEN,
               ASNGN_STEP_THINK, ASNGN_STEP_CLARIFY,
               ASNGN_STEP_ANSWER } asngn_step_kind;
const char *asngn_step_name(asngn_step_kind k);

typedef struct {
  asngn_step_kind kind;
  char *text;      /* RECALL/THINK/CLARIFY input payload, owned      */
  char *why;       /* short rationale, owned, may be NULL            */
  char *success;   /* declared success condition, owned, may be NULL */
  char *fallback;  /* declared fallback plan, owned, may be NULL     */
  char *call_ref;  /* CALL: tool ref, owned                          */
  char *call_cmd;  /* CALL: command, owned                           */
  char *call_args; /* CALL: args object text, owned                  */
  int   blob_n;    /* OPEN: 1-based handle number                    */
} asngn_step;

void asngn_step_free(asngn_step *st);
/* Parse one action-object line (defense in depth over the grammar). */
asngn_err asngn_step_parse(asngn_ctx *c, const char *line, asngn_step *out);

/* Merge the per-turn GBNF: asngn productions + astools call production
 * (grafted, renamed to avoid rule collisions) + concrete blob handles.
 * with_call/with_recall reflect sibling availability and turn options;
 * with_think supports the one-pass consecutive-thinking guard. */
asngn_err asngn_grammar_steps(asngn_ctx *c, bool with_call, bool with_recall,
                              bool with_think, size_t blobs_n,
                              const char *astools_gbnf, char **out);
asngn_err asngn_grammar_classify(char **out);
asngn_err asngn_grammar_judge(char **out);

/* ── safety (safety.c, redact.c, judge.c) ─────────────────────────────── */

/* Input gate: validates and sanitizes in place (may shrink); precise
 * reason via asngn_seterr on failure. */
asngn_err asngn_gate_input(asngn_ctx *c, char *text, size_t *len);

#define ASNGN_INPUT_MAX (64 * 1024)

/* Redaction scanner: masks likely secrets with «redacted:sha8».
 * Returns a new string in *out (owned) and the match count; *out is NULL
 * when nothing matched (caller keeps the original). */
asngn_err asngn_redact(const char *text, size_t len, char **out,
                       size_t *n_masked);

/* Judge: one grammar-constrained pass; score in [0,10]. */
asngn_err asngn_judge_run(asngn_ctx *c, asngn_session *s,
                          struct asngn_turn_state *t, const char *request,
                          const char *evidence, const char *answer,
                          int *out_score, char **out_critique,
                          size_t *aux_tokens);

/* Per-turn guard state lives in asngn_turn_state (loop.c). */

/* ── siblings (siblings.c) ────────────────────────────────────────────── */

typedef struct {
  char  ref[64];       /* tool id (with version when given)          */
  char  cmd[64];
  bool  read_only, destructive, idempotent, long_running;
  char  version[32];
} asngn_tool_note;

asngn_err asngn_siblings_open(asngn_ctx *c);
void      asngn_siblings_close(asngn_ctx *c);
/* Cached astools catalog + exported grammar; refreshed per turn. */
asngn_err asngn_siblings_catalog(asngn_ctx *c, char **out_text);
asngn_err asngn_siblings_grammar(asngn_ctx *c, char **out_gbnf);
/* Annotation lookup: parses (and caches) the tool manifest. */
asngn_err asngn_siblings_annotations(asngn_ctx *c, const char *ref,
                                     const char *cmd, asngn_tool_note *out);
asngn_err asngn_siblings_recall(asngn_ctx *c, const char *question,
                                char **out_block); /* rendered w/ cites  */
asngn_err asngn_siblings_project(asngn_ctx *c, const char *slug);
asngn_err asngn_siblings_project_sync(asngn_ctx *c, const char *want);
asngn_err asngn_siblings_readiness(asngn_ctx *c);
asngn_err asngn_siblings_workspace_sync(asngn_ctx *c, const char *root);

typedef enum {
  ASNGN_MEM_USER = 0,
  ASNGN_MEM_ASSISTANT,
  ASNGN_MEM_DECISION,
  ASNGN_MEM_TOOL_CALL,
  ASNGN_MEM_TOOL_RESULT,
  ASNGN_MEM_DIAGNOSTIC,
  ASNGN_MEM_CHECKPOINT,
  ASNGN_MEM_ARTIFACT
} asngn_memory_kind;

typedef struct {
  char id[37];
  unsigned long long sequence;
  asngn_time at;
  asngn_memory_kind kind;
  char *text;
  char object_ref[72];
  bool pinned;
} asngn_memory_event;

asngn_err asngn_siblings_event_append(asngn_ctx *c, const char *scope,
                                      asngn_memory_kind kind,
                                      const char *text,
                                      const char *object_ref, bool pinned,
                                      char out_id[37]);
asngn_err asngn_siblings_event_list(asngn_ctx *c, const char *scope,
                                    asngn_memory_event **out, size_t *out_n);
void asngn_siblings_events_free(asngn_memory_event *events, size_t n);
asngn_err asngn_siblings_event_pin(asngn_ctx *c, const char *scope,
                                   const char *event_id, bool pinned);
asngn_err asngn_siblings_object_put(asngn_ctx *c, const void *data,
                                    size_t len, char out_ref[72]);
asngn_err asngn_siblings_object_read(asngn_ctx *c, const char *ref,
                                     size_t offset, size_t max_bytes,
                                     void **out, size_t *out_len);
asngn_err asngn_siblings_checkpoint(asngn_ctx *c, const char *scope,
                                    const char *text, char out_id[37]);
asngn_err asngn_siblings_compact(asngn_ctx *c);
asngn_err asngn_siblings_context(asngn_ctx *c, const char *scope,
                                 const char *base_prompt, const char *query,
                                 size_t history_tokens,
                                 size_t checkpoint_tokens, int count_slot,
                                 char **out_system, char **out_context,
                                 size_t *out_system_tokens,
                                 size_t *out_context_tokens);

/* ── control loop (loop.c) ────────────────────────────────────────────── */

typedef struct {
  char   *text;   /* one working-zone item, owned                    */
  size_t  tokens; /* counted with the turn's tokenizer               */
} asngn_work_item;

/* Model-visible capabilities are phase-scoped.  Only ACTION sees the tool
 * catalog and may reach step_call; DRAFT creates an opaque tool payload;
 * RESPONSE is the only phase whose text may reach the user. */
typedef enum {
  ASNGN_PHASE_ACTION = 0,
  ASNGN_PHASE_DRAFT,
  ASNGN_PHASE_RESPONSE
} asngn_turn_phase;

typedef struct asngn_turn_state {
  asngn_session *s;
  char          *retrieval_query;
  char          *user_msg;      /* owned, gated                      */
  asngn_submit_opts opts;
  bool           continuation;  /* /more: continue the last answer   */
  bool           retry_up;      /* /retry: start one tier up         */
  volatile int   cancel;
  int64_t        deadline_mono; /* mono ms                           */
  char           span_root[37];
  size_t         log_before, turns_before;
  bool           tx_started, tx_committed, action_mutates;
  /* route */
  asngn_route_profile prof;
  asngn_route_evidence evidence; /* as weighed by the classifier     */
  asngn_detail   detail;        /* effective                         */
  int            gen_slot;      /* generator slot (escalations move) */
  int            escalations;
  char           cache_outcome[8];
  /* working zone */
  asngn_work_item *work;
  size_t         work_n, work_cap;
  /* catalog + grammar snapshots (owned) */
  char          *catalog;
  char          *astools_gbnf;
  asngn_turn_phase phase;
  /* step accounting + guards */
  int            steps, tool_calls, thinks_row, thinks_total;
  int            recalls_total;
  uint8_t        recall_keys[3][32]; /* unique RECALL questions this turn */
  size_t         recall_keys_n;
  uint8_t      (*call_keys)[32];
  size_t         call_keys_n, call_keys_cap;
  uint8_t        osc_a[32], osc_b[32];
  int            osc_cycles;
  int            repeat_calls; /* consecutive identical-call rejections */
  int            futile_row;   /* consecutive non-progressing steps     */
  bool           call_mute;    /* withhold CALL from the next decision
                                 * pass (set when a repeat was blocked) */
  bool           think_mute;   /* withhold THINK for one pass after the
                                * consecutive-thinking budget is consumed */
  bool           tools_used;
  bool           tool_ok_seen; /* at least one call succeeded this turn */
  bool           wrote_workspace; /* a non-read_only call succeeded     */
  bool           artifact_written; /* fs.write/edit content landed       */
  bool           verification_attempted; /* build/test/run after mutation */
  bool           verification_ok;  /* applicable verification succeeded  */
  bool           authorization_blocked; /* denied action became a notice   */
  bool           answer_nudged;  /* outcome gate already bounced ANSWER */
  char         **tools_list;    /* labels for the cache entry        */
  size_t         tools_list_n;
  bool           forced_answer; /* guard/step exhaustion             */
  /* ledger accumulation */
  asngn_ledger_entry led;
  /* result */
  char          *answer;        /* owned                             */
  bool           capped, clarify;
  /* streaming */
  asngn_token_fn token_cb; void *token_ud;
  asngn_stream_fn stream_cb; void *stream_ud;
  asngn_usage_mode usage_mode;
  asngn_security_profile security_profile;
} asngn_turn_state;

void      asngn_turn_state_free(asngn_turn_state *t);
/* Run one full turn on the agent worker; fills t->answer / t->led. */
asngn_err asngn_loop_run(asngn_ctx *c, asngn_turn_state *t);
asngn_err asngn_work_push(asngn_ctx *c, asngn_turn_state *t,
                          const char *text); /* counts + appends      */
void asngn_turn_stream_emit(asngn_turn_state *t, asngn_stream_kind kind,
                            const char *text);

/* ── tasks and workers (worker.c, api.c) ──────────────────────────────── */

struct asngn_task {
  asngn_ctx     *ctx;
  asngn_session *session;
  asngn_turn_state *turn;   /* owned                                 */
  os_mutex       mu;
  os_cond        cv;
  bool           done;
  asngn_err      verdict;
  asngn_turn_result result; /* moved out once by task_wait           */
  bool           result_taken;
  struct asngn_task *next;  /* agent-worker queue                    */
};

typedef struct {
  char id[64];          /* confirm_id (uuid)                         */
  char ref[64], cmd[64];
  int  decided;         /* 0 pending, 1 decided                      */
  int  allow, session_wide;
  os_mutex mu;
  os_cond  cv;
} asngn_confirm_slot;

asngn_err asngn_runtime_create(asngn_ctx *owner, asngn_ctx **out);
void asngn_runtime_free(asngn_ctx *c);
size_t asngn_scheduler_capacity(const asngn_ctx *c);
asngn_err asngn_workers_start(asngn_ctx *c);
void      asngn_workers_stop(asngn_ctx *c);
asngn_err asngn_worker_submit(asngn_ctx *c, asngn_task *t);
/* Background maintenance (warmup, sweeps, telemetry flush). */
void      asngn_background_kick(asngn_ctx *c);
asngn_err asngn_run_due_work(asngn_ctx *c);

struct xcdn_node *asngn_turn_node(const asngn_turn *t);
bool asngn_turn_parse(const struct xcdn_node *n, asngn_turn *out);
struct xcdn_node *asngn_ledger_node(const asngn_ledger_entry *e);
bool asngn_ledger_parse(const struct xcdn_node *n, asngn_ledger_entry *out);
asngn_err asngn_turn_journal(asngn_turn_state *t, const char *state,
                             const char *action);
asngn_err asngn_turn_commit(asngn_turn_state *t, const asngn_turn *answer);
asngn_err asngn_turn_recover(asngn_session *s);
asngn_err asngn_session_stage_turn(asngn_session *s, const asngn_turn *t);
/* Internal fault hook, per context. Return nonzero to fail the named boundary. */

asngn_err asngn_retrieval_query(asngn_session *s, asngn_turn_state *t, char **out);
asngn_err asngn_code_retrieve(asngn_ctx *c, asngn_turn_state *t);
void asngn_code_index_free(void *index);

/* ── context struct ───────────────────────────────────────────────────── */

struct asngn_ctx {
  int (*fault)(void *ud, const char *point);
  void *fault_ud;
  asngn_config  cfg;
  asngn_clock   clock;
  os_rwlock     lock;

  char         *root;          /* resolved engine root, owned        */
  char         *sessions_dir, *cache_dir, *tele_dir;
  asngn_workspace_info workspace;
  asngn_repo_stats repo_stats; /* refreshed with the fingerprint     */
  bool          allow_degraded;
  bool          session_workspaces; /* sessions/<slug>/workspace mode */

  /* eval-suite calibration (route.c; probed once, agent thread only) */
  struct {
    bool    probed, present;
    double  success;           /* task_success_rate in [0,1]         */
    int64_t tasks, guard_trips;
  } calib;

  /* error */
  char          errbuf[512];
  asngn_context_diagnostics context_diag;
  os_mutex      err_mu;

  /* logging */
  FILE         *log_fp;
  size_t        log_size;
  os_mutex      log_mu;
  asngn_log_fn  log_cb;  void *log_ud;

  /* telemetry */
  asngn_tele_ring ring;
  os_mutex      tele_mu;
  FILE         *tele_fp;
  size_t        tele_size;
  asngn_buf     tele_batch;    /* per-turn file batch                */
  asngn_event_fn event_cb; void *event_ud;

  /* models */
  asngn_model_slot models[ASNGN_MAX_POOL];
  size_t        models_n;
  int           role_slot[ASNGN_ROLE_COUNT];
  os_mutex      models_mu;
  asmodel_manager *shared_models;
  void *model_aux;

  /* siblings */
  struct asper_ctx   *asper;
  struct astools_ctx *astools;
  bool          asper_ok, astools_ok;
  char         *astools_workspace_active; /* canonical root, owned      */
  char         *astools_catalog;   /* cached, owned                  */
  char         *astools_grammar;   /* cached, owned                  */
  asngn_tool_note *notes;          /* annotation cache               */
  size_t        notes_n, notes_cap;
  os_mutex      sib_mu;

  /* caches */
  asngn_cache_entry *cache;
  size_t        cache_n, cache_cap;
  size_t        cache_touch_pending;
  asngn_toolcache_entry *toolcache;
  size_t        toolcache_n, toolcache_cap;
  os_rwlock     cache_mu;
  int           embed_dim;         /* discovered from embedder       */

  /* sessions (open handles) */
  asngn_session **sessions;
  size_t        sessions_n, sessions_cap;

  /* budgets / stats */
  int64_t       daily_spent;
  asngn_time    daily_day;         /* unix day of daily_spent        */
  asngn_stats   stats;

  /* confirmations */
  asngn_confirm_slot confirm;      /* one pending at a time          */

  /* stall watchdog: the background worker aborts a model call
   * whose token stream stops making progress. call_cancel is the flag
   * handed to the backend; the turn-level cancel also raises it. */
  volatile int  call_cancel;
  volatile int  call_active;
  volatile int64_t call_last_ms;
  volatile int64_t call_started_ms;
  asngn_turn_state *call_turn;   /* borrowed; agent worker only        */

  /* workers */
  os_thread     agent_thread, bg_thread;
  struct asngn_ctx *owner; /* isolated inference/tool lane -> coordinator */
  struct asngn_ctx *lanes[8];
  size_t lanes_n, q_n;
  asngn_task *active_task; /* guarded by owner's q_mu */
  char *lane_project;

  os_mutex      q_mu;
  os_cond       q_cv;
  asngn_task   *q_head, *q_tail;
  bool          stop;
  volatile int  bg_cancel;       /* raised at shutdown: abort warmup  */
  bool          bg_due_warm;     /* preload role models at open       */
  int64_t       last_sweep_ms;
  bool          no_threads;
};

/* Fake-injection open used by tests (mirrors asper_open_with). Fakes are
 * matched to pool entries by id; matched entries never load llama. */
asngn_err asngn_open_with(const asngn_open_params *p,
                          const asngn_model_iface *fakes, size_t fakes_n,
                          const char *const *fake_ids,
                          const asngn_clock *clk, asngn_ctx **out);

#endif /* ASNGN_INTERNAL_H */
