/*
 * tokens.c — token estimation heuristic: UTF-8 bytes / 4, minimum 1.
 *
 * Used when no tokenizer is available (degraded contexts only); the model
 * runtime prefers the real tokenizer of the resolved slot.
 */

#include "asngn_internal.h"

#include <limits.h>
#include <string.h>

int asngn_token_heuristic(const char *text) {
  size_t len, t;
  if (!text || text[0] == '\0') return 1;
  len = strlen(text);
  t = len / 4;
  if (t == 0) t = 1;
  if (t > (size_t)INT_MAX) t = (size_t)INT_MAX;
  return (int)t;
}
