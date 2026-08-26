/*
 * main.c — asngn-mcp: MCP stdio server over libasngn.
 *
 * JSON-RPC 2.0, newline-delimited compact JSON on stdin/stdout, protocol
 * revision 2025-06-18.
 *
 * Wire conventions:
 *   - One request per line; responses are single-line compact JSON,
 *     flushed after each write.
 *   - Batch arrays are not supported (MCP does not use them): -32600.
 *   - Requests without an "id" are notifications: processed, no response.
 *     Methods under "notifications/" are ignored only when the id is
 *     absent; with an id they dispatch normally (unknown => -32601).
 *   - Tool results: {content:[{type:"text",text:<compact JSON payload>}],
 *     isError:bool}. Engine failures set isError:true with payload
 *     {"error":"ASNGN_ERR_...","message":"..."}; turn-level notices
 *     (capped, clarify) travel inside the agent_ask result payload.
 *   - stderr carries nothing but a startup failure message; the library's
 *     default stderr log callback is disabled at startup.
 *
 * Trust model: the MCP client is a local process spawned by the
 * user over stdio; no network listener exists.
 *
 * The server uses the PUBLIC libasngn API (asngn.h) with exactly one
 * sanctioned internal touch: after asngn_open it applies the MCP
 * confirmation default ("deny" over MCP, so an MCP client
 * cannot grant the agent anything) by copying cfg.mcp_autoconfirm over
 * cfg.autoconfirm, which has no public setter. That is the only reason
 * asngn_internal.h is included here.
 *
 * MIT License — per aspera ad astra.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asngn.h"
#include "asngn_internal.h" /* the one sanctioned internal touch; see above */
#include "json.h"

/* ═══════════════════════ usage / help ═══════════════════════ */

static const char USAGE[] =
    "usage: asngn-mcp --root <dir> [--config <file>] "
    "[--workspace <dir>] [--allow-degraded]\n"
    "       asngn-mcp --help | --version\n";

static const char HELP[] =
    "asngn-mcp - MCP stdio server for the Asterism Engine (asngn)\n"
    "\n"
    "usage: asngn-mcp --root <dir> [--config <file>] "
    "[--workspace <dir>] [--allow-degraded]\n"
    "       asngn-mcp --help | --version\n"
    "\n"
    "options:\n"
    "  --root <dir>     engine root directory (created if missing);"
    " required\n"
    "  --config <file>  optional xCDN configuration file (#asngn_config)\n"
    "  --workspace <dir> canonical coding workspace (overrides config)\n"
    "  --allow-degraded  explicitly allow missing coding dependencies\n"
    "  --help           print this help and exit\n"
    "  --version        print the version and exit\n"
    "\n"
    "The server speaks JSON-RPC 2.0 over stdio (MCP protocol revision\n"
    "2025-06-18), one compact JSON message per line. Tool confirmations\n"
    "follow mcp.autoconfirm (default \"deny\"): an MCP client cannot\n"
    "grant the agent anything.\n";

/* Emitted verbatim when even the error response cannot be allocated. */
static const char OOM_RESPONSE[] =
    "{\"jsonrpc\":\"2.0\",\"id\":null,"
    "\"error\":{\"code\":-32603,\"message\":\"out of memory\"}}";

#define MCP_LEGACY_VERSION "2025-06-18"
#define MCP_MODERN_VERSION "2026-07-28"
static int mcp_modern_response;

/* ═══════════════════════ server state ═══════════════════════ */

/* Small registry of sessions this server has opened, keyed by slug.
 * Sessions are opened on demand and stay open until shutdown. */
#define MAX_SESSIONS 32

typedef struct {
  asngn_ctx *ctx;
  asngn_session *sessions[MAX_SESSIONS];
  size_t sessions_n;
} server_state;

/* ═══════════════════════ small helpers ═══════════════════════ */

static void emit_line(const char *s) {
  fputs(s, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

/* Serialize + emit + free resp; OOM falls back to the static response. */
static void send_value(jx_value *resp) {
  char *s = jx_write(resp, 0);
  jx_free(resp);
  if (!s) {
    emit_line(OOM_RESPONSE);
    return;
  }
  emit_line(s);
  free(s);
}

/* want == 0: notification — consume the owned arguments, emit nothing. */
static int decorate_modern_result(jx_value *result) {
  jx_value *meta, *info;
  int ok;
  if (!result || jx_typeof(result) != JX_OBJECT) return 0;
  meta = jx_object();
  info = jx_object();
  ok = meta != NULL && info != NULL;
  ok &= jx_object_set(info, "name", jx_string("asngn-mcp")) == 0;
  ok &= jx_object_set(info, "version", jx_string(asngn_version())) == 0;
  ok &= jx_object_set(meta, "io.modelcontextprotocol/serverInfo", info) == 0;
  ok &= jx_object_set(result, "resultType", jx_string("complete")) == 0;
  ok &= jx_object_set(result, "_meta", meta) == 0;
  return ok;
}

static void send_result(int want, jx_value *id, jx_value *result) {
  jx_value *resp;
  int ok;
  if (!want) {
    jx_free(id);
    jx_free(result);
    return;
  }
  if (mcp_modern_response && !decorate_modern_result(result)) {
    jx_free(id);
    jx_free(result);
    emit_line(OOM_RESPONSE);
    return;
  }
  resp = jx_object();
  ok = (resp != NULL);
  ok &= jx_object_set(resp, "jsonrpc", jx_string("2.0")) == 0;
  ok &= jx_object_set(resp, "id", id ? id : jx_null()) == 0;
  ok &= jx_object_set(resp, "result", result) == 0;
  if (!ok) {
    jx_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

static void send_error(int want, jx_value *id, int code, const char *message,
                       const char *asngn_name) {
  jx_value *err, *resp;
  int ok;
  if (!want) {
    jx_free(id);
    return;
  }
  err = jx_object();
  ok = (err != NULL);
  ok &= jx_object_set(err, "code", jx_int(code)) == 0;
  ok &= jx_object_set(err, "message", jx_string(message)) == 0;
  if (asngn_name) {
    jx_value *data = jx_object();
    ok &= jx_object_set(data, "asngn", jx_string(asngn_name)) == 0;
    ok &= jx_object_set(err, "data", data) == 0;
  }
  resp = jx_object();
  ok &= (resp != NULL);
  ok &= jx_object_set(resp, "jsonrpc", jx_string("2.0")) == 0;
  ok &= jx_object_set(resp, "id", id ? id : jx_null()) == 0;
  ok &= jx_object_set(resp, "error", err) == 0;
  if (!ok) {
    jx_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

/* Wrap a tool payload in the MCP content envelope and send it. */
static void send_tool_result(int want, jx_value *id, jx_value *payload,
                             int is_error) {
  char *txt;
  jx_value *item, *content, *res;
  int ok;
  if (!want) {
    jx_free(id);
    jx_free(payload);
    return;
  }
  txt = jx_write(payload, 0);
  jx_free(payload);
  if (!txt) {
    jx_free(id);
    emit_line(OOM_RESPONSE);
    return;
  }
  item = jx_object();
  ok = (item != NULL);
  ok &= jx_object_set(item, "type", jx_string("text")) == 0;
  ok &= jx_object_set(item, "text", jx_string(txt)) == 0;
  free(txt);
  content = jx_array();
  ok &= jx_array_push(content, item) == 0;
  res = jx_object();
  ok &= jx_object_set(res, "content", content) == 0;
  ok &= jx_object_set(res, "isError", jx_bool(is_error)) == 0;
  if (!ok) {
    jx_free(res);
    jx_free(id);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_result(1, id, res);
}

/* ═══════════════════════ argument extraction ═══════════════════════ */

/* 1 = present and well-typed, 0 = absent, -1 = wrong type. A string with
 * an embedded NUL is wrong-typed (it would be silently truncated by C
 * string consumers); *msg then carries the specific reason. args may be
 * NULL ("arguments" omitted). */
static int arg_str(const jx_value *args, const char *key, const char **out,
                   const char **msg) {
  const jx_value *v = jx_object_get(args, key);
  if (!v) return 0;
  if (jx_typeof(v) != JX_STRING) return -1;
  if (jx_string_length(v) != strlen(jx_string_value(v))) {
    *msg = "string must not contain NUL";
    return -1;
  }
  *out = jx_string_value(v);
  return 1;
}

static int arg_int(const jx_value *args, const char *key, long long *out) {
  const jx_value *v = jx_object_get(args, key);
  double d;
  if (!v) return 0;
  if (jx_typeof(v) != JX_NUMBER) return -1;
  if (jx_is_int(v)) {
    *out = jx_int_value(v);
    return 1;
  }
  /* Schema-valid integral spellings (5.0, 1e2): accept when the double
   * is finite, integral, and exactly representable as long long. */
  d = jx_double_value(v);
  if (!isfinite(d) || d != floor(d) || d < -0x1p63 || d >= 0x1p63)
    return -1;
  *out = (long long)d;
  return 1;
}

static int arg_bool(const jx_value *args, const char *key, int *out) {
  const jx_value *v = jx_object_get(args, key);
  if (!v) return 0;
  if (jx_typeof(v) != JX_BOOL) return -1;
  *out = jx_bool_value(v);
  return 1;
}

/* ═══════════════════════ payload builders ═══════════════════════ */

/* Doubles from engine counters; non-finite degrades to null, never to a
 * writer failure. */
static jx_value *jx_finite(double d) {
  return isfinite(d) ? jx_double(d) : jx_null();
}

static jx_value *ok_payload(void) {
  jx_value *o = jx_object();
  if (jx_object_set(o, "ok", jx_bool(1)) != 0) {
    jx_free(o);
    return NULL;
  }
  return o;
}

/* ═══════════════════════ tools ═══════════════════════ */

/* TOOL_OK / TOOL_FAIL carry a payload in *out (isError false / true);
 * TOOL_PARAM maps to JSON-RPC -32602, TOOL_OOM to -32603. */
enum { TOOL_OK = 0, TOOL_FAIL, TOOL_PARAM, TOOL_OOM };

/* First message wins: arg_str may already have set a more specific one
 * (*msg is NULL on tool entry). */
#define BADP(m)                                                              \
  do {                                                                       \
    if (!*msg) *msg = (m);                                                   \
    return TOOL_PARAM;                                                       \
  } while (0)

typedef int (*tool_fn)(server_state *st, const jx_value *args,
                       jx_value **out, const char **msg);

/* {"error": <name>, "message": <text>} with isError:true. */
static int fail_payload(jx_value **out, const char *name,
                        const char *message) {
  jx_value *o = jx_object();
  jx_value *m;
  int ok = (o != NULL);
  ok &= jx_object_set(o, "error", jx_string(name)) == 0;
  m = jx_string(message ? message : "");
  if (!m) m = jx_string(""); /* message not valid UTF-8: drop it */
  ok &= jx_object_set(o, "message", m) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_FAIL;
}

static int engine_fail(server_state *st, asngn_err e, jx_value **out) {
  return fail_payload(out, asngn_err_name(e), asngn_last_error(st->ctx));
}

/* Resolve a slug (NULL = "main") to an open session: reuse a registered
 * handle or open on demand and register it. */
static int state_session(server_state *st, const char *slug,
                         asngn_session **out_s, jx_value **out) {
  asngn_session *s = NULL;
  asngn_err e;
  size_t i;
  if (!slug) slug = "main";
  for (i = 0; i < st->sessions_n; i++) {
    const char *have = asngn_session_slug(st->sessions[i]);
    if (have && strcmp(have, slug) == 0) {
      *out_s = st->sessions[i];
      return TOOL_OK;
    }
  }
  if (st->sessions_n >= MAX_SESSIONS)
    return fail_payload(out, "ASNGN_ERR_UNSUPPORTED",
                        "session registry full");
  e = asngn_session_open(st->ctx, slug, &s);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  st->sessions[st->sessions_n++] = s;
  *out_s = s;
  return TOOL_OK;
}

static int tool_agent_ask(server_state *st, const jx_value *args,
                          jx_value **out, const char **msg) {
  const char *message = NULL, *session = NULL, *detail = NULL;
  int no_tools = 0, rc, ok;
  asngn_session *s = NULL;
  asngn_submit_opts opts;
  asngn_task *t = NULL;
  asngn_turn_result r;
  asngn_err e;
  jx_value *o;

  if (arg_str(args, "message", &message, msg) != 1)
    BADP("agent_ask: \"message\" must be a string");
  if (arg_str(args, "session", &session, msg) < 0)
    BADP("agent_ask: \"session\" must be a string");
  memset(&opts, 0, sizeof opts);
  rc = arg_str(args, "detail", &detail, msg);
  if (rc < 0)
    BADP("agent_ask: \"detail\" must be one of terse|normal|rich");
  if (rc == 1) {
    if (strcmp(detail, "terse") == 0) opts.detail = ASNGN_DETAIL_TERSE;
    else if (strcmp(detail, "normal") == 0) opts.detail = ASNGN_DETAIL_NORMAL;
    else if (strcmp(detail, "rich") == 0) opts.detail = ASNGN_DETAIL_RICH;
    else BADP("agent_ask: \"detail\" must be one of terse|normal|rich");
  }
  if (arg_bool(args, "no_tools", &no_tools) < 0)
    BADP("agent_ask: \"no_tools\" must be a boolean");
  opts.no_tools = no_tools;

  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;

  /* One synchronous turn: no streaming callback; timeout 0 waits until
   * completion — the engine's own turn deadline bounds the wait. */
  e = asngn_submit(s, message, &opts, NULL, NULL, &t);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  memset(&r, 0, sizeof r);
  e = asngn_task_wait(t, 0, &r);
  asngn_task_free(t);
  if (e != ASNGN_OK) {
    asngn_turn_result_free(&r);
    return engine_fail(st, e, out);
  }

  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "answer", jx_string(r.answer ? r.answer : "")) == 0;
  ok &= jx_object_set(o, "turn", jx_int((long long)r.turn)) == 0;
  ok &= jx_object_set(o, "class", jx_string(r.klass)) == 0;
  ok &= jx_object_set(o, "detail", jx_string(r.detail)) == 0;
  ok &= jx_object_set(o, "tier", jx_string(r.tier)) == 0;
  ok &= jx_object_set(o, "cache", jx_string(r.cache)) == 0;
  ok &= jx_object_set(o, "capped", jx_bool(r.capped)) == 0;
  ok &= jx_object_set(o, "clarify", jx_bool(r.clarify)) == 0;
  ok &= jx_object_set(o, "tokens_prompt",
                      jx_int((long long)r.tokens_prompt)) == 0;
  ok &= jx_object_set(o, "tokens_gen",
                      jx_int((long long)r.tokens_gen)) == 0;
  ok &= jx_object_set(o, "tokens_saved",
                      jx_int((long long)r.tokens_saved)) == 0;
  ok &= jx_object_set(o, "duration_ms",
                      jx_int((long long)r.duration_ms)) == 0;
  asngn_turn_result_free(&r);
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_agent_feedback(server_state *st, const jx_value *args,
                               jx_value **out, const char **msg) {
  const char *session = NULL;
  long long turn = 0, signal = 0;
  asngn_session *s = NULL;
  asngn_err e;
  int rc;

  if (arg_str(args, "session", &session, msg) != 1)
    BADP("agent_feedback: \"session\" must be a string");
  if (arg_int(args, "turn", &turn) != 1 || turn < 0)
    BADP("agent_feedback: \"turn\" must be a non-negative integer");
  if (arg_int(args, "signal", &signal) != 1 || signal < -1 || signal > 1)
    BADP("agent_feedback: \"signal\" must be -1, 0 or 1");

  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;
  e = asngn_feedback(s, (size_t)turn, (int)signal);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  *out = ok_payload();
  return *out ? TOOL_OK : TOOL_OOM;
}

static int tool_session_list(server_state *st, const jx_value *args,
                             jx_value **out, const char **msg) {
  char **slugs = NULL;
  size_t n = 0, i;
  asngn_err e;
  jx_value *arr, *o;
  int ok;
  (void)args;
  (void)msg;

  e = asngn_session_list(st->ctx, &slugs, &n);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  arr = jx_array();
  ok = (arr != NULL);
  for (i = 0; i < n; i++) {
    if (jx_array_push(arr, jx_string(slugs[i])) != 0) ok = 0;
  }
  asngn_strings_free(slugs, n);
  o = jx_object();
  ok &= jx_object_set(o, "sessions", arr) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_delete(server_state *st, const jx_value *args,
                               jx_value **out, const char **msg) {
  const char *slug = NULL;
  size_t i;
  asngn_err e;
  jx_value *o;

  if (arg_str(args, "slug", &slug, msg) < 0 || slug == NULL)
    BADP("session_delete: \"slug\" is required");
  /* a session this server holds open must be closed first */
  for (i = 0; i < st->sessions_n; i++) {
    if (strcmp(asngn_session_slug(st->sessions[i]), slug) == 0) {
      asngn_session_close(st->sessions[i]);
      memmove(&st->sessions[i], &st->sessions[i + 1],
              (st->sessions_n - i - 1) * sizeof st->sessions[0]);
      st->sessions_n--;
      break;
    }
  }
  e = asngn_session_delete(st->ctx, slug);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = jx_object();
  if (jx_object_set(o, "deleted", jx_string(slug)) != 0) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_new(server_state *st, const jx_value *args,
                            jx_value **out, const char **msg) {
  const char *slug = NULL;
  asngn_session *s = NULL;
  asngn_err e;
  jx_value *o;
  int rc;

  if (arg_str(args, "slug", &slug, msg) < 0)
    BADP("session_new: \"slug\" must be a string");
  if (slug) {
    rc = state_session(st, slug, &s, out);
    if (rc != TOOL_OK) return rc;
  } else {
    if (st->sessions_n >= MAX_SESSIONS)
      return fail_payload(out, "ASNGN_ERR_UNSUPPORTED",
                          "session registry full");
    e = asngn_session_open(st->ctx, NULL, &s);
    if (e != ASNGN_OK) return engine_fail(st, e, out);
    st->sessions[st->sessions_n++] = s;
  }
  o = jx_object();
  if (jx_object_set(o, "slug", jx_string(asngn_session_slug(s))) != 0) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_stats(server_state *st, const jx_value *args,
                              jx_value **out, const char **msg) {
  const char *session = NULL;
  asngn_session *s = NULL;
  asngn_session_stats stt;
  asngn_err e;
  jx_value *o;
  int rc, ok;

  if (arg_str(args, "session", &session, msg) != 1)
    BADP("session_stats: \"session\" must be a string");
  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;

  memset(&stt, 0, sizeof stt);
  e = asngn_session_get_stats(s, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);

  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "turns", jx_int((long long)stt.turns)) == 0;
  ok &= jx_object_set(o, "tokens_prompt",
                      jx_int((long long)stt.tokens_prompt)) == 0;
  ok &= jx_object_set(o, "tokens_gen",
                      jx_int((long long)stt.tokens_gen)) == 0;
  ok &= jx_object_set(o, "tokens_saved",
                      jx_int((long long)stt.tokens_saved)) == 0;
  ok &= jx_object_set(o, "cache_hits",
                      jx_int((long long)stt.cache_hits)) == 0;
  ok &= jx_object_set(o, "cache_adapts",
                      jx_int((long long)stt.cache_adapts)) == 0;
  ok &= jx_object_set(o, "cache_misses",
                      jx_int((long long)stt.cache_misses)) == 0;
  ok &= jx_object_set(o, "clarifies",
                      jx_int((long long)stt.clarifies)) == 0;
  ok &= jx_object_set(o, "capped", jx_int((long long)stt.capped)) == 0;
  ok &= jx_object_set(o, "escalations",
                      jx_int((long long)stt.escalations)) == 0;
  ok &= jx_object_set(o, "qpt_rolling", jx_finite(stt.qpt_rolling)) == 0;
  ok &= jx_object_set(o, "world_epoch",
                      jx_int((long long)stt.world_epoch)) == 0;
  ok &= jx_object_set(o, "spent_tokens", jx_int(stt.spent_tokens)) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_project_select(server_state *st, const jx_value *args,
                               jx_value **out, const char **msg) {
  const jx_value *sv = jx_object_get(args, "slug");
  const char *slug = NULL, *session = NULL;
  asngn_session *s = NULL;
  asngn_err e;
  jx_value *o;
  int rc, ok;

  if (!sv)
    BADP("project_select: \"slug\" is required (string or null)");
  if (jx_typeof(sv) == JX_STRING) {
    if (jx_string_length(sv) != strlen(jx_string_value(sv)))
      BADP("string must not contain NUL");
    slug = jx_string_value(sv);
  } else if (jx_typeof(sv) != JX_NULL) {
    BADP("project_select: \"slug\" must be a string or null");
  }
  if (arg_str(args, "session", &session, msg) < 0)
    BADP("project_select: \"session\" must be a string");

  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;
  e = asngn_session_project(s, slug);
  if (e != ASNGN_OK) return engine_fail(st, e, out);

  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "ok", jx_bool(1)) == 0;
  ok &= jx_object_set(o, "active", slug ? jx_string(slug) : jx_null()) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_cache_stats(server_state *st, const jx_value *args,
                            jx_value **out, const char **msg) {
  asngn_stats stt;
  asngn_err e;
  jx_value *o;
  int ok;
  (void)args;
  (void)msg;

  memset(&stt, 0, sizeof stt);
  e = asngn_get_stats(st->ctx, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "hits", jx_int((long long)stt.cache_hits)) == 0;
  ok &= jx_object_set(o, "adapts",
                      jx_int((long long)stt.cache_adapts)) == 0;
  ok &= jx_object_set(o, "misses",
                      jx_int((long long)stt.cache_misses)) == 0;
  ok &= jx_object_set(o, "tool_cache_hits",
                      jx_int((long long)stt.tool_cache_hits)) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_cache_clear(server_state *st, const jx_value *args,
                            jx_value **out, const char **msg) {
  const char *scope = NULL;
  asngn_err e;
  int rc;

  rc = arg_str(args, "scope", &scope, msg);
  if (rc < 0 || (rc == 1 && strcmp(scope, "session") != 0 &&
                 strcmp(scope, "global") != 0))
    BADP("cache_clear: \"scope\" must be \"session\" or \"global\"");
  e = asngn_cache_clear(st->ctx, scope); /* NULL = both */
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  *out = ok_payload();
  return *out ? TOOL_OK : TOOL_OOM;
}

static int tool_telemetry_tail(server_state *st, const jx_value *args,
                               jx_value **out, const char **msg) {
  long long n = 50; /* default when absent */
  char **lines = NULL;
  size_t ln = 0, i;
  asngn_err e;
  jx_value *arr, *o;
  int rc, ok;

  rc = arg_int(args, "n", &n);
  if (rc < 0 || n < 0)
    BADP("telemetry_tail: \"n\" must be a non-negative integer");

  e = asngn_telemetry_tail(st->ctx, (size_t)n, &lines, &ln);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  arr = jx_array();
  ok = (arr != NULL);
  for (i = 0; i < ln; i++) {
    if (jx_array_push(arr, jx_string(lines[i])) != 0) ok = 0;
  }
  asngn_strings_free(lines, ln);
  o = jx_object();
  ok &= jx_object_set(o, "events", arr) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_engine_stats(server_state *st, const jx_value *args,
                             jx_value **out, const char **msg) {
  asngn_stats stt;
  asngn_err e;
  jx_value *o;
  int ok;
  (void)args;
  (void)msg;

  memset(&stt, 0, sizeof stt);
  e = asngn_get_stats(st->ctx, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = jx_object();
  ok = (o != NULL);
  ok &= jx_object_set(o, "turns", jx_int((long long)stt.turns)) == 0;
  ok &= jx_object_set(o, "cache_hits",
                      jx_int((long long)stt.cache_hits)) == 0;
  ok &= jx_object_set(o, "cache_adapts",
                      jx_int((long long)stt.cache_adapts)) == 0;
  ok &= jx_object_set(o, "cache_misses",
                      jx_int((long long)stt.cache_misses)) == 0;
  ok &= jx_object_set(o, "tool_calls",
                      jx_int((long long)stt.tool_calls)) == 0;
  ok &= jx_object_set(o, "tool_cache_hits",
                      jx_int((long long)stt.tool_cache_hits)) == 0;
  ok &= jx_object_set(o, "escalations",
                      jx_int((long long)stt.escalations)) == 0;
  ok &= jx_object_set(o, "guard_trips",
                      jx_int((long long)stt.guard_trips)) == 0;
  ok &= jx_object_set(o, "tokens_prompt",
                      jx_int((long long)stt.tokens_prompt)) == 0;
  ok &= jx_object_set(o, "tokens_gen",
                      jx_int((long long)stt.tokens_gen)) == 0;
  ok &= jx_object_set(o, "tokens_saved",
                      jx_int((long long)stt.tokens_saved)) == 0;
  ok &= jx_object_set(o, "summary_debt",
                      jx_int((long long)stt.summary_debt)) == 0;
  ok &= jx_object_set(o, "folds", jx_int((long long)stt.folds)) == 0;
  ok &= jx_object_set(o, "qpt_rolling", jx_finite(stt.qpt_rolling)) == 0;
  ok &= jx_object_set(o, "last_turn_at", jx_int(stt.last_turn_at)) == 0;
  ok &= jx_object_set(o, "last_fold_at", jx_int(stt.last_fold_at)) == 0;
  ok &= jx_object_set(o, "last_sweep_at", jx_int(stt.last_sweep_at)) == 0;
  if (!ok) {
    jx_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

/* ═══════════════════════ tool table ════════════════════════════ */

static int tool_workspace_info(server_state *st, const jx_value *args,
                               jx_value **out, const char **msg) {
  asngn_workspace_info w;
  jx_value *o;
  int ok;
  asngn_err e;
  (void)args; (void)msg;
  memset(&w, 0, sizeof w);
  e = asngn_workspace_get(st->ctx, &w);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = jx_object(); ok = o != NULL;
  ok &= jx_object_set(o, "canonical_root", jx_string(w.canonical_root)) == 0;
  ok &= jx_object_set(o, "repository_root", jx_string(w.repository_root)) == 0;
  ok &= jx_object_set(o, "head", jx_string(w.head)) == 0;
  ok &= jx_object_set(o, "branch", jx_string(w.branch)) == 0;
  ok &= jx_object_set(o, "project_id", jx_string(w.project_id)) == 0;
  ok &= jx_object_set(o, "ignore_rules", jx_string(w.ignore_rules)) == 0;
  ok &= jx_object_set(o, "build_adapter", jx_string(w.build_adapter)) == 0;
  ok &= jx_object_set(o, "fingerprint", jx_string(w.fingerprint)) == 0;
  if (!ok) { jx_free(o); return TOOL_OOM; }
  *out = o; return TOOL_OK;
}

typedef struct {
  const char *name;
  const char *desc;
  const char *schema; /* JSON Schema literal, parsed on tools/list */
  tool_fn fn;
} tool_def;

static const tool_def TOOLS[] = {
    {"agent_ask",
     "Run one full agent turn; returns the answer plus the route summary "
     "and token counts. Blocking, bounded by the engine turn deadline.",
     "{\"type\":\"object\",\"properties\":{"
     "\"message\":{\"type\":\"string\",\"description\":\"User message "
     "for this turn\"},"
     "\"session\":{\"type\":\"string\",\"description\":\"Session slug "
     "(default \\\"main\\\"; opened on demand)\"},"
     "\"detail\":{\"type\":\"string\",\"enum\":[\"terse\",\"normal\","
     "\"rich\"],\"description\":\"Answer detail override; absent lets "
     "the engine decide\"},"
     "\"no_tools\":{\"type\":\"boolean\",\"description\":\"Disable tool "
     "calls for this turn (default false)\"}},"
     "\"required\":[\"message\"]}",
     tool_agent_ask},
    {"agent_feedback",
     "Record +1 / -1 quality feedback for a turn (0 clears it).",
     "{\"type\":\"object\",\"properties\":{"
     "\"session\":{\"type\":\"string\",\"description\":\"Session "
     "slug\"},"
     "\"turn\":{\"type\":\"integer\",\"minimum\":0,\"description\":"
     "\"Turn number the feedback refers to\"},"
     "\"signal\":{\"type\":\"integer\",\"enum\":[-1,0,1],"
     "\"description\":\"+1 good, -1 poor, 0 clears\"}},"
     "\"required\":[\"session\",\"turn\",\"signal\"]}",
     tool_agent_feedback},
    {"workspace_info",
     "Return the selected canonical workspace, repository identity, build "
     "adapter and live content fingerprint.",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_workspace_info},
    {"session_list",
     "List known session slugs.",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_session_list},
    {"session_new",
     "Create (or open) a session; returns its slug.",
     "{\"type\":\"object\",\"properties\":{"
     "\"slug\":{\"type\":\"string\",\"description\":\"Session slug; "
     "omitted generates one\"}}}",
     tool_session_new},
    {"session_delete",
     "Delete a session's files (transcript, ledger, blobs). "
     "Irreversible.",
     "{\"type\":\"object\",\"properties\":{"
     "\"slug\":{\"type\":\"string\",\"description\":\"Session to "
     "delete\"}},\"required\":[\"slug\"]}",
     tool_session_delete},
    {"session_stats",
     "Ledger totals and route mix for one session.",
     "{\"type\":\"object\",\"properties\":{"
     "\"session\":{\"type\":\"string\",\"description\":\"Session "
     "slug\"}},"
     "\"required\":[\"session\"]}",
     tool_session_stats},
    {"project_select",
     "Select the Asper project for a session (proxied to Asper); null "
     "deselects.",
     "{\"type\":\"object\",\"properties\":{"
     "\"slug\":{\"type\":[\"string\",\"null\"],\"description\":"
     "\"Project slug to activate; null deselects\"},"
     "\"session\":{\"type\":\"string\",\"description\":\"Session slug "
     "(default \\\"main\\\")\"}},"
     "\"required\":[\"slug\"]}",
     tool_project_select},
    {"cache_stats",
     "Semantic-cache hit / adapt / miss counters.",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_cache_stats},
    {"cache_clear",
     "Clear the semantic cache.",
     "{\"type\":\"object\",\"properties\":{"
     "\"scope\":{\"type\":\"string\",\"enum\":[\"session\",\"global\"],"
     "\"description\":\"Scope to clear; omitted clears both\"}}}",
     tool_cache_clear},
    {"telemetry_tail",
     "Most recent telemetry events as xCDN text lines, oldest first.",
     "{\"type\":\"object\",\"properties\":{"
     "\"n\":{\"type\":\"integer\",\"minimum\":0,\"description\":"
     "\"Number of most recent events (default 50)\"}}}",
     tool_telemetry_tail},
    {"engine_stats",
     "Engine-wide counters (asngn_get_stats).",
     "{\"type\":\"object\",\"properties\":{}}",
     tool_engine_stats},
};

#define TOOLS_N (sizeof TOOLS / sizeof TOOLS[0])

/* ═══════════════════════ method handlers ═══════════════════════ */

static jx_value *initialize_result(void) {
  jx_value *res = jx_object();
  jx_value *caps, *si;
  int ok = (res != NULL);
  ok &= jx_object_set(res, "protocolVersion", jx_string(MCP_LEGACY_VERSION)) == 0;
  caps = jx_object();
  ok &= jx_object_set(caps, "tools", jx_object()) == 0;
  ok &= jx_object_set(res, "capabilities", caps) == 0;
  si = jx_object();
  ok &= jx_object_set(si, "name", jx_string("asngn-mcp")) == 0;
  ok &= jx_object_set(si, "version", jx_string(asngn_version())) == 0;
  ok &= jx_object_set(res, "serverInfo", si) == 0;
  if (!ok) {
    jx_free(res);
    return NULL;
  }
  return res;
}

static jx_value *discover_result(void) {
  jx_value *res = jx_object();
  jx_value *versions = jx_array();
  jx_value *caps = jx_object();
  int ok = res != NULL && versions != NULL && caps != NULL;
  ok &= jx_array_push(versions, jx_string(MCP_MODERN_VERSION)) == 0;
  ok &= jx_array_push(versions, jx_string(MCP_LEGACY_VERSION)) == 0;
  ok &= jx_object_set(caps, "tools", jx_object()) == 0;
  ok &= jx_object_set(res, "supportedVersions", versions) == 0;
  ok &= jx_object_set(res, "capabilities", caps) == 0;
  ok &= jx_object_set(res, "instructions",
                      jx_string("Local Asterism agent tools.")) == 0;
  ok &= jx_object_set(res, "ttlMs", jx_int(3600000)) == 0;
  ok &= jx_object_set(res, "cacheScope", jx_string("private")) == 0;
  if (!ok) {
    jx_free(res);
    return NULL;
  }
  return res;
}

static void handle_tools_list(int want, jx_value *rid) {
  jx_value *arr = jx_array();
  jx_value *res;
  int ok = (arr != NULL);
  size_t i;
  for (i = 0; ok && i < TOOLS_N; i++) {
    jx_value *sch = NULL, *t;
    int tok;
    if (jx_parse(TOOLS[i].schema, strlen(TOOLS[i].schema), &sch) != 0) {
      ok = 0;
      break;
    }
    t = jx_object();
    tok = (t != NULL);
    tok &= jx_object_set(t, "name", jx_string(TOOLS[i].name)) == 0;
    tok &= jx_object_set(t, "description", jx_string(TOOLS[i].desc)) == 0;
    tok &= jx_object_set(t, "inputSchema", sch) == 0;
    if (!tok) {
      jx_free(t);
      ok = 0;
      break;
    }
    if (jx_array_push(arr, t) != 0) {
      ok = 0;
      break;
    }
  }
  if (!ok) {
    jx_free(arr);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  res = jx_object();
  if (jx_object_set(res, "tools", arr) != 0) {
    jx_free(res);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  send_result(want, rid, res);
}

static void handle_tools_call(server_state *st, int want, jx_value *rid,
                              const jx_value *params) {
  const char *name = NULL;
  const jx_value *args;
  const tool_def *tool = NULL;
  jx_value *payload = NULL;
  const char *pmsg = NULL;
  size_t i;
  int rc;

  if (!params || jx_typeof(params) != JX_OBJECT) {
    send_error(want, rid, -32602, "params must be an object", NULL);
    return;
  }
  if (arg_str(params, "name", &name, &pmsg) != 1) {
    send_error(want, rid, -32602,
               pmsg ? pmsg : "params.name must be a string", NULL);
    return;
  }
  args = jx_object_get(params, "arguments");
  if (args && jx_typeof(args) != JX_OBJECT) {
    send_error(want, rid, -32602, "params.arguments must be an object", NULL);
    return;
  }
  for (i = 0; i < TOOLS_N; i++) {
    if (strcmp(TOOLS[i].name, name) == 0) {
      tool = &TOOLS[i];
      break;
    }
  }
  if (!tool) {
    char buf[128];
    snprintf(buf, sizeof buf, "unknown tool: %.80s", name);
    send_error(want, rid, -32602, buf, NULL);
    return;
  }

  rc = tool->fn(st, args, &payload, &pmsg);
  switch (rc) {
  case TOOL_OK:
    send_tool_result(want, rid, payload, 0);
    break;
  case TOOL_FAIL:
    send_tool_result(want, rid, payload, 1);
    break;
  case TOOL_PARAM:
    send_error(want, rid, -32602, pmsg ? pmsg : "invalid params", NULL);
    break;
  default:
    send_error(want, rid, -32603, "out of memory", NULL);
    break;
  }
}

static void handle_request(server_state *st, jx_value *req) {
  const jx_value *idv, *ver, *methv, *protocolv = NULL;
  const char *method;
  jx_value *rid;
  int has_id;

  if (jx_typeof(req) == JX_ARRAY) {
    send_error(1, NULL, -32600, "batch requests are not supported", NULL);
    return;
  }
  if (jx_typeof(req) != JX_OBJECT) {
    send_error(1, NULL, -32600, "request must be a JSON object", NULL);
    return;
  }
  idv = jx_object_get(req, "id");
  has_id = (idv != NULL);
  ver = jx_object_get(req, "jsonrpc");
  methv = jx_object_get(req, "method");
  if (!ver || jx_typeof(ver) != JX_STRING ||
      strcmp(jx_string_value(ver), "2.0") != 0 || !methv ||
      jx_typeof(methv) != JX_STRING) {
    send_error(has_id, jx_clone(idv), -32600,
               "invalid JSON-RPC 2.0 request", NULL);
    return;
  }
  method = jx_string_value(methv);
  {
    const jx_value *params = jx_object_get(req, "params");
    const jx_value *meta = params && jx_typeof(params) == JX_OBJECT
                               ? jx_object_get(params, "_meta")
                               : NULL;
    protocolv = meta && jx_typeof(meta) == JX_OBJECT
                    ? jx_object_get(meta,
                        "io.modelcontextprotocol/protocolVersion")
                    : NULL;
    mcp_modern_response = protocolv && jx_typeof(protocolv) == JX_STRING &&
                          strcmp(jx_string_value(protocolv),
                                 MCP_MODERN_VERSION) == 0;
  }
  /* "notifications/..." methods are ignored only as true notifications
   * (no id); an id-carrying request must get a response, so it falls
   * through to the normal dispatch (=> -32601 when unknown). */
  if (!has_id && strncmp(method, "notifications/", 14) == 0) return;

  rid = jx_clone(idv); /* NULL (=> null id) when absent or on OOM */
  if (protocolv && !mcp_modern_response &&
      strcmp(method, "server/discover") != 0) {
    send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
    return;
  }
  if (strcmp(method, "initialize") == 0) {
    const jx_value *params = jx_object_get(req, "params");
    const jx_value *pv = params && jx_typeof(params) == JX_OBJECT
                             ? jx_object_get(params, "protocolVersion")
                             : NULL;
    mcp_modern_response = 0;
    if (pv && (jx_typeof(pv) != JX_STRING ||
               strcmp(jx_string_value(pv), MCP_LEGACY_VERSION) != 0)) {
      send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
      return;
    }
    jx_value *res = initialize_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res);
    return;
  }
  if (strcmp(method, "server/discover") == 0) {
    jx_value *res;
    mcp_modern_response = 1;
    res = discover_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res);
    return;
  }
  if (strcmp(method, "ping") == 0) {
    send_result(has_id, rid, jx_object());
    return;
  }
  if (strcmp(method, "tools/list") == 0) {
    handle_tools_list(has_id, rid);
    return;
  }
  if (strcmp(method, "tools/call") == 0) {
    handle_tools_call(st, has_id, rid, jx_object_get(req, "params"));
    return;
  }
  send_error(has_id, rid, -32601, "method not found", NULL);
}

/* ═══════════════════════ stdin line reader ═══════════════════════ */

/* 1 = line read (*out malloc'd, trailing \n/\r stripped), 0 = EOF with no
 * data, -1 = allocation failure (rest of the line drained). */
static int read_line(FILE *f, char **out, size_t *out_len) {
  size_t cap = 256, n = 0;
  char *buf = malloc(cap);
  int ch;
  if (!buf) {
    while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
    return -1;
  }
  while ((ch = getc(f)) != EOF && ch != '\n') {
    if (n + 2 > cap) {
      char *nb;
      if (cap > (size_t)-1 / 2) {
        free(buf);
        while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
        return -1;
      }
      cap *= 2;
      nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
        return -1;
      }
      buf = nb;
    }
    buf[n++] = (char)ch;
  }
  if (ch == EOF && n == 0) {
    free(buf);
    return 0;
  }
  while (n > 0 && buf[n - 1] == '\r') n--;
  buf[n] = '\0';
  *out = buf;
  *out_len = n;
  return 1;
}

static int line_blank(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] != ' ' && s[i] != '\t') return 0;
  }
  return 1;
}

/* ═══════════════════════ main ═══════════════════════ */

int main(int argc, char **argv) {
  const char *root = NULL, *config = NULL, *workspace = NULL;
  int allow_degraded = 0;
  asngn_open_params params;
  server_state st;
  asngn_err e;
  int i;

  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--help") == 0) {
      fputs(HELP, stdout);
      return 0;
    }
    if (strcmp(a, "--version") == 0) {
      printf("asngn-mcp %s\n", asngn_version());
      return 0;
    }
    if (strcmp(a, "--root") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "asngn-mcp: --root requires a value\n%s", USAGE);
        return 2;
      }
      root = argv[i];
    } else if (strncmp(a, "--root=", 7) == 0) {
      root = a + 7;
    } else if (strcmp(a, "--config") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "asngn-mcp: --config requires a value\n%s", USAGE);
        return 2;
      }
      config = argv[i];
    } else if (strncmp(a, "--config=", 9) == 0) {
      config = a + 9;
    } else if (strcmp(a, "--workspace") == 0) {
      if (++i >= argc) {
        fprintf(stderr, "asngn-mcp: --workspace requires a value\n%s", USAGE);
        return 2;
      }
      workspace = argv[i];
    } else if (strncmp(a, "--workspace=", 12) == 0) {
      workspace = a + 12;
    } else if (strcmp(a, "--allow-degraded") == 0) {
      allow_degraded = 1;
    } else {
      fprintf(stderr, "asngn-mcp: unknown argument \"%s\"\n%s", a, USAGE);
      return 2;
    }
  }
  if (!root || !*root) {
    fprintf(stderr, "asngn-mcp: --root is required\n%s", USAGE);
    return 2;
  }

  memset(&st, 0, sizeof st);
  memset(&params, 0, sizeof params);
  params.engine_root = root;
  params.config_path = config;
  params.workspace_root = workspace;
  params.allow_degraded = allow_degraded;
  e = asngn_open(&params, &st.ctx);
  if (e != ASNGN_OK || !st.ctx) {
    fprintf(stderr, "asngn-mcp: cannot open engine at \"%s\": %s\n",
            root, asngn_err_name(e));
    return 1;
  }
  /* MCP confirmation default: the effective autoconfirm
   * over MCP is mcp.autoconfirm ("deny" unless configured otherwise);
   * an MCP client cannot grant the agent anything. There is no public
   * setter, so this is the one sanctioned internal touch. */
  st.ctx->cfg.autoconfirm = st.ctx->cfg.mcp_autoconfirm;
  /* stdio belongs to the protocol: silence the default stderr log sink. */
  asngn_set_logger(st.ctx, NULL, NULL);

  for (;;) {
    char *line = NULL;
    size_t llen = 0;
    int rst = read_line(stdin, &line, &llen);
    if (rst == 0) break;
    if (rst < 0) {
      emit_line(OOM_RESPONSE);
      continue;
    }
    if (line_blank(line, llen)) {
      free(line);
      continue;
    }
    {
      jx_value *req = NULL;
      if (jx_parse(line, llen, &req) != 0) {
        send_error(1, NULL, -32700, "parse error", NULL);
      } else {
        handle_request(&st, req);
        jx_free(req);
      }
    }
    free(line);
  }

  while (st.sessions_n > 0) {
    asngn_session_close(st.sessions[--st.sessions_n]);
  }
  asngn_close(st.ctx);
  return 0;
}
