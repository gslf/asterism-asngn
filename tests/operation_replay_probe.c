/* Component benchmark probe. Use only evaluator-owned synthetic stores. */
#include "asngn_internal.h"
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  asngn_ctx *c = calloc(1,sizeof *c);
  if (!c) return 2;
  c->root = asngn_strdup(argv[1]);
  os_rwlock_init(&c->lock); os_mutex_init(&c->err_mu);
  asngn_err e = c->root ? asngn_operations_load(c) : ASNGN_ERR_NOMEM;
  printf("{\"outcome\":\"%s\",\"daily_spent\":%lld}\n",asngn_err_name(e),(long long)c->consumption.today.charged_tokens);
  os_mutex_destroy(&c->err_mu); os_rwlock_destroy(&c->lock);
  free(c->root); free(c);
  return e != ASNGN_OK;
}
