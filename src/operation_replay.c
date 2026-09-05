/* Reconcile usage by operation identity, not by blindly summing log deltas. */
#include "asngn_internal.h"
#include "xcdn.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *id;
  int64_t day, reserved;
  bool settled;
} reservation;

static size_t slot_for(reservation *table, size_t cap, const char *id) {
  size_t h = 2166136261u;
  for (const char *p = id; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
  h &= cap - 1;
  while (table[h].id && strcmp(table[h].id, id)) h = (h + 1) & (cap - 1);
  return h;
}

asngn_err asngn_operations_load(asngn_ctx *c) {
  xcdn_document_t *doc = NULL;
  char *path = os_path_join(c->root, "operations.xcdn");
  reservation *table = NULL;
  size_t cap = 1;
  asngn_err e;
  if (!path) return ASNGN_ERR_NOMEM;
  e = asngn_wal_load(c, path, &doc);
  free(path);
  c->daily_day = asngn_clock_now(&c->clock) / 86400;
  c->daily_spent = 0;
  if (e != ASNGN_OK || !doc) goto done;
  if (doc->values_len > 262144) { e = ASNGN_ERR_LIMIT; goto done; }
  while (cap < doc->values_len * 2 + 1) cap *= 2;
  table = calloc(cap, sizeof *table);
  if (!table) { e = ASNGN_ERR_NOMEM; goto done; }
  for (size_t i = 0; i < doc->values_len; i++) {
    int64_t day, delta, schema, ti, to;
    bool known;
    const xcdn_value_t *v = doc->values[i]->value;
    const char *id = asngn_xstr(asngn_xfield(v, "id"));
    const char *state = asngn_xstr(asngn_xfield(v, "state"));
    if (!id || !asngn_uuid_valid(id) || !state ||
        !asngn_xint(asngn_xfield(v, "schema"), &schema) || schema != 1 ||
        !asngn_xint(asngn_xfield(v, "day"), &day) ||
        !asngn_xint(asngn_xfield(v, "budget_delta"), &delta) ||
        !asngn_xint(asngn_xfield(v, "input_tokens"), &ti) || ti < 0 || ti > INT_MAX ||
        !asngn_xint(asngn_xfield(v, "output_tokens"), &to) || to < 0 || to > INT_MAX ||
        !asngn_xbool(asngn_xfield(v, "usage_known"), &known)) {
      e = ASNGN_ERR_PARSE; break;
    }
    reservation *r = &table[slot_for(table, cap, id)];
    if (!strcmp(state, "reserved")) {
      if (r->id || delta < 0 || known || ti || to) { e = ASNGN_ERR_PARSE; break; }
      r->id = id; r->day = day; r->reserved = delta;
    } else if (!strcmp(state, "settled")) {
      if (!r->id || r->settled || r->day != day ||
          delta != (known ? ti + to - r->reserved : 0)) { e = ASNGN_ERR_PARSE; break; }
      r->settled = true;
    } else { e = ASNGN_ERR_PARSE; break; }
    if (day == c->daily_day) {
      if ((delta > 0 && c->daily_spent > INT64_MAX - delta) ||
          (delta < 0 && c->daily_spent < -delta)) { e = ASNGN_ERR_PARSE; break; }
      c->daily_spent += delta;
    }
  }
done:
  free(table);
  xcdn_document_free(doc);
  c->usage_recovery_required = e != ASNGN_OK;
  return e;
}
