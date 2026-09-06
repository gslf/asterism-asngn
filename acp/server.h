/* Editor host state. Tool clients never enter this approval channel. */
#ifndef ASNGN_ACP_SERVER_H
#define ASNGN_ACP_SERVER_H
#include "../mcp/tasks.h"
#include "asmodel_json.h"
#include "io.h"
#define ACP_SESSIONS 8
#define ACP_EVENTS 64
typedef asmodel_json_value acp_json;
typedef struct {
  asngn_session *session;
  mcp_job *job;
  acp_json *prompt_id, *close_id;
  acp_json *terminal;
  const char *final_text; /* borrowed from the validated terminal packet */
  size_t final_offset, final_length;
  unsigned long long cursor;
  size_t output_bytes;
  asngn_sha256_ctx output_hash;
  bool hold_output, cancelled, awaiting_permission, tool_open;
  char permission[37], call_id[37];
} acp_session;
typedef struct {
  char session[65], turn[37], action[37], approval[37], tool[128], command[64];
  enum { ACP_CONFIRM, ACP_DISPATCH, ACP_OBSERVED } kind;
  bool ok, journaled;
  char error[64];
} acp_event;
typedef struct {
  asngn_ctx *engine;
  acp_io io;
  acp_session sessions[ACP_SESSIONS];
  bool initialized;
  asngn_err failure;
  os_mutex events_mutex;
  acp_event events[ACP_EVENTS];
  size_t event_head, event_count;
  asngn_err event_error;
  size_t next_session;
} acp_server;

const char *acp_string(const acp_json *object, const char *key);
bool acp_id_valid(const acp_json *id);
bool acp_id_equal(const acp_json *left, const acp_json *right);
acp_json *acp_literal(const char *text);
void acp_reply(acp_server *server, const acp_json *id, acp_json *result);
void acp_error(acp_server *server, const acp_json *id, int code, const char *message,
               asngn_err outcome);
void acp_update(acp_server *server, acp_session *session, acp_json *update);
void acp_permission_request(acp_server *server, const char *id, acp_json *params);
void acp_dispatch(acp_server *server, const acp_json *request);
acp_session *acp_session_find(acp_server *server, const char *id);
void acp_session_new(acp_server *server, const acp_json *id, const acp_json *params);
void acp_prompt(acp_server *server, acp_session *session, const acp_json *id,
                const acp_json *params);
void acp_tool_finish(acp_server *server, acp_session *session, bool ok, const char *reason);
void acp_tick(acp_server *server);
void acp_events_receive(const char *event, void *userdata);
void acp_events_drain(acp_server *server);
void acp_permission_reply(acp_server *server, const acp_json *response);
void acp_session_drop(acp_session *session);
int asngn_acp_run(asngn_ctx *engine, int input, int output);
#endif
