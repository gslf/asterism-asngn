/*
 * asngn.h — Asterism Engine ("asngn")
 *
 * An outcome-gated agentic coding engine for small local LLMs.
 * Public C API of libasngn.
 *
 * Contract notes:
 *   - All strings are UTF-8.
 *   - Every function returns asngn_err except destructors and accessors.
 *   - Every out-allocation is released with the matching asngn_free /
 *     asngn_turn_result_free / asngn_strings_free.
 *   - asngn_open / asngn_close are not thread-safe with respect to the same
 *     context; everything else is.
 *   - No global state: multiple contexts over different roots may coexist.
 *   - ASNGN_ERR_SIBLING carries the sibling's own error name and message
 *     through asngn_last_error.
 *
 * License: MIT — per aspera ad astra.
 */

#ifndef ASNGN_H
#define ASNGN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASNGN_VERSION_MAJOR 0
#define ASNGN_VERSION_MINOR 1
#define ASNGN_VERSION_PATCH 0

/* Returns "major.minor.patch". */
const char *asngn_version(void);

typedef struct asngn_ctx     asngn_ctx;
typedef struct asngn_session asngn_session;
typedef struct asngn_task    asngn_task;

typedef enum {
  ASNGN_OK = 0,
  ASNGN_ERR_IO,          /* filesystem failure                              */
  ASNGN_ERR_PARSE,       /* malformed xCDN / stream / cache                 */
  ASNGN_ERR_CONFIG,      /* invalid configuration value                     */
  ASNGN_ERR_MODEL,       /* model load / inference failure                  */
  ASNGN_ERR_NOT_FOUND,   /* unknown session / turn / entry                  */
  ASNGN_ERR_INVALID,     /* invalid argument or state                       */
  ASNGN_ERR_DENIED,      /* confirmation required or denied                 */
  ASNGN_ERR_TIMEOUT,     /* deadline exceeded                               */
  ASNGN_ERR_CANCELLED,   /* cancelled by the caller                         */
  ASNGN_ERR_BUSY,        /* a turn is already running / not done yet        */
  ASNGN_ERR_PROTOCOL,    /* malformed step / model protocol output          */
  ASNGN_ERR_CONTEXT,     /* prompt exceeds the model's global context budget */
  ASNGN_ERR_UNSUPPORTED, /* disabled feature or unsupported operation       */
  ASNGN_ERR_SIBLING,     /* asper / astools failure (see asngn_last_error)  */
  ASNGN_ERR_NOMEM        /* allocation failure                              */
} asngn_err;

/* Stable name of an error code, e.g. "ASNGN_ERR_MODEL". */
const char *asngn_err_name(asngn_err e);

typedef enum {
  ASNGN_DETAIL_AUTO = 0,
  ASNGN_DETAIL_TERSE,
  ASNGN_DETAIL_NORMAL,
  ASNGN_DETAIL_RICH
} asngn_detail;

/* Log levels for asngn_set_logger callbacks. */
enum {
  ASNGN_LOG_ERROR = 0,
  ASNGN_LOG_WARN  = 1,
  ASNGN_LOG_INFO  = 2,
  ASNGN_LOG_DEBUG = 3
};

typedef struct {
  const char *engine_root; /* override store dir; NULL discovers ~/asngn   */
  const char *config_path; /* optional config.xcdn; NULL = defaults        */
  const char *workspace_root; /* external override; NULL=config/session    */
  const char *project_id;     /* stable project id; NULL=derived           */
  const char *build_adapter;  /* cmake/npm/cargo/...; NULL=auto-detect      */
  int         allow_degraded; /* explicit opt-in to missing coding deps     */
} asngn_open_params;

#define ASNGN_WORKSPACE_PATH_MAX 1024
typedef struct {
  char canonical_root[ASNGN_WORKSPACE_PATH_MAX];
  char repository_root[ASNGN_WORKSPACE_PATH_MAX];
  char head[65];
  char branch[128];
  char project_id[65];
  char ignore_rules[256];
  char build_adapter[32];
  char fingerprint[65];
} asngn_workspace_info;

asngn_err asngn_workspace_get(asngn_ctx *c, asngn_workspace_info *out);

asngn_err   asngn_open(const asngn_open_params *p, asngn_ctx **out);
/* Flush, close siblings, join workers, release everything. NULL: no-op. */
void        asngn_close(asngn_ctx *c);
/* UTF-8 message for the most recent error on this context; ctx-owned. */
const char *asngn_last_error(const asngn_ctx *c);

/* Diagnostics for the most recent ASNGN_ERR_CONTEXT. `overhead` includes
 * chat-template tokens and zone headings/separators not attributable to a
 * payload. All values are tokens. */
typedef struct {
  size_t n_ctx, output_reserve, safety_margin, prompt_budget, prompt_total;
  size_t system, memory, catalog, summary, verbatim, working, overhead;
} asngn_context_diagnostics;
asngn_err asngn_last_context_diagnostics(
    asngn_ctx *c, asngn_context_diagnostics *out);

/* ---- sessions ---------------------------------------------------------- */

/* slug NULL creates a new session with a generated slug. */
asngn_err asngn_session_open (asngn_ctx *c, const char *slug,
                              asngn_session **out);
void      asngn_session_close(asngn_session *s);
/* Delete a session's directory (transcript, ledger, summary, blobs).
 * Fails with ASNGN_ERR_BUSY while that session is open. Irreversible. */
asngn_err asngn_session_delete(asngn_ctx *c, const char *slug);

/* Read a session's headline numbers from disk without opening it. */
typedef struct {
  size_t    turns;         /* committed answer turns (ledger entries) */
  long long spent_tokens;  /* lifetime prompt+gen total               */
  long long created_at;    /* unix seconds UTC                        */
  long long last_turn_at;  /* unix seconds UTC; 0 = no turns yet      */
  char      project[65];   /* active Asper project or ""              */
} asngn_session_peek_info;
asngn_err asngn_session_peek(asngn_ctx *c, const char *slug,
                             asngn_session_peek_info *out);
asngn_err asngn_session_list (asngn_ctx *c, char ***out_slugs,
                              size_t *out_n);
/* Slug of an open session; session-owned, valid until close. */
const char *asngn_session_slug(const asngn_session *s);
asngn_err asngn_session_workspace(asngn_session *s,
                                  asngn_workspace_info *out);
asngn_err asngn_session_pin    (asngn_session *s, size_t turn, int on);
asngn_err asngn_session_compact(asngn_session *s);    /* fold + compact    */

/* One transcript entry of an open session (history replay). */
typedef struct {
  size_t      turn;      /* ordinal, 1-based                            */
  char        role[10];  /* "user" | "assistant"                        */
  const char *text;      /* points into the array allocation            */
  long long   at;        /* unix seconds UTC                            */
  int         pinned, folded;
  char        tier[16];  /* assistant turns: generator tier, else ""    */
} asngn_transcript_entry;
/* Full transcript of an open session, oldest first. *out is one
 * allocation released with asngn_free; *out = NULL, *out_n = 0 when the
 * session has no turns yet. */
asngn_err asngn_session_transcript(asngn_session *s,
                                   asngn_transcript_entry **out,
                                   size_t *out_n);

/* ---- events (telemetry sink; the TUI is built on this) ----------------- */

/* One #asngn_event xCDN value per call. Called from engine threads; the
 * callback must be fast and must not call back into the API. */
typedef void (*asngn_event_fn)(const char *event_xcdn, void *ud);
void asngn_set_event_sink(asngn_ctx *c, asngn_event_fn fn, void *ud);

/* ---- turns ------------------------------------------------------------- */

/* Streaming answer tokens; called from the agent worker thread. */
typedef void (*asngn_token_fn)(const char *utf8, void *ud);

typedef struct {
  asngn_detail detail;      /* override; AUTO = controller decides         */
  uint32_t     deadline_ms; /* 0 = safety.turn_deadline                    */
  int          no_tools;    /* disable CALL for this turn                  */
  int          no_cache;    /* bypass the semantic cache for this turn     */
} asngn_submit_opts;

typedef struct {
  char    *answer;          /* UTF-8, asngn_free                           */
  size_t   turn;
  char     klass[12];       /* "simple" | "moderate" | "complex" | ...     */
  char     detail[8];       /* "terse" | "normal" | "rich"                 */
  char     tier[16];        /* generator tier actually used                */
  char     cache[8];        /* "hit" | "adapt" | "miss" | "off"            */
  int      capped;          /* answer hit its detail cap                   */
  int      clarify;         /* answer is a CLARIFY question                */
  size_t   tokens_prompt, tokens_gen, tokens_saved;
  uint64_t duration_ms;
} asngn_turn_result;

/* Asynchronous; token_fn may be NULL (no streaming). Turns are serialized
 * per session: ASNGN_ERR_BUSY while one is running. */
asngn_err asngn_submit(asngn_session *s, const char *text_utf8,
                       const asngn_submit_opts *opts,
                       asngn_token_fn token_fn, void *ud,
                       asngn_task **out);
/* ASNGN_ERR_BUSY: not done within timeout_ms. On completion fills *out
 * exactly once; later calls return the stored outcome. */
asngn_err asngn_task_wait  (asngn_task *t, uint32_t timeout_ms,
                            asngn_turn_result *out);
asngn_err asngn_task_cancel(asngn_task *t);
void      asngn_task_free  (asngn_task *t);

/* ---- confirmations ---------------------------------------------- */

/* An event of kind "confirm" carries data.confirm_id; the agent worker
 * blocks until asngn_confirm answers it (or the turn is cancelled).
 * session_wide != 0 adds tool.command to the session allowlist. */
asngn_err asngn_confirm(asngn_ctx *c, const char *confirm_id,
                        int allow, int session_wide);

/* ---- feedback and continuation ----------------------------------------- */

/* signal: +1 good, -1 poor, 0 clears. Recorded into the ledger. */
asngn_err asngn_feedback(asngn_session *s, size_t turn, int signal);
/* Continue the last capped answer. */
asngn_err asngn_more    (asngn_session *s, asngn_token_fn fn, void *ud,
                         asngn_task **out);

/* ---- caches ------------------------------------------------------------ */

/* scope: "session" | "global" | NULL (= both). */
asngn_err asngn_cache_clear(asngn_ctx *c, const char *scope);

/* ---- introspection (TUI panes, asngn-mcp) ------------------------------ */

typedef struct {
  size_t   turns;                 /* committed ledger entries            */
  size_t   tokens_prompt, tokens_gen, tokens_saved;
  size_t   tokens_memory;         /* memory-zone (Asper) share of prompt */
  size_t   cache_hits, cache_adapts, cache_misses;
  size_t   clarifies, capped;
  size_t   escalations;
  double   qpt_rolling;           /* window of 20                        */
  uint64_t world_epoch;
  long long spent_tokens;         /* lifetime prompt+gen total           */
} asngn_session_stats;
asngn_err asngn_session_get_stats(asngn_session *s,
                                  asngn_session_stats *out);

typedef struct {
  int       asper_ok, astools_ok;
  size_t    mem_identity, mem_context, mem_project, mem_deprecated;
  long long mem_last_cycle_at;    /* unix seconds UTC; 0 = never         */
  char      project[65];          /* active Asper project or ""          */
  size_t    tools_total, tools_enabled, tools_unavailable;
  size_t    tool_invocations, tool_ok, tool_failed, tool_denied;
} asngn_sibling_stats;
asngn_err asngn_get_sibling_stats(asngn_ctx *c, asngn_sibling_stats *out);

/* One resolved tool of the astools registry. */
typedef struct {
  char ref[64];   /* tool id                                          */
  int  enabled;   /* host toggle AND pinning gate                     */
  int  available; /* runnable on this platform                        */
} asngn_tool_info;
/* Every resolved tool, sorted by id; *out is one allocation released
 * with asngn_free. *out = NULL, *out_n = 0 when astools is disabled. */
asngn_err asngn_tool_list(asngn_ctx *c, asngn_tool_info **out,
                          size_t *out_n);
/* Enable or disable a tool ("fs" or "fs@1.2.0") for CALL steps. Under
 * pinning "enforce" an enable only takes effect once the lockfile
 * verifies; re-read the state with asngn_tool_list. */
asngn_err asngn_tool_enable(asngn_ctx *c, const char *ref, int on);

/* Direct Asper recall, bypassing the loop (/memory). Answer rendered
 * with citations; asngn_free. */
asngn_err asngn_recall(asngn_ctx *c, const char *question, char **out);

/* Select the Asper project for this session; NULL deselects. The
 * semantic cache is additionally partitioned by the project slug. */
asngn_err asngn_session_project(asngn_session *s, const char *slug);
asngn_err asngn_project_list(asngn_ctx *c, char ***out_slugs,
                             size_t *out_n);

/* Toggle the model-visible redaction scan for this session. */
asngn_err asngn_session_redact(asngn_session *s, int on);

/* Re-run the last turn one tier up, bypassing the cache. */
asngn_err asngn_retry(asngn_session *s, asngn_token_fn fn, void *ud,
                      asngn_task **out);

/* Session report: writes report.xcdn + report.txt next to the
 * session and returns the plain-text rendering (asngn_free). */
asngn_err asngn_session_export(asngn_session *s, char **out_text);

/* Most recent n telemetry events as xCDN lines, oldest first;
 * asngn_strings_free. */
asngn_err asngn_telemetry_tail(asngn_ctx *c, size_t n, char ***out,
                               size_t *out_n);

/* ---- stats / logging / lifecycle --------------------------------------- */

/* The configured model pool: which weights file each pool id maps to,
 * the engine roles it serves, and whether it is resident right now. */
typedef struct {
  char id[32];     /* pool id ("nano", "light", ...)                  */
  char file[96];   /* basename of the resolved weights file           */
  char roles[112]; /* space-joined roles served ("router judge ...")  */
  int  resident;   /* loaded in memory at this moment                 */
  int  embedding;  /* embedding model (never generates)               */
} asngn_model_info;
/* Fills up to cap entries; *out_n = configured pool size. */
asngn_err asngn_get_models(asngn_ctx *c, asngn_model_info *out,
                           size_t cap, size_t *out_n);

typedef struct {
  size_t turns, cache_hits, cache_adapts, cache_misses;
  size_t tool_calls, tool_cache_hits, escalations, guard_trips;
  size_t tokens_prompt, tokens_gen, tokens_saved, summary_debt;
  size_t folds;
  double qpt_rolling;             /* rolling QpT over the last 20 turns   */
  long long last_turn_at;         /* unix seconds UTC; 0 = never          */
  long long last_fold_at;
  long long last_sweep_at;
} asngn_stats;
asngn_err asngn_get_stats(asngn_ctx *c, asngn_stats *out);

/* Callback sink, independent of the file log. level: ASNGN_LOG_*. The
 * default callback writes WARN and ERROR to stderr. fn NULL disables. */
typedef void (*asngn_log_fn)(int level, const char *msg, void *ud);
void asngn_set_logger(asngn_ctx *c, asngn_log_fn fn, void *ud);

/* ASNGN_NO_THREADS builds: run due background work on the caller.
 * Threaded builds: no-op returning ASNGN_OK. */
asngn_err asngn_tick(asngn_ctx *c);

/* ---- memory management -------------------------------------------------- */

void asngn_turn_result_free(asngn_turn_result *r);
void asngn_strings_free(char **s, size_t n);
void asngn_free(void *p);          /* strings/arrays from the API          */

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_H */
