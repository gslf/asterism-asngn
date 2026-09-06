/* Disposable repository chunks; source versions are distinct from chunk hashes. */
#ifndef ASNGN_RETRIEVAL_H
#define ASNGN_RETRIEVAL_H
#include "asngn_internal.h"
#define CODE_MAX 1024
#define CODE_CHUNK 2048
#define CODE_FILE_BYTES 262144
#define CODE_SCAN_BYTES (64u * 1024u * 1024u)
#define CODE_FILE_CHUNKS 8
typedef struct {
  char *path, *text;
  size_t line, end_line, offset;
  unsigned admission_score;
  uint8_t hash[32], file_hash[32];
  float *vec;
} chunk;
typedef struct {
  chunk *v;
  size_t n;
  int dim;
  uint8_t model[32];
} code_index;
bool asngn_code_stopped(asngn_ctx *c, asngn_turn_state *t);
asngn_err asngn_code_scan(asngn_ctx *c, asngn_turn_state *t, code_index *ix);
#endif
