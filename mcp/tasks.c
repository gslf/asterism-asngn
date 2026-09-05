/* Bounded event retention. Cursor gaps are explicit; polling never waits for
 * an entire turn, and only the main thread writes JSON-RPC responses. */
#include "tasks.h"
#include "os.h"
#include <stdlib.h>
#include <string.h>
#define EVENTS 256
#define EVENT_BYTES 4096

struct mcp_job {
  asngn_task *task;
  asngn_turn_result result;
  asngn_err outcome;
  int done;
  os_mutex mu;
  unsigned long long next;
  struct { char *text; asngn_stream_kind kind; int truncated; } events[EVENTS];
};

static void event(const asngn_stream_event *ev, void *ud) {
  mcp_job *j = ud;
  size_t n = strlen(ev->text), original = n;
  char *text;
  if (n > EVENT_BYTES) {
    n = EVENT_BYTES;
    while (n && ((unsigned char)ev->text[n] & 0xc0) == 0x80) n--;
  }
  text = malloc(n + 1);
  if (text) { memcpy(text, ev->text, n); text[n] = 0; }
  os_mutex_lock(&j->mu);
  size_t slot = (size_t)(j->next++ % EVENTS);
  free(j->events[slot].text);
  j->events[slot].text = text;
  j->events[slot].kind = ev->kind;
  j->events[slot].truncated = original != n || !text;
  os_mutex_unlock(&j->mu);
}

asngn_err mcp_job_submit(asngn_session *session, const char *message, mcp_job **out) {
  mcp_job *j = calloc(1, sizeof *j);
  asngn_err e;
  *out = NULL;
  if (!j) return ASNGN_ERR_NOMEM;
  os_mutex_init(&j->mu);
  e = asngn_submit_stream(session, message, NULL, event, j, &j->task);
  if (e != ASNGN_OK) { os_mutex_destroy(&j->mu); free(j); return e; }
  *out = j;
  return ASNGN_OK;
}
const char *mcp_job_id(const mcp_job *j) { return asngn_task_id(j->task); }
asngn_err mcp_job_cancel(mcp_job *j) { return asngn_task_cancel(j->task); }

asngn_err mcp_job_poll(mcp_job *j, unsigned long long cursor, jx_value **out) {
  jx_value *o = jx_object(), *events = jx_array();
  int ok = o && events;
  *out = NULL;
  if (!j->done) {
    j->outcome = asngn_task_wait(j->task, 1, &j->result);
    j->done = j->outcome != ASNGN_ERR_BUSY;
  }
  os_mutex_lock(&j->mu);
  unsigned long long first = j->next > EVENTS ? j->next - EVENTS : 0;
  if (cursor > j->next) { os_mutex_unlock(&j->mu); jx_free(o); jx_free(events); return ASNGN_ERR_INVALID; }
  ok &= jx_object_set(o, "cursor_gap", jx_bool(cursor < first)) == 0;
  if (cursor < first) cursor = first;
  for (; ok && cursor < j->next; cursor++) {
    size_t i = (size_t)(cursor % EVENTS);
    jx_value *v = jx_object();
    ok = v != NULL;
    ok &= jx_object_set(v, "cursor", jx_int((long long)cursor)) == 0;
    ok &= jx_object_set(v, "kind", jx_int(j->events[i].kind)) == 0;
    ok &= jx_object_set(v, "text", jx_string(j->events[i].text ? j->events[i].text : "")) == 0;
    ok &= jx_object_set(v, "truncated", jx_bool(j->events[i].truncated)) == 0;
    ok &= jx_array_push(events, v) == 0;
  }
  ok &= jx_object_set(o, "next_cursor", jx_int((long long)j->next)) == 0;
  os_mutex_unlock(&j->mu);
  ok &= jx_object_set(o, "events", events) == 0;
  ok &= jx_object_set(o, "done", jx_bool(j->done)) == 0;
  ok &= jx_object_set(o, "task_id", jx_string(mcp_job_id(j))) == 0;
  if (j->done) {
    ok &= jx_object_set(o, "outcome", jx_string(asngn_err_name(j->outcome))) == 0;
    ok &= jx_object_set(o, "answer", jx_string(j->result.answer ? j->result.answer : "")) == 0;
    ok &= jx_object_set(o, "task_state", jx_string("unconfirmed")) == 0;
  }
  if (!ok) { jx_free(o); return ASNGN_ERR_NOMEM; }
  *out = o;
  return ASNGN_OK;
}

void mcp_job_free(mcp_job *j) {
  if (!j) return;
  if (!j->done) (void)asngn_task_cancel(j->task);
  asngn_task_free(j->task); /* joins before callback storage is released */
  asngn_turn_result_free(&j->result);
  for (size_t i = 0; i < EVENTS; i++) free(j->events[i].text);
  os_mutex_destroy(&j->mu);
  free(j);
}
