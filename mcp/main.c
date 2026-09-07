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
#include "asmodel_json.h"
#include "tasks.h"
#include "work.h"
#include "approval.h"
#include "recovery.h"
#include "consumption.h"

/* ═══════════════════════ usage / help ═══════════════════════ */

static const char USAGE[] =
    "usage: asngn-mcp [--root <dir>] [--config <file>] "
    "[--workspace <dir>] [--allow-degraded]\n"
    "       asngn-mcp --help | --version\n";

static const char HELP[] =
    "asngn-mcp - MCP stdio server for asngn\n"
    "\n"
    "usage: asngn-mcp [--root <dir>] [--config <file>] "
    "[--workspace <dir>] [--allow-degraded]\n"
    "       asngn-mcp --help | --version\n"
    "\n"
    "options:\n"
    "  --root <dir>     override engine root (default: ~/asngn)\n"
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
#define MCP_REQUEST_BYTES (8u * 1024u * 1024u)
static int mcp_modern_response;

/* ═══════════════════════ server state ═══════════════════════ */

/* Small registry of sessions this server has opened, keyed by slug.
 * Sessions are opened on demand and stay open until shutdown. */
#define MAX_SESSIONS 32

typedef struct {
  asngn_ctx *ctx;
  asngn_session *sessions[MAX_SESSIONS];
  size_t sessions_n;
  mcp_job *jobs[32];
} server_state;

/* ═══════════════════════ small helpers ═══════════════════════ */

static void emit_line(const char *s) {
  fputs(s, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

/* Serialize + emit + free resp; OOM falls back to the static response. */
static void send_value(asmodel_json_value *resp) {
  char *s = asmodel_json_write(resp, 0);
  asmodel_json_free(resp);
  if (!s) {
    emit_line(OOM_RESPONSE);
    return;
  }
  emit_line(s);
  free(s);
}

/* want == 0: notification — consume the owned arguments, emit nothing. */
static int decorate_modern_result(asmodel_json_value *result) {
  asmodel_json_value *meta, *info;
  int ok;
  if (!result || asmodel_json_typeof(result) != ASMODEL_JSON_OBJECT) return 0;
  meta = asmodel_json_object();
  info = asmodel_json_object();
  ok = meta != NULL && info != NULL;
  ok &= asmodel_json_object_set(info, "name", asmodel_json_string("asngn-mcp")) == 0;
  ok &= asmodel_json_object_set(info, "title", asmodel_json_string("asngn")) == 0;
  ok &= asmodel_json_object_set(info, "version", asmodel_json_string(asngn_version())) == 0;
  ok &= asmodel_json_object_set(meta, "io.modelcontextprotocol/serverInfo", info) == 0;
  ok &= asmodel_json_object_set(result, "resultType", asmodel_json_string("complete")) == 0;
  ok &= asmodel_json_object_set(result, "_meta", meta) == 0;
  return ok;
}

static void send_result(int want, asmodel_json_value *id, asmodel_json_value *result) {
  asmodel_json_value *resp;
  int ok;
  if (!want) {
    asmodel_json_free(id);
    asmodel_json_free(result);
    return;
  }
  if (mcp_modern_response && !decorate_modern_result(result)) {
    asmodel_json_free(id);
    asmodel_json_free(result);
    emit_line(OOM_RESPONSE);
    return;
  }
  resp = asmodel_json_object();
  ok = (resp != NULL);
  ok &= asmodel_json_object_set(resp, "jsonrpc", asmodel_json_string("2.0")) == 0;
  ok &= asmodel_json_object_set(resp, "id", id ? id : asmodel_json_null()) == 0;
  ok &= asmodel_json_object_set(resp, "result", result) == 0;
  if (!ok) {
    asmodel_json_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

static void send_error(int want, asmodel_json_value *id, int code, const char *message,
                       const char *asngn_name) {
  asmodel_json_value *err, *resp;
  int ok;
  if (!want) {
    asmodel_json_free(id);
    return;
  }
  err = asmodel_json_object();
  ok = (err != NULL);
  ok &= asmodel_json_object_set(err, "code", asmodel_json_int(code)) == 0;
  ok &= asmodel_json_object_set(err, "message", asmodel_json_string(message)) == 0;
  if (asngn_name) {
    asmodel_json_value *data = asmodel_json_object();
    ok &= asmodel_json_object_set(data, "asngn", asmodel_json_string(asngn_name)) == 0;
    ok &= asmodel_json_object_set(err, "data", data) == 0;
  }
  resp = asmodel_json_object();
  ok &= (resp != NULL);
  ok &= asmodel_json_object_set(resp, "jsonrpc", asmodel_json_string("2.0")) == 0;
  ok &= asmodel_json_object_set(resp, "id", id ? id : asmodel_json_null()) == 0;
  ok &= asmodel_json_object_set(resp, "error", err) == 0;
  if (!ok) {
    asmodel_json_free(resp);
    emit_line(OOM_RESPONSE);
    return;
  }
  send_value(resp);
}

/* Wrap a tool payload in the MCP content envelope and send it. */
static void send_tool_result(int want, asmodel_json_value *id, asmodel_json_value *payload,
                             int is_error) {
  char *txt;
  asmodel_json_value *item, *content, *res;
  int ok;
  if (!want) {
    asmodel_json_free(id);
    asmodel_json_free(payload);
    return;
  }
  txt = asmodel_json_write(payload, 0);
  asmodel_json_free(payload);
  if (!txt) {
    asmodel_json_free(id);
    emit_line(OOM_RESPONSE);
    return;
  }
  item = asmodel_json_object();
  ok = (item != NULL);
  ok &= asmodel_json_object_set(item, "type", asmodel_json_string("text")) == 0;
  ok &= asmodel_json_object_set(item, "text", asmodel_json_string(txt)) == 0;
  free(txt);
  content = asmodel_json_array();
  ok &= asmodel_json_array_push(content, item) == 0;
  res = asmodel_json_object();
  ok &= asmodel_json_object_set(res, "content", content) == 0;
  ok &= asmodel_json_object_set(res, "isError", asmodel_json_bool(is_error)) == 0;
  if (!ok) {
    asmodel_json_free(res);
    asmodel_json_free(id);
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
static int arg_str(const asmodel_json_value *args, const char *key, const char **out,
                   const char **msg) {
  const asmodel_json_value *v = asmodel_json_object_get(args, key);
  if (!v) return 0;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_STRING) return -1;
  if (asmodel_json_string_length(v) != strlen(asmodel_json_string_value(v))) {
    *msg = "string must not contain NUL";
    return -1;
  }
  *out = asmodel_json_string_value(v);
  return 1;
}

static int arg_int(const asmodel_json_value *args, const char *key, long long *out) {
  const asmodel_json_value *v = asmodel_json_object_get(args, key);
  double d;
  if (!v) return 0;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_NUMBER) return -1;
  if (asmodel_json_is_int(v)) {
    *out = asmodel_json_int_value(v);
    return 1;
  }
  /* Schema-valid integral spellings (5.0, 1e2): accept when the double
   * is finite, integral, and exactly representable as long long. */
  d = asmodel_json_double_value(v);
  if (!isfinite(d) || d != floor(d) || d < -0x1p63 || d >= 0x1p63)
    return -1;
  *out = (long long)d;
  return 1;
}

static int arg_bool(const asmodel_json_value *args, const char *key, int *out) {
  const asmodel_json_value *v = asmodel_json_object_get(args, key);
  if (!v) return 0;
  if (asmodel_json_typeof(v) != ASMODEL_JSON_BOOL) return -1;
  *out = asmodel_json_bool_value(v);
  return 1;
}

/* ═══════════════════════ payload builders ═══════════════════════ */

/* Doubles from engine counters; non-finite degrades to null, never to a
 * writer failure. */
static asmodel_json_value *asmodel_json_finite(double d) {
  return isfinite(d) ? asmodel_json_double(d) : asmodel_json_null();
}

static asmodel_json_value *ok_payload(void) {
  asmodel_json_value *o = asmodel_json_object();
  if (asmodel_json_object_set(o, "ok", asmodel_json_bool(1)) != 0) {
    asmodel_json_free(o);
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

typedef int (*tool_fn)(server_state *st, const asmodel_json_value *args,
                       asmodel_json_value **out, const char **msg);

/* {"error": <name>, "message": <text>} with isError:true. */
static int fail_payload(asmodel_json_value **out, const char *name,
                        const char *message) {
  asmodel_json_value *o = asmodel_json_object();
  asmodel_json_value *m;
  int ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "error", asmodel_json_string(name)) == 0;
  m = asmodel_json_string(message ? message : "");
  if (!m) m = asmodel_json_string(""); /* message not valid UTF-8: drop it */
  ok &= asmodel_json_object_set(o, "message", m) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_FAIL;
}

static int engine_fail(server_state *st, asngn_err e, asmodel_json_value **out) {
  return fail_payload(out, asngn_err_name(e), asngn_last_error(st->ctx));
}

/* Resolve a slug (NULL = "main") to an open session: reuse a registered
 * handle or open on demand and register it. */
static int state_session(server_state *st, const char *slug,
                         asngn_session **out_s, asmodel_json_value **out) {
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

static int tool_session_work(server_state *st, const asmodel_json_value *args,
                             asmodel_json_value **out, const char **msg) {
  const char *slug = NULL;
  asngn_session *s = NULL;
  if (arg_str(args,"session",&slug,msg) < 0) BADP("session must be a string");
  int rc = state_session(st,slug,&s,out);
  if (rc != TOOL_OK) return rc;
  asngn_err e = mcp_work_request(s,args,out);
  return e == ASNGN_OK ? TOOL_OK : engine_fail(st,e,out);
}

static int tool_session_approval(server_state *st, const asmodel_json_value *args,
                                 asmodel_json_value **out, const char **msg) {
  const char *slug = NULL;
  asngn_session *s = NULL;
  if (arg_str(args,"session",&slug,msg) < 0 ||
      asmodel_json_object_count(args) > (size_t)(slug ? 1 : 0)) BADP("only session is accepted");
  int rc = state_session(st,slug,&s,out);
  if (rc != TOOL_OK) return rc;
  asngn_err e = mcp_approval_read(s,out);
  return e == ASNGN_OK ? TOOL_OK : engine_fail(st,e,out);
}

static int tool_agent_recover(server_state *st, const asmodel_json_value *args,
                              asmodel_json_value **out, const char **msg) {
  const char *id = NULL, *slug = NULL;
  asngn_session *s = NULL;
  if (arg_str(args,"task_id",&id,msg) != 1 || !asngn_uuid_valid(id) ||
      arg_str(args,"session",&slug,msg) != 1 || asmodel_json_object_count(args) != 2)
    BADP("session and task_id UUID are required; no other fields are accepted");
  int rc = state_session(st,slug,&s,out);
  if (rc != TOOL_OK) return rc;
  asngn_err e = mcp_task_recover(s,id,out);
  return e == ASNGN_OK ? TOOL_OK : engine_fail(st,e,out);
}

static int tool_agent_submit(server_state *st, const asmodel_json_value *args,
                              asmodel_json_value **out, const char **msg) {
  const char *message = NULL, *session = NULL;
  asngn_session *s = NULL;
  size_t slot = 0;
  asngn_err e;
  int rc;
  if (arg_str(args, "message", &message, msg) != 1 ||
      arg_str(args, "session", &session, msg) < 0) BADP("message/session must be strings");
  while (slot < 32 && st->jobs[slot]) slot++;
  if (slot == 32) return engine_fail(st, ASNGN_ERR_BUSY, out);
  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;
  e = mcp_job_submit(s, message, &st->jobs[slot]);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  *out = asmodel_json_object();
  if (!*out || asmodel_json_object_set(*out, "task_id", asmodel_json_string(mcp_job_id(st->jobs[slot]))) != 0) {
    asmodel_json_free(*out); *out = NULL; return TOOL_OOM;
  }
  return TOOL_OK;
}

static int job_request(server_state *st, const asmodel_json_value *args, asmodel_json_value **out,
                        const char **msg, int cancel, int release) {
  const char *id = NULL;
  unsigned long long cursor = 0;
  asngn_err e;
  if (arg_str(args, "task_id", &id, msg) != 1) BADP("task_id must be a string");
  const asmodel_json_value *v = asmodel_json_object_get(args, "cursor");
  if (v) {
    if (!asmodel_json_is_int(v) || asmodel_json_int_value(v) < 0) BADP("cursor must be nonnegative");
    cursor = (unsigned long long)asmodel_json_int_value(v);
  }
  for (size_t i = 0; i < 32; i++) if (st->jobs[i] && !strcmp(id, mcp_job_id(st->jobs[i]))) {
    if (cancel) (void)mcp_job_cancel(st->jobs[i]);
    e = mcp_job_poll(st->jobs[i], cursor, out);
    if (e != ASNGN_OK) return engine_fail(st, e, out);
    if (release && asmodel_json_bool_value(asmodel_json_object_get(*out, "done"))) {
      mcp_job_free(st->jobs[i]); st->jobs[i] = NULL;
    }
    return TOOL_OK;
  }
  return engine_fail(st, ASNGN_ERR_NOT_FOUND, out);
}
static int tool_agent_poll(server_state *s, const asmodel_json_value *a, asmodel_json_value **o, const char **m) {
  return job_request(s, a, o, m, 0, 0);
}
static int tool_agent_cancel(server_state *s, const asmodel_json_value *a, asmodel_json_value **o, const char **m) {
  return job_request(s, a, o, m, 1, 0);
}
static int tool_agent_release(server_state *s, const asmodel_json_value *a, asmodel_json_value **o, const char **m) {
  return job_request(s, a, o, m, 0, 1);
}

static int tool_agent_ask(server_state *st, const asmodel_json_value *args,
                          asmodel_json_value **out, const char **msg) {
  const char *message = NULL, *session = NULL, *detail = NULL;
  const char *active_file=NULL,*objective=NULL;
  int no_tools = 0, rc, ok;
  asngn_session *s = NULL;
  asngn_submit_opts opts;
  asngn_task *t = NULL;
  asngn_turn_result r;
  asngn_err e;
  asmodel_json_value *o;

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

  if (arg_str(args,"active_file",&active_file,msg)<0 ||
      arg_str(args,"objective",&objective,msg)<0)
    BADP("agent_ask: active_file and objective must be strings");
  if (active_file || objective) {
    e=asngn_session_retrieval_context(s,active_file,objective);
    if (e!=ASNGN_OK) return engine_fail(st,e,out);
  }

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

  o = asmodel_json_object();
  ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "answer", asmodel_json_string(r.answer ? r.answer : "")) == 0;
  ok &= asmodel_json_object_set(o, "turn", asmodel_json_int((long long)r.turn)) == 0;
  ok &= asmodel_json_object_set(o, "class", asmodel_json_string(r.klass)) == 0;
  ok &= asmodel_json_object_set(o, "detail", asmodel_json_string(r.detail)) == 0;
  ok &= asmodel_json_object_set(o, "tier", asmodel_json_string(r.tier)) == 0;
  ok &= asmodel_json_object_set(o, "cache", asmodel_json_string(r.cache)) == 0;
  ok &= asmodel_json_object_set(o, "capped", asmodel_json_bool(r.capped)) == 0;
  ok &= asmodel_json_object_set(o, "clarify", asmodel_json_bool(r.clarify)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_prompt",
                      asmodel_json_int((long long)r.tokens_prompt)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_gen",
                      asmodel_json_int((long long)r.tokens_gen)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_saved",
                      asmodel_json_int((long long)r.tokens_saved)) == 0;
  ok &= asmodel_json_object_set(o, "duration_ms",
                      asmodel_json_int((long long)r.duration_ms)) == 0;
  asngn_turn_result_free(&r);
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_agent_feedback(server_state *st, const asmodel_json_value *args,
                               asmodel_json_value **out, const char **msg) {
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

static int tool_session_list(server_state *st, const asmodel_json_value *args,
                             asmodel_json_value **out, const char **msg) {
  char **slugs = NULL;
  size_t n = 0, i;
  asngn_err e;
  asmodel_json_value *arr, *o;
  int ok;
  (void)args;
  (void)msg;

  e = asngn_session_list(st->ctx, &slugs, &n);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  arr = asmodel_json_array();
  ok = (arr != NULL);
  for (i = 0; i < n; i++) {
    if (asmodel_json_array_push(arr, asmodel_json_string(slugs[i])) != 0) ok = 0;
  }
  asngn_strings_free(slugs, n);
  o = asmodel_json_object();
  ok &= asmodel_json_object_set(o, "sessions", arr) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_delete(server_state *st, const asmodel_json_value *args,
                               asmodel_json_value **out, const char **msg) {
  const char *slug = NULL;
  size_t i;
  asngn_err e;
  asmodel_json_value *o;

  if (arg_str(args, "slug", &slug, msg) < 0 || slug == NULL)
    BADP("session_delete: \"slug\" is required");
  /* a session this server holds open must be closed first */
  for (i = 0; i < st->sessions_n; i++) {
    if (strcmp(asngn_session_slug(st->sessions[i]), slug) == 0) {
      for (size_t j = 0; j < 32; j++)
        if (st->jobs[j] && mcp_job_uses_session(st->jobs[j],st->sessions[i]))
          return fail_payload(out,"ASNGN_ERR_BUSY","release session tasks before deleting the session");
      asngn_session_close(st->sessions[i]);
      memmove(&st->sessions[i], &st->sessions[i + 1],
              (st->sessions_n - i - 1) * sizeof st->sessions[0]);
      st->sessions_n--;
      break;
    }
  }
  e = asngn_session_delete(st->ctx, slug);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = asmodel_json_object();
  if (asmodel_json_object_set(o, "deleted", asmodel_json_string(slug)) != 0) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_new(server_state *st, const asmodel_json_value *args,
                            asmodel_json_value **out, const char **msg) {
  const char *slug = NULL;
  asngn_session *s = NULL;
  asngn_err e;
  asmodel_json_value *o;
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
  o = asmodel_json_object();
  if (asmodel_json_object_set(o, "slug", asmodel_json_string(asngn_session_slug(s))) != 0) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int parse_usage_mode(const char *value, asngn_usage_mode *out) {
  if (strcmp(value, "chat") == 0) *out = ASNGN_USAGE_CHAT;
  else if (strcmp(value, "coding") == 0) *out = ASNGN_USAGE_CODING;
  else if (strcmp(value, "automate") == 0) *out = ASNGN_USAGE_AUTOMATE;
  else return 0;
  return 1;
}

static int parse_security_profile(const char *value,
                                  asngn_security_profile *out) {
  if (strcmp(value, "chat") == 0) *out = ASNGN_SECURITY_CHAT;
  else if (strcmp(value, "coding-readonly") == 0)
    *out = ASNGN_SECURITY_CODING_READONLY;
  else if (strcmp(value, "coding-sandboxed") == 0)
    *out = ASNGN_SECURITY_CODING_SANDBOXED;
  else if (strcmp(value, "automation-ci") == 0)
    *out = ASNGN_SECURITY_AUTOMATION_CI;
  else return 0;
  return 1;
}

static int tool_session_mode(server_state *st, const asmodel_json_value *args,
                             asmodel_json_value **out, const char **msg) {
  const char *session = NULL, *mode_value = NULL, *profile_value = NULL;
  asngn_usage_mode mode = ASNGN_USAGE_CHAT;
  asngn_security_profile profile = ASNGN_SECURITY_CHAT;
  asngn_session *s = NULL;
  asngn_err e;
  asmodel_json_value *o;
  int rc, ok;

  if (arg_str(args, "session", &session, msg) < 0)
    BADP("session_mode: \"session\" must be a string");
  rc = arg_str(args, "mode", &mode_value, msg);
  if (rc < 0 || (rc == 1 && !parse_usage_mode(mode_value, &mode)))
    BADP("session_mode: \"mode\" must be chat|coding|automate");
  rc = arg_str(args, "security_profile", &profile_value, msg);
  if (rc < 0 ||
      (rc == 1 && !parse_security_profile(profile_value, &profile)))
    BADP("session_mode: invalid \"security_profile\"");
  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;
  if (mode_value != NULL) {
    e = asngn_session_set_mode(s, mode);
    if (e != ASNGN_OK) return engine_fail(st, e, out);
  }
  if (profile_value != NULL) {
    e = asngn_session_set_security_profile(s, profile);
    if (e != ASNGN_OK) return engine_fail(st, e, out);
  }
  e = asngn_session_get_mode(s, &mode, &profile);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = asmodel_json_object();
  ok = o != NULL;
  ok &= asmodel_json_object_set(o, "mode", asmodel_json_string(asngn_usage_mode_name(mode))) == 0;
  ok &= asmodel_json_object_set(o, "security_profile",
                      asmodel_json_string(asngn_security_profile_name(profile))) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_session_stats(server_state *st, const asmodel_json_value *args,
                              asmodel_json_value **out, const char **msg) {
  const char *session = NULL;
  asngn_session *s = NULL;
  asngn_session_stats stt;
  asngn_err e;
  asmodel_json_value *o;
  int rc, ok;

  if (arg_str(args, "session", &session, msg) != 1)
    BADP("session_stats: \"session\" must be a string");
  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;

  memset(&stt, 0, sizeof stt);
  e = asngn_session_get_stats(s, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);

  o = asmodel_json_object();
  ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "turns", asmodel_json_int((long long)stt.turns)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_prompt",
                      asmodel_json_int((long long)stt.tokens_prompt)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_gen",
                      asmodel_json_int((long long)stt.tokens_gen)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_saved",
                      asmodel_json_int((long long)stt.tokens_saved)) == 0;
  ok &= asmodel_json_object_set(o, "cache_hits",
                      asmodel_json_int((long long)stt.cache_hits)) == 0;
  ok &= asmodel_json_object_set(o, "cache_adapts",
                      asmodel_json_int((long long)stt.cache_adapts)) == 0;
  ok &= asmodel_json_object_set(o, "cache_misses",
                      asmodel_json_int((long long)stt.cache_misses)) == 0;
  ok &= asmodel_json_object_set(o, "clarifies",
                      asmodel_json_int((long long)stt.clarifies)) == 0;
  ok &= asmodel_json_object_set(o, "capped", asmodel_json_int((long long)stt.capped)) == 0;
  ok &= asmodel_json_object_set(o, "escalations",
                      asmodel_json_int((long long)stt.escalations)) == 0;
  ok &= asmodel_json_object_set(o, "qpt_rolling", asmodel_json_finite(stt.qpt_rolling)) == 0;
  ok &= asmodel_json_object_set(o, "world_epoch",
                      asmodel_json_int((long long)stt.world_epoch)) == 0;
  ok &= asmodel_json_object_set(o, "spent_tokens", asmodel_json_int(stt.spent_tokens)) == 0;
  ok &= asmodel_json_object_set(o, "token_basis", asmodel_json_string("committed_turns")) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_project_select(server_state *st, const asmodel_json_value *args,
                               asmodel_json_value **out, const char **msg) {
  const asmodel_json_value *sv = asmodel_json_object_get(args, "slug");
  const char *slug = NULL, *session = NULL;
  asngn_session *s = NULL;
  asngn_err e;
  asmodel_json_value *o;
  int rc, ok;

  if (!sv)
    BADP("project_select: \"slug\" is required (string or null)");
  if (asmodel_json_typeof(sv) == ASMODEL_JSON_STRING) {
    if (asmodel_json_string_length(sv) != strlen(asmodel_json_string_value(sv)))
      BADP("string must not contain NUL");
    slug = asmodel_json_string_value(sv);
  } else if (asmodel_json_typeof(sv) != ASMODEL_JSON_NULL) {
    BADP("project_select: \"slug\" must be a string or null");
  }
  if (arg_str(args, "session", &session, msg) < 0)
    BADP("project_select: \"session\" must be a string");

  rc = state_session(st, session, &s, out);
  if (rc != TOOL_OK) return rc;
  e = asngn_session_project(s, slug);
  if (e != ASNGN_OK) return engine_fail(st, e, out);

  o = asmodel_json_object();
  ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "ok", asmodel_json_bool(1)) == 0;
  ok &= asmodel_json_object_set(o, "active", slug ? asmodel_json_string(slug) : asmodel_json_null()) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_cache_stats(server_state *st, const asmodel_json_value *args,
                            asmodel_json_value **out, const char **msg) {
  asngn_stats stt;
  asngn_err e;
  asmodel_json_value *o;
  int ok;
  (void)args;
  (void)msg;

  memset(&stt, 0, sizeof stt);
  e = asngn_get_stats(st->ctx, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = asmodel_json_object();
  ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "hits", asmodel_json_int((long long)stt.cache_hits)) == 0;
  ok &= asmodel_json_object_set(o, "adapts",
                      asmodel_json_int((long long)stt.cache_adapts)) == 0;
  ok &= asmodel_json_object_set(o, "misses",
                      asmodel_json_int((long long)stt.cache_misses)) == 0;
  ok &= asmodel_json_object_set(o, "tool_cache_hits",
                      asmodel_json_int((long long)stt.tool_cache_hits)) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_cache_clear(server_state *st, const asmodel_json_value *args,
                            asmodel_json_value **out, const char **msg) {
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

static int tool_telemetry_tail(server_state *st, const asmodel_json_value *args,
                               asmodel_json_value **out, const char **msg) {
  long long n = 50; /* default when absent */
  char **lines = NULL;
  size_t ln = 0, i;
  asngn_err e;
  asmodel_json_value *arr, *o;
  int rc, ok;

  rc = arg_int(args, "n", &n);
  if (rc < 0 || n < 0)
    BADP("telemetry_tail: \"n\" must be a non-negative integer");

  e = asngn_telemetry_tail(st->ctx, (size_t)n, &lines, &ln);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  arr = asmodel_json_array();
  ok = (arr != NULL);
  for (i = 0; i < ln; i++) {
    if (asmodel_json_array_push(arr, asmodel_json_string(lines[i])) != 0) ok = 0;
  }
  asngn_strings_free(lines, ln);
  o = asmodel_json_object();
  ok &= asmodel_json_object_set(o, "events", arr) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

static int tool_engine_stats(server_state *st, const asmodel_json_value *args,
                             asmodel_json_value **out, const char **msg) {
  asngn_stats stt;
  asngn_err e;
  asmodel_json_value *o;
  int ok;
  (void)args;
  (void)msg;

  memset(&stt, 0, sizeof stt);
  e = asngn_get_stats(st->ctx, &stt);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = asmodel_json_object();
  ok = (o != NULL);
  ok &= asmodel_json_object_set(o, "turns", asmodel_json_int((long long)stt.turns)) == 0;
  ok &= asmodel_json_object_set(o, "cache_hits",
                      asmodel_json_int((long long)stt.cache_hits)) == 0;
  ok &= asmodel_json_object_set(o, "cache_adapts",
                      asmodel_json_int((long long)stt.cache_adapts)) == 0;
  ok &= asmodel_json_object_set(o, "cache_misses",
                      asmodel_json_int((long long)stt.cache_misses)) == 0;
  ok &= asmodel_json_object_set(o, "tool_calls",
                      asmodel_json_int((long long)stt.tool_calls)) == 0;
  ok &= asmodel_json_object_set(o, "tool_cache_hits",
                      asmodel_json_int((long long)stt.tool_cache_hits)) == 0;
  ok &= asmodel_json_object_set(o, "escalations",
                      asmodel_json_int((long long)stt.escalations)) == 0;
  ok &= asmodel_json_object_set(o, "guard_trips",
                      asmodel_json_int((long long)stt.guard_trips)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_prompt",
                      asmodel_json_int((long long)stt.tokens_prompt)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_gen",
                      asmodel_json_int((long long)stt.tokens_gen)) == 0;
  ok &= asmodel_json_object_set(o, "tokens_saved",
                      asmodel_json_int((long long)stt.tokens_saved)) == 0;
  ok &= asmodel_json_object_set(o, "qpt_rolling", asmodel_json_finite(stt.qpt_rolling)) == 0;
  ok &= asmodel_json_object_set(o, "last_turn_at", asmodel_json_int(stt.last_turn_at)) == 0;
  ok &= asmodel_json_object_set(o, "last_sweep_at", asmodel_json_int(stt.last_sweep_at)) == 0;
  if (!ok) {
    asmodel_json_free(o);
    return TOOL_OOM;
  }
  *out = o;
  return TOOL_OK;
}

/* ═══════════════════════ tool table ════════════════════════════ */

static int tool_engine_consumption(server_state *st, const asmodel_json_value *args,
                                   asmodel_json_value **out, const char **msg) {
  if (asmodel_json_typeof(args) != ASMODEL_JSON_OBJECT || asmodel_json_object_count(args))
    BADP("engine_consumption: arguments must be an empty object");
  asngn_err e = mcp_consumption_read(st->ctx,out);
  return e == ASNGN_OK ? TOOL_OK : engine_fail(st,e,out);
}

static int tool_workspace_info(server_state *st, const asmodel_json_value *args,
                               asmodel_json_value **out, const char **msg) {
  asngn_workspace_info w;
  asmodel_json_value *o;
  int ok;
  asngn_err e;
  (void)args; (void)msg;
  memset(&w, 0, sizeof w);
  e = asngn_workspace_get(st->ctx, &w);
  if (e != ASNGN_OK) return engine_fail(st, e, out);
  o = asmodel_json_object(); ok = o != NULL;
  ok &= asmodel_json_object_set(o, "canonical_root", asmodel_json_string(w.canonical_root)) == 0;
  ok &= asmodel_json_object_set(o, "repository_root", asmodel_json_string(w.repository_root)) == 0;
  ok &= asmodel_json_object_set(o, "head", asmodel_json_string(w.head)) == 0;
  ok &= asmodel_json_object_set(o, "branch", asmodel_json_string(w.branch)) == 0;
  ok &= asmodel_json_object_set(o, "project_id", asmodel_json_string(w.project_id)) == 0;
  ok &= asmodel_json_object_set(o, "ignore_rules", asmodel_json_string(w.ignore_rules)) == 0;
  ok &= asmodel_json_object_set(o, "build_adapter", asmodel_json_string(w.build_adapter)) == 0;
  ok &= asmodel_json_object_set(o, "fingerprint", asmodel_json_string(w.fingerprint)) == 0;
  if (!ok) { asmodel_json_free(o); return TOOL_OOM; }
  *out = o; return TOOL_OK;
}

typedef struct {
  const char *name;
  const char *desc;
  const char *schema; /* JSON Schema literal, parsed on tools/list */
  tool_fn fn;
} tool_def;

static const tool_def TOOLS[] = {
    {"engine_consumption", "Read durable engine-wide inference charges, known usage, unknown usage and unsettled reservations. Token/call counters are decimal strings, not conversation totals or monetary prices.",
     "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{}}", tool_engine_consumption},
    {"agent_recover", "Read a task's durable outcome after release or restart. Requires an idle session; never resumes execution or replays events/effects.",
     "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"session\":{\"type\":\"string\"},\"task_id\":{\"type\":\"string\"}},\"required\":[\"session\",\"task_id\"]}", tool_agent_recover},
    {"session_approval", "Inspect the latest durable confirmation and complete redacted arguments. Read-only; never grants permission.",
     "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"}},\"additionalProperties\":false}", tool_session_approval},
    {"session_work", "Read or define host acceptance criteria; revisions prevent stale updates. Only the runtime records proof.",
     MCP_WORK_SCHEMA, tool_session_work},
    {"agent_submit", "Submit asynchronously; returns a task ID.",
     "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"},\"session\":{\"type\":\"string\"}},\"required\":[\"message\"]}", tool_agent_submit},
    {"agent_poll", "Read retained events using a cursor; gaps are explicit.",
     "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"},\"cursor\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"task_id\"]}", tool_agent_poll},
    {"agent_cancel", "Cancel a submitted task and return its current state.",
     "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"}},\"required\":[\"task_id\"]}", tool_agent_cancel},
    {"agent_release", "Release a completed task; running tasks remain available.",
     "{\"type\":\"object\",\"properties\":{\"task_id\":{\"type\":\"string\"}},\"required\":[\"task_id\"]}", tool_agent_release},
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
     "\"active_file\":{\"type\":\"string\"},"
     "\"objective\":{\"type\":\"string\"},"
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
    {"session_mode",
     "Read or change a session mode and explicit security profile.",
     "{\"type\":\"object\",\"properties\":{"
     "\"session\":{\"type\":\"string\",\"description\":\"Session slug "
     "(default \\\"main\\\")\"},"
     "\"mode\":{\"type\":\"string\",\"enum\":[\"chat\",\"coding\","
     "\"automate\"]},"
     "\"security_profile\":{\"type\":\"string\",\"enum\":[\"chat\","
     "\"coding-readonly\",\"coding-sandboxed\",\"automation-ci\"]}}}",
     tool_session_mode},
    {"session_stats",
     "Ledger totals and route mix for one session.",
     "{\"type\":\"object\",\"properties\":{"
     "\"session\":{\"type\":\"string\",\"description\":\"Session "
     "slug\"}},"
     "\"required\":[\"session\"]}",
     tool_session_stats},
    {"project_select",
     "Select the asper project for a session (proxied to asper); null "
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

static asmodel_json_value *initialize_result(void) {
  asmodel_json_value *res = asmodel_json_object();
  asmodel_json_value *caps, *si;
  int ok = (res != NULL);
  ok &= asmodel_json_object_set(res, "protocolVersion", asmodel_json_string(MCP_LEGACY_VERSION)) == 0;
  caps = asmodel_json_object();
  ok &= asmodel_json_object_set(caps, "tools", asmodel_json_object()) == 0;
  asmodel_json_value *experimental = NULL;
  ok &= asmodel_json_parse("{\"dev.asterism/asngn\":{\"contractVersion\":1}}",
                           strlen("{\"dev.asterism/asngn\":{\"contractVersion\":1}}"),
                           &experimental) == 0;
  ok &= asmodel_json_object_set(caps, "experimental", experimental) == 0;
  ok &= asmodel_json_object_set(res, "capabilities", caps) == 0;
  si = asmodel_json_object();
  ok &= asmodel_json_object_set(si, "name", asmodel_json_string("asngn-mcp")) == 0;
  ok &= asmodel_json_object_set(si, "title", asmodel_json_string("asngn")) == 0;
  ok &= asmodel_json_object_set(si, "version", asmodel_json_string(asngn_version())) == 0;
  ok &= asmodel_json_object_set(res, "serverInfo", si) == 0;
  if (!ok) {
    asmodel_json_free(res);
    return NULL;
  }
  return res;
}

static asmodel_json_value *discover_result(void) {
  asmodel_json_value *res = asmodel_json_object();
  asmodel_json_value *versions = asmodel_json_array();
  asmodel_json_value *caps = asmodel_json_object();
  int ok = res != NULL && versions != NULL && caps != NULL;
  ok &= asmodel_json_array_push(versions, asmodel_json_string(MCP_MODERN_VERSION)) == 0;
  ok &= asmodel_json_array_push(versions, asmodel_json_string(MCP_LEGACY_VERSION)) == 0;
  ok &= asmodel_json_object_set(caps, "tools", asmodel_json_object()) == 0;
  ok &= asmodel_json_object_set(res, "supportedVersions", versions) == 0;
  ok &= asmodel_json_object_set(res, "capabilities", caps) == 0;
  ok &= asmodel_json_object_set(res, "instructions",
                      asmodel_json_string("Local asterism agent tools.")) == 0;
  ok &= asmodel_json_object_set(res, "ttlMs", asmodel_json_int(3600000)) == 0;
  ok &= asmodel_json_object_set(res, "cacheScope", asmodel_json_string("private")) == 0;
  if (!ok) {
    asmodel_json_free(res);
    return NULL;
  }
  return res;
}

static void handle_tools_list(int want, asmodel_json_value *rid) {
  asmodel_json_value *arr = asmodel_json_array();
  asmodel_json_value *res;
  int ok = (arr != NULL);
  size_t i;
  for (i = 0; ok && i < TOOLS_N; i++) {
    asmodel_json_value *sch = NULL, *t;
    int tok;
    if (asmodel_json_parse(TOOLS[i].schema, strlen(TOOLS[i].schema), &sch) != 0) {
      ok = 0;
      break;
    }
    t = asmodel_json_object();
    tok = (t != NULL);
    tok &= asmodel_json_object_set(t, "name", asmodel_json_string(TOOLS[i].name)) == 0;
    tok &= asmodel_json_object_set(t, "description", asmodel_json_string(TOOLS[i].desc)) == 0;
    tok &= asmodel_json_object_set(t, "inputSchema", sch) == 0;
    if (!tok) {
      asmodel_json_free(t);
      ok = 0;
      break;
    }
    if (asmodel_json_array_push(arr, t) != 0) {
      ok = 0;
      break;
    }
  }
  if (!ok) {
    asmodel_json_free(arr);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  res = asmodel_json_object();
  if (asmodel_json_object_set(res, "tools", arr) != 0) {
    asmodel_json_free(res);
    send_error(want, rid, -32603, "out of memory", NULL);
    return;
  }
  send_result(want, rid, res);
}

static void handle_tools_call(server_state *st, int want, asmodel_json_value *rid,
                              const asmodel_json_value *params) {
  const char *name = NULL;
  const asmodel_json_value *args;
  const tool_def *tool = NULL;
  asmodel_json_value *payload = NULL;
  const char *pmsg = NULL;
  size_t i;
  int rc;

  if (!params || asmodel_json_typeof(params) != ASMODEL_JSON_OBJECT) {
    send_error(want, rid, -32602, "params must be an object", NULL);
    return;
  }
  if (arg_str(params, "name", &name, &pmsg) != 1) {
    send_error(want, rid, -32602,
               pmsg ? pmsg : "params.name must be a string", NULL);
    return;
  }
  args = asmodel_json_object_get(params, "arguments");
  if (args && asmodel_json_typeof(args) != ASMODEL_JSON_OBJECT) {
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

static void handle_request(server_state *st, asmodel_json_value *req) {
  const asmodel_json_value *idv, *ver, *methv, *protocolv = NULL;
  const char *method;
  asmodel_json_value *rid;
  int has_id;

  if (asmodel_json_typeof(req) == ASMODEL_JSON_ARRAY) {
    send_error(1, NULL, -32600, "batch requests are not supported", NULL);
    return;
  }
  if (asmodel_json_typeof(req) != ASMODEL_JSON_OBJECT) {
    send_error(1, NULL, -32600, "request must be a JSON object", NULL);
    return;
  }
  idv = asmodel_json_object_get(req, "id");
  has_id = (idv != NULL);
  ver = asmodel_json_object_get(req, "jsonrpc");
  methv = asmodel_json_object_get(req, "method");
  if (!ver || asmodel_json_typeof(ver) != ASMODEL_JSON_STRING ||
      strcmp(asmodel_json_string_value(ver), "2.0") != 0 || !methv ||
      asmodel_json_typeof(methv) != ASMODEL_JSON_STRING) {
    send_error(has_id, asmodel_json_clone(idv), -32600,
               "invalid JSON-RPC 2.0 request", NULL);
    return;
  }
  method = asmodel_json_string_value(methv);
  {
    const asmodel_json_value *params = asmodel_json_object_get(req, "params");
    const asmodel_json_value *meta = params && asmodel_json_typeof(params) == ASMODEL_JSON_OBJECT
                               ? asmodel_json_object_get(params, "_meta")
                               : NULL;
    protocolv = meta && asmodel_json_typeof(meta) == ASMODEL_JSON_OBJECT
                    ? asmodel_json_object_get(meta,
                        "io.modelcontextprotocol/protocolVersion")
                    : NULL;
    mcp_modern_response = protocolv && asmodel_json_typeof(protocolv) == ASMODEL_JSON_STRING &&
                          strcmp(asmodel_json_string_value(protocolv),
                                 MCP_MODERN_VERSION) == 0;
  }
  /* "notifications/..." methods are ignored only as true notifications
   * (no id); an id-carrying request must get a response, so it falls
   * through to the normal dispatch (=> -32601 when unknown). */
  if (!has_id && strncmp(method, "notifications/", 14) == 0) return;

  rid = asmodel_json_clone(idv); /* NULL (=> null id) when absent or on OOM */
  if (protocolv && !mcp_modern_response &&
      strcmp(method, "server/discover") != 0) {
    send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
    return;
  }
  if (strcmp(method, "initialize") == 0) {
    const asmodel_json_value *params = asmodel_json_object_get(req, "params");
    const asmodel_json_value *pv = params && asmodel_json_typeof(params) == ASMODEL_JSON_OBJECT
                             ? asmodel_json_object_get(params, "protocolVersion")
                             : NULL;
    mcp_modern_response = 0;
    if (pv && (asmodel_json_typeof(pv) != ASMODEL_JSON_STRING ||
               strcmp(asmodel_json_string_value(pv), MCP_LEGACY_VERSION) != 0)) {
      send_error(has_id, rid, -32022, "unsupported MCP protocol version", NULL);
      return;
    }
    asmodel_json_value *res = initialize_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res);
    return;
  }
  if (strcmp(method, "server/discover") == 0) {
    asmodel_json_value *res;
    mcp_modern_response = 1;
    res = discover_result();
    if (!res) send_error(has_id, rid, -32603, "out of memory", NULL);
    else send_result(has_id, rid, res);
    return;
  }
  if (strcmp(method, "ping") == 0) {
    send_result(has_id, rid, asmodel_json_object());
    return;
  }
  if (strcmp(method, "tools/list") == 0) {
    handle_tools_list(has_id, rid);
    return;
  }
  if (strcmp(method, "tools/call") == 0) {
    handle_tools_call(st, has_id, rid, asmodel_json_object_get(req, "params"));
    return;
  }
  send_error(has_id, rid, -32601, "method not found", NULL);
}

/* ═══════════════════════ stdin line reader ═══════════════════════ */

/* 1 = line read (*out malloc'd, trailing \n/\r stripped), 0 = EOF with no
 * data, -1 = allocation failure, -2 = request limit exceeded. */
static int read_line(FILE *f, char **out, size_t *out_len) {
  size_t cap = 256, n = 0;
  char *buf = malloc(cap);
  int ch;
  if (!buf) {
    while ((ch = getc(f)) != EOF && ch != '\n') { /* drain */ }
    return -1;
  }
  while ((ch = getc(f)) != EOF && ch != '\n') {
    if (n == MCP_REQUEST_BYTES) { free(buf); return -2; }
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
  memset(&st, 0, sizeof st);
  memset(&params, 0, sizeof params);
  params.engine_root = root;
  params.config_path = config;
  params.workspace_root = workspace;
  params.allow_degraded = allow_degraded;
  e = asngn_open(&params, &st.ctx);
  if (e != ASNGN_OK || !st.ctx) {
    fprintf(stderr, "asngn-mcp: cannot open engine%s%s%s: %s\n",
            root != NULL ? " at \"" : "",
            root != NULL ? root : "",
            root != NULL ? "\"" : "", asngn_err_name(e));
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
    if (rst == -2) {
      send_error(1, NULL, -32600, "request exceeds 8 MiB; connection closed", NULL);
      break;
    }
    if (rst < 0) {
      emit_line(OOM_RESPONSE);
      continue;
    }
    if (line_blank(line, llen)) {
      free(line);
      continue;
    }
    {
      asmodel_json_value *req = NULL;
      if (asmodel_json_parse(line, llen, &req) != 0) {
        send_error(1, NULL, -32700, "parse error", NULL);
      } else {
        handle_request(&st, req);
        asmodel_json_free(req);
      }
    }
    free(line);
  }

  for (size_t job_index = 0; job_index < 32; job_index++)
    mcp_job_free(st.jobs[job_index]);
  while (st.sessions_n > 0) {
    asngn_session_close(st.sessions[--st.sessions_n]);
  }
  asngn_close(st.ctx);
  return 0;
}
