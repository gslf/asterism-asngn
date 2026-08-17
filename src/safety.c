/*
 * safety.c — input gate: size cap, UTF-8 validity, control-character
 * stripping. Every rejection carries a precise reason via asngn_seterr; the
 * message is sanitized in place (may shrink, never grows).
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <string.h>

/* Byte offset of the first invalid UTF-8 sequence. Sequence lengths come
 * from the lead byte; each candidate sequence is checked with the one
 * authoritative validator (asngn_utf8_valid over just that sequence), so
 * this never disagrees with the gate's accept/reject decision. */
static size_t utf8_invalid_offset(const char *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned char b = (unsigned char)s[i];
    size_t l;
    if (b < 0x80)
      l = 1;
    else if (b >= 0xC2 && b <= 0xDF)
      l = 2;
    else if (b >= 0xE0 && b <= 0xEF)
      l = 3;
    else if (b >= 0xF0 && b <= 0xF4)
      l = 4;
    else
      return i;
    if (l > len - i || !asngn_utf8_valid(s + i, l)) return i;
    i += l;
  }
  return len;
}

asngn_err asngn_gate_input(asngn_ctx *c, char *text, size_t *len) {
  size_t i;
  bool only_ws;

  if (!len || !text || *len == 0)
    return asngn_seterr(c, ASNGN_ERR_INVALID, "empty message");

  if (*len > (size_t)ASNGN_INPUT_MAX)
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "message exceeds 64 KiB (got %zu bytes)", *len);

  if (!asngn_utf8_valid(text, *len))
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "message is not valid UTF-8 (byte offset %zu)",
                        utf8_invalid_offset(text, *len));

  /* Sanitize in place: strip every control character except \n and \t. */
  *len = asngn_strip_controls(text, *len);

  only_ws = true;
  for (i = 0; i < *len; i++) {
    char ch = text[i];
    if (ch != ' ' && ch != '\t' && ch != '\n') {
      only_ws = false;
      break;
    }
  }
  if (only_ws)
    return asngn_seterr(c, ASNGN_ERR_INVALID,
                        "message is empty after control-character stripping");

  return ASNGN_OK;
}
