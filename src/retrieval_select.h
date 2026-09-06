/* Streaming admission is separate from the final hybrid ranker. */
#ifndef ASNGN_RETRIEVAL_SELECT_H
#define ASNGN_RETRIEVAL_SELECT_H
#include "retrieval.h"
typedef struct {
  asngn_ctx *c;
  asngn_turn_state *turn;
  code_index *index;
  char terms[64][192];
  size_t term_count, pinned, candidates, read_bytes, read_files;
  size_t skipped_large, skipped_binary, skipped_budget;
  bool terms_truncated, active_read, active_rechecked;
  uint8_t active_hash[32];
} code_selection;
void asngn_code_terms(code_selection *selection, const char *query);
unsigned asngn_code_rank(const code_selection *selection, const char *path, const char *text,
                         size_t length);
asngn_err asngn_code_select_file(code_selection *selection, const char *path, const char *text,
                                 size_t length, bool active);
void asngn_code_selection_sort(code_selection *selection);
#endif
