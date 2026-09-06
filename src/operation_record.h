/* The consumption journal is strict JSON inside checksummed WAL frames. */
#ifndef ASNGN_OPERATION_RECORD_H
#define ASNGN_OPERATION_RECORD_H
#include "asngn_internal.h"
typedef struct {
  char id[37];
  uint8_t identity[32]; /* model, operation kind and host request identity */
  int64_t day, delta, input, output;
  bool reserved, known;
} asngn_operation_record;
char *asngn_operation_encode(const asngn_operation *op, const char *state,
    int64_t delta, int input, int output, bool known, asngn_err outcome);
asngn_err asngn_operation_decode(const char *text, size_t bytes, asngn_operation_record *out);
#endif
