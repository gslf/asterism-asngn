/* One reducer serves durable replay and live admission. Callers validate
 * operation identities before applying a settlement and publish only on sync. */
#include "operation_record.h"
#include <string.h>

static bool add(int64_t *value, int64_t delta) {
  if ((delta > 0 && *value > INT64_MAX - delta) || (delta < 0 && (*value < 0 || delta < -*value)))
    return false;
  *value += delta;
  return true;
}

static bool observe(asngn_consumption_totals *v, const asngn_operation_record *r,
                    int64_t reserved) {
  if (reserved < 0 || !add(&v->charged_tokens, r->delta))
    return false;
  if (r->reserved)
    return add(&v->calls, 1) && add(&v->unsettled_calls, 1) && add(&v->unsettled_tokens, reserved);
  if (!add(&v->unsettled_calls, -1) || !add(&v->unsettled_tokens, -reserved))
    return false;
  if (r->known) {
    if (!add(&v->known_input_tokens, r->input) || !add(&v->known_output_tokens, r->output))
      return false;
  } else if (!add(&v->unknown_calls, 1) || !add(&v->unknown_tokens, reserved))
    return false;
  if (r->outcome == ASNGN_ERR_CANCELLED)
    return add(&v->cancelled_calls, 1);
  return r->outcome == ASNGN_OK || add(&v->failed_calls, 1);
}

void asngn_consumption_roll(asngn_consumption *usage, int64_t day) {
  if (day > usage->utc_day) {
    usage->utc_day = day;
    memset(&usage->today, 0, sizeof usage->today);
  }
}

bool asngn_consumption_observe(asngn_consumption *usage, const asngn_operation_record *row,
                               int64_t reserved) {
  asngn_consumption next = *usage;
  asngn_consumption_roll(&next, row->day);
  if (!observe(&next.lifetime, row, reserved) ||
      (row->day == next.utc_day && !observe(&next.today, row, reserved)))
    return false;
  *usage = next;
  return true;
}

asngn_err asngn_get_consumption(asngn_ctx *c, asngn_consumption *out) {
  if (!out)
    return ASNGN_ERR_INVALID;
  memset(out, 0, sizeof *out);
  if (!c)
    return ASNGN_ERR_INVALID;
  c = c->owner ? c->owner : c;
  os_rwlock_wrlock(&c->lock);
  asngn_err e = c->usage_recovery_required ? ASNGN_ERR_IO : ASNGN_OK;
  if (e == ASNGN_OK) {
    asngn_consumption_roll(&c->consumption, asngn_clock_now(&c->clock) / 86400);
    *out = c->consumption;
  }
  os_rwlock_wrunlock(&c->lock);
  return e;
}

int64_t asngn_daily_spend(asngn_ctx *c) {
  asngn_consumption usage;
  return asngn_get_consumption(c, &usage) == ASNGN_OK ? usage.today.charged_tokens : INT64_MAX;
}
