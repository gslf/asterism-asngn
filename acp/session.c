/* A stdio host serves one operator-selected workspace and bounded live sessions. */
#include "server.h"
#include <stdlib.h>
#include <string.h>

acp_session *acp_session_find(acp_server *s, const char *id) {
  if (id)
    for (size_t i = 0; i < ACP_SESSIONS; i++) {
      acp_session *session = &s->sessions[i];
      if (session->session && !strcmp(id, asngn_session_slug(session->session)))
        return session;
    }
  return NULL;
}
void acp_session_drop(acp_session *session) {
  mcp_job_free(session->job);
  asngn_session_close(session->session);
  asmodel_json_free(session->prompt_id);
  asmodel_json_free(session->close_id);
  asmodel_json_free(session->terminal);
  memset(session, 0, sizeof *session);
}
static bool empty_array(const acp_json *params, const char *key, bool optional) {
  const acp_json *v = asmodel_json_object_get(params, key);
  return (!v && optional) ||
         (asmodel_json_typeof(v) == ASMODEL_JSON_ARRAY && !asmodel_json_array_len(v));
}
void acp_session_new(acp_server *s, const acp_json *id, const acp_json *params) {
  const char *cwd = acp_string(params, "cwd");
  asngn_workspace_info workspace;
  char *canonical = cwd && cwd[0] == '/' ? os_realpath(cwd) : NULL;
  bool allowed = canonical && asngn_workspace_get(s->engine, &workspace) == ASNGN_OK &&
                 !strcmp(canonical, workspace.canonical_root);
  free(canonical);
  if (!allowed || !empty_array(params, "mcpServers", false) ||
      !empty_array(params, "additionalDirectories", true)) {
    acp_error(
        s, id, -32602,
        "Use the configured workspace, no additional directories, and an empty mcpServers list",
        ASNGN_ERR_DENIED);
    return;
  }
  acp_session *session = NULL;
  for (size_t i = 0; i < ACP_SESSIONS; i++)
    if (!s->sessions[i].session) {
      session = &s->sessions[i];
      break;
    }
  if (!session) {
    acp_error(s, id, -32000, "Live session limit reached", ASNGN_ERR_LIMIT);
    return;
  }
  asngn_err e = asngn_session_open(s->engine, NULL, &session->session);
  if (e != ASNGN_OK) {
    acp_error(s, id, -32000, "Cannot open session", e);
    return;
  }
  acp_json *result = asmodel_json_object();
  if (asmodel_json_object_set(result, "sessionId",
                              asmodel_json_string(asngn_session_slug(session->session)))) {
    asmodel_json_free(result);
    result = NULL;
  }
  acp_reply(s, id, result);
}

static asngn_err prompt_text(const acp_json *blocks, char **out) {
  *out = NULL;
  size_t count = asmodel_json_array_len(blocks);
  if (!count || count > 64)
    return ASNGN_ERR_INVALID;
  asngn_buf text = {0};
  asngn_err e = ASNGN_OK;
  for (size_t i = 0; e == ASNGN_OK && i < count; i++) {
    const acp_json *block = asmodel_json_array_at(blocks, i);
    const char *type = acp_string(block, "type"), *value = NULL, *name = NULL;
    if (type && !strcmp(type, "text"))
      value = acp_string(block, "text");
    else if (type && !strcmp(type, "resource_link")) {
      value = acp_string(block, "uri");
      name = acp_string(block, "name");
      if (!name || !*name)
        value = NULL;
    }
    if (!value) {
      e = ASNGN_ERR_INVALID;
      break;
    }
    size_t bytes = strlen(value) + (name ? strlen(name) : 0);
    if (bytes > 256u * 1024u || text.len + bytes + 64 > 256u * 1024u) {
      e = ASNGN_ERR_LIMIT;
      break;
    }
    if (i)
      e = asngn_buf_appends(&text, "\n\n");
    if (e == ASNGN_OK && name)
      e = asngn_buf_printf(&text, "Resource reference (not opened): %s\nURI: ", name);
    if (e == ASNGN_OK)
      e = asngn_buf_appends(&text, value);
  }
  if (e == ASNGN_OK && text.len)
    *out = asngn_buf_detach(&text);
  else if (e == ASNGN_OK)
    e = ASNGN_ERR_INVALID;
  asngn_buf_free(&text);
  return e;
}
void acp_prompt(acp_server *s, acp_session *session, const acp_json *id, const acp_json *params) {
  if (session->job) {
    acp_error(s, id, -32000, "Session already has an active prompt", ASNGN_ERR_BUSY);
    return;
  }
  char *text = NULL;
  asngn_err e = prompt_text(asmodel_json_object_get(params, "prompt"), &text);
  if (e == ASNGN_OK) {
    session->prompt_id = asmodel_json_clone(id);
    if (!session->prompt_id)
      e = ASNGN_ERR_NOMEM;
  }
  if (e == ASNGN_OK)
    e = mcp_job_submit(session->session, text, &session->job);
  free(text);
  if (e != ASNGN_OK) {
    asmodel_json_free(session->prompt_id);
    session->prompt_id = NULL;
    acp_error(s, id, e == ASNGN_ERR_INVALID ? -32602 : -32000,
              "Cannot admit prompt; only bounded text and resource links are supported", e);
    return;
  }
  session->cursor = session->output_bytes = 0;
  session->hold_output = session->cancelled = false;
  asngn_sha256_init(&session->output_hash);
}
