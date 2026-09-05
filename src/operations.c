/* Inference consumption survives conversation rollback. Unknown outcomes keep
 * their reservation; a later known usage record reconciles only that operation. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <stdlib.h>
#include <string.h>

static asngn_err record(asngn_ctx *c, const asngn_operation *op,
                        const char *state, int64_t delta, int ti, int to,
                        bool known, asngn_err outcome) {
  xcdn_value_t *v = xcdn_value_object();
  xcdn_node_t *node;
  asngn_buf b;
  asngn_stream st;
  asngn_err e;
  char *path = os_path_join(c->root, "operations.xcdn");
  if (!v || !path) { xcdn_value_free(v); free(path); return ASNGN_ERR_NOMEM; }
  if (!asngn_xobj_put(v, "schema", xcdn_value_int(1)) ||
      !asngn_xobj_put(v, "id", xcdn_value_string(op->id)) ||
      !asngn_xobj_put(v, "state", xcdn_value_string(state)) ||
      !asngn_xobj_put(v, "model", xcdn_value_string(op->model)) ||
      !asngn_xobj_put(v, "kind", xcdn_value_string(op->kind)) ||
      !asngn_xobj_put(v, "day", xcdn_value_int(op->day)) ||
      !asngn_xobj_put(v, "budget_delta", xcdn_value_int(delta)) ||
      !asngn_xobj_put(v, "input_tokens", xcdn_value_int(ti)) ||
      !asngn_xobj_put(v, "output_tokens", xcdn_value_int(to)) ||
      !asngn_xobj_put(v, "usage_known", xcdn_value_bool(known)) ||
      !asngn_xobj_put(v, "outcome", xcdn_value_string(asngn_err_name(outcome)))) {
    xcdn_value_free(v); free(path); return ASNGN_ERR_NOMEM;
  }
  node = xcdn_node_new(v);
  if (!node) { xcdn_value_free(v); free(path); return ASNGN_ERR_NOMEM; }
  asngn_buf_init(&b);
  e = asngn_xnode_write(node, false, &b);
  xcdn_node_free(node);
  if (e == ASNGN_OK) e = asngn_stream_open(c, &st, path, true);
  if (e == ASNGN_OK) {
    e = asngn_wal_append(c, &st, b.data, b.len);
    asngn_stream_close(&st);
  }
  asngn_buf_free(&b);
  free(path);
  if (e == ASNGN_OK && op->day == c->daily_day) c->daily_spent += delta;
  if (e != ASNGN_OK) c->usage_recovery_required = true;
  return e;
}

asngn_err asngn_operation_begin(asngn_ctx *c, const char *model,
                                 const char *kind, int64_t reserve,
                                 asngn_operation *op) {
  asngn_err e;
  c = c->owner ? c->owner : c;
  memset(op, 0, sizeof *op);
  asngn_uuid_v4(op->id);
  op->model = model; op->kind = kind;
  op->reserved = reserve > 0 ? reserve : 0;
  op->day = asngn_clock_now(&c->clock) / 86400;
  os_rwlock_wrlock(&c->lock);
  if (op->day > c->daily_day) { c->daily_day = op->day; c->daily_spent = 0; }
  if (c->usage_recovery_required) e = ASNGN_ERR_IO;
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
  if (!op || !op->active || ti < 0 || to < 0) return ASNGN_ERR_INVALID;
  int64_t delta = known ? (int64_t)ti + to - op->reserved : 0;
  c = c->owner ? c->owner : c;
  os_rwlock_wrlock(&c->lock);
  op->active = false;
  e = record(c, op, "settled", delta, ti, to, known, outcome);
  os_rwlock_wrunlock(&c->lock);
  return e;
}
