/* Extractive views retain byte ranges in the exact, redacted source object. */
#ifndef ASNGN_EXCERPT_H
#define ASNGN_EXCERPT_H
#include "asngn_internal.h"
#define ASNGN_EXCERPT_SPANS 8
typedef struct {
  size_t start, end;
  const char *reason;
} asngn_excerpt_span;
typedef struct {
  char *text;
  size_t matches, count, selected_bytes;
  asngn_excerpt_span spans[ASNGN_EXCERPT_SPANS];
} asngn_excerpt;
asngn_err asngn_excerpt_build(const char *text, size_t len, size_t budget, asngn_excerpt *out);
void asngn_excerpt_free(asngn_excerpt *view);
/* Owned JSON metadata for telemetry; no original or summarized text. */
char *asngn_excerpt_trace(const asngn_excerpt *view, const char *source, size_t bytes,
                          size_t budget, bool compressor_used);
#endif
