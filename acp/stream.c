/* Reconcile accepted stream prefixes with the complete terminal answer. */
#include "server.h"
#include <stdlib.h>
#include <string.h>

static void chunk(acp_server *s, acp_session *session, const char *text, size_t length) {
  char *part = asngn_strndup(text, length);
  acp_json *update = acp_literal("{\"sessionUpdate\":\"agent_message_chunk\","
                                 "\"content\":{\"type\":\"text\"}}");
  int bad = asmodel_json_object_set(asmodel_json_object_get(update, "content"), "text",
                                    part ? asmodel_json_string(part) : NULL);
  bad |=
      asmodel_json_object_set(update, "messageId", asmodel_json_string(mcp_job_id(session->job)));
  free(part);
  if (bad) {
    asmodel_json_free(update);
    update = NULL;
  }
  acp_update(s, session, update);
  if (s->failure == ASNGN_OK) {
    asngn_sha256_update(&session->output_hash, text, length);
    session->output_bytes += length;
  }
}
static void finish(acp_server *s, acp_session *session) {
  const char *outcome = acp_string(session->terminal, "outcome");
  bool cancelled = session->cancelled || (outcome && !strcmp(outcome, "ASNGN_ERR_CANCELLED"));
  acp_tool_finish(s, session, false, cancelled ? "cancelled" : "action_not_dispatched");
  if (outcome && (!strcmp(outcome, "ASNGN_OK") || cancelled)) {
    acp_json *result = asmodel_json_object(), *meta = asmodel_json_object();
    acp_json *status = asmodel_json_object();
    int bad = asmodel_json_object_set(result, "stopReason",
                                      asmodel_json_string(cancelled ? "cancelled" : "end_turn"));
    const char *keys[] = {"task_id", "outcome", "task_state", "work_revision", "work_sequence"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
      const acp_json *v = asmodel_json_object_get(session->terminal, keys[i]);
      if (v)
        bad |= asmodel_json_object_set(status, keys[i], asmodel_json_clone(v));
    }
    bad |= asmodel_json_object_set(meta, "dev.asterism/asngn", status);
    bad |= asmodel_json_object_set(result, "_meta", meta);
    if (bad) {
      asmodel_json_free(result);
      result = NULL;
    }
    acp_reply(s, session->prompt_id, result);
  } else {
    asngn_err error = ASNGN_ERR_IO;
    /* Preserve the public engine outcome without inventing ACP stop reasons. */
    for (int i = ASNGN_OK; i <= ASNGN_ERR_LIMIT; i++)
      if (outcome && !strcmp(outcome, asngn_err_name((asngn_err)i)))
        error = (asngn_err)i;
    acp_error(s, session->prompt_id, -32000, "Prompt failed", error);
  }
  mcp_job_free(session->job);
  session->job = NULL;
  asmodel_json_free(session->prompt_id);
  session->prompt_id = NULL;
  asmodel_json_free(session->terminal);
  session->terminal = NULL;
  session->final_text = NULL;
  session->awaiting_permission = false;
  session->permission[0] = 0;
  if (session->close_id) {
    acp_reply(s, session->close_id, asmodel_json_object());
    acp_session_drop(session);
  }
}
static void terminal(acp_server *s, acp_session *session) {
  const char *answer = session->final_text;
  size_t length = session->final_length;
  if (!answer || session->final_offset > length) {
    s->failure = ASNGN_ERR_PARSE;
    return;
  }
  size_t sent = 0;
  while (s->failure == ASNGN_OK && session->final_offset < length && sent < 65536) {
    size_t n = length - session->final_offset;
    if (n > 16384)
      n = 16384;
    while (n && ((unsigned char)answer[session->final_offset + n] & 0xc0) == 0x80)
      n--;
    if (!n) {
      s->failure = ASNGN_ERR_PARSE;
      return;
    }
    chunk(s, session, answer + session->final_offset, n);
    session->final_offset += n;
    sent += n;
  }
  if (s->failure == ASNGN_OK && session->final_offset == length)
    finish(s, session);
}
static void poll_session(acp_server *s, acp_session *session) {
  if (session->terminal) {
    terminal(s, session);
    return;
  }
  acp_json *packet = NULL;
  asngn_err e = mcp_job_poll(session->job, session->cursor, &packet);
  if (e != ASNGN_OK) {
    s->failure = e;
    return;
  }
  if (asmodel_json_bool_value(asmodel_json_object_get(packet, "cursor_gap")))
    session->hold_output = true;
  const acp_json *events = asmodel_json_object_get(packet, "events");
  size_t count = asmodel_json_array_len(events), consumed = 0;
  for (; consumed < count && consumed < 16 && !session->hold_output; consumed++) {
    const acp_json *event = asmodel_json_array_at(events, consumed);
    session->cursor =
        (unsigned long long)asmodel_json_int_value(asmodel_json_object_get(event, "cursor")) + 1;
    if (asmodel_json_int_value(asmodel_json_object_get(event, "kind")) != ASNGN_STREAM_OUTPUT)
      continue;
    if (asmodel_json_bool_value(asmodel_json_object_get(event, "truncated"))) {
      session->hold_output = true;
      break;
    }
    const char *text = acp_string(event, "text");
    if (!text) {
      s->failure = ASNGN_ERR_PARSE;
      break;
    }
    if (*text)
      chunk(s, session, text, strlen(text));
  }
  if (session->hold_output)
    session->cursor =
        (unsigned long long)asmodel_json_int_value(asmodel_json_object_get(packet, "next_cursor"));
  if (s->failure == ASNGN_OK && (session->hold_output || consumed == count) &&
      asmodel_json_bool_value(asmodel_json_object_get(packet, "done"))) {
    const char *answer = acp_string(packet, "answer");
    size_t length = answer ? strlen(answer) : 0;
    uint8_t expected[32], observed[32];
    asngn_sha256_ctx hash = session->output_hash;
    asngn_sha256_final(&hash, observed);
    if (!answer || length < session->output_bytes)
      s->failure = ASNGN_ERR_PARSE;
    else {
      asngn_sha256(answer, session->output_bytes, expected);
      if (memcmp(expected, observed, 32))
        s->failure = ASNGN_ERR_PARSE;
    }
    if (s->failure == ASNGN_OK) {
      /* A completed worker has delivered every action callback before this drain. */
      acp_events_drain(s);
      session->terminal = packet;
      session->final_text = answer;
      session->final_length = length;
      session->final_offset = session->output_bytes;
      packet = NULL;
      terminal(s, session);
    }
  }
  asmodel_json_free(packet);
}
void acp_tick(acp_server *s) {
  size_t first = s->next_session++ % ACP_SESSIONS;
  for (size_t i = 0; i < ACP_SESSIONS && s->failure == ASNGN_OK; i++) {
    if (s->io.outgoing.len - s->io.written > 256u * 1024u)
      break;
    acp_session *session = &s->sessions[(first + i) % ACP_SESSIONS];
    if (session->job)
      poll_session(s, session);
  }
}
