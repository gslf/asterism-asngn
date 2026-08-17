/*
 * sha256.c — SHA-256 per FIPS 180-4. Written for asngn from the published
 * specification; streaming, one-shot, file and hex helpers.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_transform(asngn_sha256_ctx *ctx, const uint8_t *block) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = ctx->state[0];
  b = ctx->state[1];
  c = ctx->state[2];
  d = ctx->state[3];
  e = ctx->state[4];
  f = ctx->state[5];
  g = ctx->state[6];
  h = ctx->state[7];
  for (i = 0; i < 64; i++) {
    uint32_t s1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + sha256_k[i] + w[i];
    uint32_t s0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

void asngn_sha256_init(asngn_sha256_ctx *ctx) {
  if (!ctx) return;
  ctx->state[0] = 0x6a09e667u;
  ctx->state[1] = 0xbb67ae85u;
  ctx->state[2] = 0x3c6ef372u;
  ctx->state[3] = 0xa54ff53au;
  ctx->state[4] = 0x510e527fu;
  ctx->state[5] = 0x9b05688cu;
  ctx->state[6] = 0x1f83d9abu;
  ctx->state[7] = 0x5be0cd19u;
  ctx->bitlen = 0;
  ctx->buflen = 0;
}

void asngn_sha256_update(asngn_sha256_ctx *ctx, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  if (!ctx || (!p && len > 0)) return;
  ctx->bitlen += (uint64_t)len * 8;
  while (len > 0) {
    size_t take;
    if (ctx->buflen == 0 && len >= 64) {
      sha256_transform(ctx, p);
      p += 64;
      len -= 64;
      continue;
    }
    take = 64 - ctx->buflen;
    if (take > len) take = len;
    memcpy(ctx->buffer + ctx->buflen, p, take);
    ctx->buflen += take;
    p += take;
    len -= take;
    if (ctx->buflen == 64) {
      sha256_transform(ctx, ctx->buffer);
      ctx->buflen = 0;
    }
  }
}

void asngn_sha256_final(asngn_sha256_ctx *ctx, uint8_t out[32]) {
  uint64_t bits;
  uint8_t tail[8];
  size_t i;
  static const uint8_t pad80 = 0x80;
  static const uint8_t zero = 0x00;
  if (!ctx || !out) return;
  bits = ctx->bitlen;
  asngn_sha256_update(ctx, &pad80, 1);
  while (ctx->buflen != 56) asngn_sha256_update(ctx, &zero, 1);
  for (i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - 8 * i));
  asngn_sha256_update(ctx, tail, 8); /* flushes the final block */
  for (i = 0; i < 8; i++) {
    out[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
  }
}

void asngn_sha256(const void *data, size_t len, uint8_t out[32]) {
  asngn_sha256_ctx ctx;
  asngn_sha256_init(&ctx);
  asngn_sha256_update(&ctx, data, len);
  asngn_sha256_final(&ctx, out);
}

asngn_err asngn_sha256_file(const char *path, uint8_t out[32]) {
  FILE *f;
  uint8_t *buf;
  asngn_sha256_ctx ctx;
  size_t got;
  asngn_err err = ASNGN_OK;
  if (!path || !out) return ASNGN_ERR_INVALID;
  f = os_fopen(path, "rb");
  if (!f) return os_file_exists(path) ? ASNGN_ERR_IO : ASNGN_ERR_NOT_FOUND;
  buf = malloc(65536);
  if (!buf) {
    fclose(f);
    return ASNGN_ERR_NOMEM;
  }
  asngn_sha256_init(&ctx);
  while ((got = fread(buf, 1, 65536, f)) > 0)
    asngn_sha256_update(&ctx, buf, got);
  if (ferror(f)) err = ASNGN_ERR_IO;
  free(buf);
  fclose(f);
  if (err == ASNGN_OK) asngn_sha256_final(&ctx, out);
  return err;
}

void asngn_sha256_hex(const uint8_t hash[32], size_t n, char *out) {
  static const char hexd[] = "0123456789abcdef";
  size_t i;
  if (!out) return;
  if (!hash) {
    out[0] = '\0';
    return;
  }
  if (n > 32) n = 32;
  for (i = 0; i < n; i++) {
    out[i * 2] = hexd[hash[i] >> 4];
    out[i * 2 + 1] = hexd[hash[i] & 0x0f];
  }
  out[n * 2] = '\0';
}
