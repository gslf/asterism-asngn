/* Inference consumption survives conversation rollback. Unknown outcomes keep
 * their reservation; a later known usage record reconciles only that operation. */
#include "operation_record.h"
#include <stdlib.h>
#include <string.h>

static asngn_err record(asngn_ctx *c, const asngn_operation *op,
                        const char *state, int64_t delta, int ti, int to,
                        bool known, asngn_err outcome) {
  asngn_stream st;
  char *path = os_path_join(c->root, "operations.xcdn");
  char *text = asngn_operation_encode(op,state,delta,ti,to,known,outcome);
  asngn_err e = path && text ? ASNGN_OK : ASNGN_ERR_NOMEM;
  if (e == ASNGN_OK) e = asngn_stream_open(c, &st, path, true);
  if (e == ASNGN_OK) {
    e = asngn_wal_append(c, &st, text, strlen(text));
    asngn_stream_close(&st);
  }
  free(text); free(path);
  if (e == ASNGN_OK && op->day == c->daily_day) c->daily_spent += delta;
  if (e != ASNGN_OK) c->usage_recovery_required = true;
  return e;
}

asngn_err asngn_operation_begin(asngn_ctx *c, const char *model,
                                 const char *kind, const char *request_id, int64_t reserve,
                                 asngn_operation *op) {
  asngn_err e;
  if (!c || !op || !model || !*model || strlen(model) > ASMODEL_ID_MAX ||
      !kind || !*kind || strlen(kind) > 32 || reserve < 0 ||
      !asngn_utf8_valid(model,strlen(model)) || !asngn_utf8_valid(kind,strlen(kind)) ||
      (request_id && (!*request_id || strlen(request_id) > ASMODEL_REQUEST_ID_MAX ||
                     !asngn_utf8_valid(request_id,strlen(request_id)))))
    return ASNGN_ERR_INVALID;
  c = c->owner ? c->owner : c;
  memset(op, 0, sizeof *op);
  asngn_uuid_v4(op->id);
  if (request_id) strcpy(op->request_id,request_id);
  op->model = model; op->kind = kind;
  op->reserved = reserve;
  op->day = asngn_clock_now(&c->clock) / 86400;
  os_rwlock_wrlock(&c->lock);
  if (op->day > c->daily_day) { c->daily_day = op->day; c->daily_spent = 0; }
  if (c->usage_recovery_required) e = ASNGN_ERR_IO;
  else if (op->reserved > INT64_MAX-c->daily_spent) e = ASNGN_ERR_LIMIT;
  else if (c->cfg.daily_tokens > 0 && op->reserved > c->cfg.daily_tokens - c->daily_spent)
    e = ASNGN_ERR_LIMIT;
  else e = record(c, op, "reserved", op->reserved, 0, 0, false, ASNGN_OK);
  op->active = e == ASNGN_OK;
  os_rwlock_wrunlock(&c->lock);
  return e;
}

asngn_err asngn_operation_end(asngn_ctx *c, asngn_operation *op,
                               int ti, int to, bool known, asngn_err outcome) {
  asngn_err e;
  if (!c || !op || !op->active || ti < 0 || to < 0 ||
      outcome < ASNGN_OK || outcome > ASNGN_ERR_LIMIT) return ASNGN_ERR_INVALID;
  int64_t delta = known ? (int64_t)ti + to - op->reserved : 0;
  c = c->owner ? c->owner : c;
  os_rwlock_wrlock(&c->lock);
  op->active = false;
  if (c->usage_recovery_required) e = ASNGN_ERR_IO;
  else if (op->day == c->daily_day && delta > 0 && c->daily_spent > INT64_MAX-delta) {
    c->usage_recovery_required = true; e = ASNGN_ERR_LIMIT;
  } else e = record(c, op, "settled", delta, ti, to, known, outcome);
  os_rwlock_wrunlock(&c->lock);
  return e;
}
