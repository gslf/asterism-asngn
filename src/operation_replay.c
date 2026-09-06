/* Stream checked records; retain only identity and settlement state for replay. */
#include "operation_record.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  char id[37];
  uint8_t identity[32];
  int64_t day, reserved;
  bool settled;
} reservation;
typedef struct {
  reservation *table;
  size_t cap, count;
  asngn_consumption usage;
} replay;

static size_t slot_for(reservation *table, size_t cap, const char *id) {
  size_t h = 2166136261u;
  for (const char *p = id; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
  h &= cap - 1;
  while (table[h].id[0] && strcmp(table[h].id,id)) h = (h + 1) & (cap - 1);
  return h;
}
static asngn_err grow(replay *p) {
  size_t cap = p->cap ? p->cap * 2 : 256;
  reservation *table = calloc(cap,sizeof *table);
  if (!table) return ASNGN_ERR_NOMEM;
  for (size_t i = 0; i < p->cap; i++) if (p->table[i].id[0])
    table[slot_for(table,cap,p->table[i].id)] = p->table[i];
  free(p->table); p->table = table; p->cap = cap; return ASNGN_OK;
}
static asngn_err observe(void *ud, const char *text, size_t bytes) {
  replay *p = ud;
  asngn_operation_record row;
  asngn_err e = asngn_operation_decode(text,bytes,&row);
  if (e != ASNGN_OK) return e;
  if (row.reserved && p->count >= ASNGN_OPERATIONS_MAX) return ASNGN_ERR_LIMIT;
  if (row.reserved && p->count >= p->cap/2) {
    e = grow(p); if (e != ASNGN_OK) return e;
  }
  if (!p->cap) return ASNGN_ERR_PARSE;
  reservation *r = &p->table[slot_for(p->table,p->cap,row.id)];
  if (row.reserved) {
    if (r->id[0] || row.delta < 0 || row.known || row.input || row.output) return ASNGN_ERR_PARSE;
    strcpy(r->id,row.id); memcpy(r->identity,row.identity,sizeof r->identity);
    r->day = row.day; r->reserved = row.delta; p->count++;
  } else {
    if (!r->id[0] || r->settled || r->day != row.day ||
        memcmp(r->identity,row.identity,sizeof r->identity) ||
        row.delta != (row.known ? row.input + row.output - r->reserved : 0)) return ASNGN_ERR_PARSE;
    r->settled = true;
  }
  if (!asngn_consumption_observe(&p->usage,&row,r->reserved)) return ASNGN_ERR_PARSE;
  return ASNGN_OK;
}

asngn_err asngn_operations_load(asngn_ctx *c) {
  replay p = {0};
  p.usage.utc_day = asngn_clock_now(&c->clock)/86400;
  char *path = os_path_join(c->root,"operations.xcdn");
  asngn_err e = path ? asngn_wal_visit(c,path,4096,observe,&p) : ASNGN_ERR_NOMEM;
  if (e == ASNGN_OK) c->consumption = p.usage;
  c->usage_recovery_required = e != ASNGN_OK;
  free(p.table); free(path); return e;
}
