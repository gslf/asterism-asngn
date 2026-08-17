/*
 * uuid.c — RFC 4122 version-4 UUIDs: generation, validation, raw form.
 */

#include "asngn_internal.h"

static const char uuid_hexd[] = "0123456789abcdef";

void asngn_uuid_from_bytes(const uint8_t in[16], char out[37]) {
  /* hyphen before bytes 4, 6, 8 and 10 => positions 8, 13, 18, 23 */
  static const int dash_before[16] = {0, 0, 0, 0, 1, 0, 1, 0,
                                      1, 0, 1, 0, 0, 0, 0, 0};
  size_t i, o = 0;
  if (!in || !out) return;
  for (i = 0; i < 16; i++) {
    if (dash_before[i]) out[o++] = '-';
    out[o++] = uuid_hexd[in[i] >> 4];
    out[o++] = uuid_hexd[in[i] & 0x0f];
  }
  out[o] = '\0';
}

void asngn_uuid_v4(char out[37]) {
  uint8_t b[16];
  if (!out) return;
  os_random_bytes(b, sizeof b);
  b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
  b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */
  asngn_uuid_from_bytes(b, out);
}

static int hex_val(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool asngn_uuid_valid(const char *s) {
  size_t i;
  if (!s) return false;
  for (i = 0; i < 36; i++) {
    if (s[i] == '\0') return false;
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') return false;
    } else if (hex_val(s[i]) < 0) {
      return false;
    }
  }
  return s[36] == '\0';
}

bool asngn_uuid_to_bytes(const char *s, uint8_t out[16]) {
  size_t i = 0, o = 0;
  if (!out || !asngn_uuid_valid(s)) return false;
  while (i < 36) {
    if (s[i] == '-') {
      i++;
      continue;
    }
    out[o++] = (uint8_t)((hex_val(s[i]) << 4) | hex_val(s[i + 1]));
    i += 2;
  }
  return true;
}
