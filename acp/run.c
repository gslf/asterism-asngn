/* One transport thread owns every write and joins workers before releasing state. */
#include "server.h"
#include <stdlib.h>

int asngn_acp_run(asngn_ctx *engine, int input, int output) {
  acp_server *s = calloc(1, sizeof *s);
  if (!s)
    return 1;
  s->engine = engine;
  if (acp_io_open(&s->io, input, output) != ASNGN_OK) {
    free(s);
    return 1;
  }
  os_mutex_init(&s->events_mutex);
  asngn_set_event_sink(engine, acp_events_receive, s);
  while (s->failure == ASNGN_OK) {
    s->failure = acp_io_pump(&s->io, 20);
    for (int i = 0; i < 16 && s->failure == ASNGN_OK; i++) {
      char *text = NULL;
      size_t length;
      asngn_err e = acp_io_next(&s->io, &text, &length);
      if (e == ASNGN_ERR_BUSY)
        break;
      if (e != ASNGN_OK) {
        s->failure = e;
        break;
      }
      acp_json *request = NULL;
      if (asmodel_json_parse(text, length, &request))
        acp_error(s, NULL, -32700, "Invalid JSON", ASNGN_ERR_PARSE);
      else
        acp_dispatch(s, request);
      asmodel_json_free(request);
      free(text);
    }
    if (s->failure != ASNGN_OK || s->io.eof)
      break;
    acp_events_drain(s);
    acp_tick(s);
  }
  /* Cancel all lanes before joining any one lane. EOF is not permission. */
  for (size_t i = 0; i < ACP_SESSIONS; i++)
    if (s->sessions[i].job)
      (void)mcp_job_cancel(s->sessions[i].job);
  for (size_t i = 0; i < ACP_SESSIONS; i++)
    acp_session_drop(&s->sessions[i]);
  asngn_set_event_sink(engine, NULL, NULL);
  os_mutex_destroy(&s->events_mutex);
  /* Permit an EOF after a complete request to receive its already queued reply. */
  while (s->failure == ASNGN_OK && s->io.outgoing.len > s->io.written)
    s->failure = acp_io_pump(&s->io, 20);
  int status = s->failure == ASNGN_OK ? 0 : 1;
  acp_io_close(&s->io);
  free(s);
  return status;
}
