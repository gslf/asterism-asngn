/* Case-sensitive whole identifiers; Unicode bytes stay intact, without stemming. */
#include "retrieval_select.h"
#include <stdlib.h>
#include <string.h>

static bool word(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c >= 128;
}
static int by_term(const void *a, const void *b) { return strcmp(a, b); }
void asngn_code_terms(code_selection *s, const char *query) {
  const char *p = query ? query : "";
  while (*p) {
    while (*p && !word((unsigned char)*p))
      p++;
    const char *start = p;
    while (word((unsigned char)*p))
      p++;
    size_t n = (size_t)(p - start), i;
    if (n < 2)
      continue;
    if (n >= sizeof s->terms[0]) {
      s->terms_truncated = true;
      continue;
    }
    for (i = 0; i < s->term_count; i++)
      if (strlen(s->terms[i]) == n && !memcmp(s->terms[i], start, n))
        break;
    if (i < s->term_count)
      continue;
    if (s->term_count == 64) {
      s->terms_truncated = true;
      continue;
    }
    memcpy(s->terms[s->term_count], start, n);
    s->terms[s->term_count++][n] = 0;
  }
  qsort(s->terms, s->term_count, sizeof s->terms[0], by_term);
}
static uint64_t matches(const code_selection *s, const char *text, size_t length) {
  uint64_t mask = 0;
  size_t pos = 0;
  while (pos < length && s->term_count) {
    while (pos < length && !word((unsigned char)text[pos]))
      pos++;
    size_t start = pos;
    while (pos < length && word((unsigned char)text[pos]))
      pos++;
    size_t n = pos - start;
    if (n < 2 || n >= sizeof s->terms[0])
      continue;
    char term[192];
    memcpy(term, text + start, n);
    term[n] = 0;
    const char (*found)[192] = bsearch(term, s->terms, s->term_count, sizeof s->terms[0], by_term);
    if (found)
      mask |= UINT64_C(1) << (size_t)(found - s->terms);
  }
  return mask;
}
static unsigned count(uint64_t mask) {
  unsigned n = 0;
  for (; mask; mask &= mask - 1)
    n++;
  return n;
}
unsigned asngn_code_rank(const code_selection *s, const char *path, const char *text,
                         size_t length) {
  uint64_t names = matches(s, path, strlen(path));
  const char *query = s->turn->retrieval_query;
  unsigned exact = query && *path && strstr(query, path) ? 64 : 0;
  return exact + 2 * count(names) + count(names | matches(s, text, length));
}
