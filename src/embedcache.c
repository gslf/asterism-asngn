/*
 * embedcache.c — cache/embeddings.bin read/write.
 *
 * On-disk layout, little-endian regardless of host (identical to Asper's
 * embedding cache, magic aside):
 *
 *   magic       4 B   "ASNG"
 *   version     u32   1
 *   dim         u32   embedding dimension
 *   count       u64   number of entries
 *   model_hash  32 B  SHA-256 of the embedder weights file
 *   entries     count x (16 B raw entry UUID
 *                        + u64 FNV-1a-64 of the query bytes
 *                        + dim x f32, L2-normalized)
 *
 * The cache is derived data: every failure mode short of allocation
 * failure — bad magic, unsupported version, dimension mismatch, unknown or
 * changed embedding model, truncation, unreadable file — degrades to "no
 * vectors" with a WARN, never to an error; entries left without a vector
 * are re-embedded lazily at probe time. Vectors are moved with memcpy to
 * avoid alignment UB; header integers go through explicit little-endian
 * codecs, and the float payload is byte-swapped on big-endian hosts. The
 * model hash is written as zeroes when the embedder is unavailable, which
 * forces a rebuild on the next load — exactly the asper discipline.
 *
 * Locking: none taken here. load runs from asngn_cache_init and save from
 * the compact path (cache_mu held) or shutdown; the caller serializes all
 * access to c->cache.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* stores raw IEEE-754 single precision. */
typedef char asngn_assert_float32[(sizeof(float) == 4) ? 1 : -1];

#define ASNGN_EMBED_HEADER 52u /* 4 + 4 + 4 + 8 + 32 */

/* ── little-endian codecs ─────────────────────────────────────────────── */

static bool embc_host_is_le(void) {
  const union {
    uint32_t u;
    uint8_t b[4];
  } probe = { 0x01020304u };
  return probe.b[0] == 0x04;
}

static void embc_put_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
  p[2] = (uint8_t)((v >> 16) & 0xffu);
  p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void embc_put_u64le(uint8_t *p, uint64_t v) {
  int i;
  for (i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
}

static uint32_t embc_get_u32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t embc_get_u64le(const uint8_t *p) {
  uint64_t v = 0;
  int i;
  for (i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
  return v;
}

/* File bytes (LE) -> aligned float buffer. */
static void embc_vec_decode(const uint8_t *src, float *dst, int dim) {
  int i;
  if (embc_host_is_le()) {
    memcpy(dst, src, (size_t)dim * 4u);
    return;
  }
  for (i = 0; i < dim; i++) {
    uint32_t bits = embc_get_u32le(src + (size_t)i * 4u);
    memcpy(&dst[i], &bits, 4);
  }
}

/* Float row -> file bytes (LE). */
static void embc_vec_encode(const float *src, uint8_t *dst, int dim) {
  int i;
  if (embc_host_is_le()) {
    memcpy(dst, src, (size_t)dim * 4u);
    return;
  }
  for (i = 0; i < dim; i++) {
    uint32_t bits;
    memcpy(&bits, &src[i], 4);
    embc_put_u32le(dst + (size_t)i * 4u, bits);
  }
}

/* ── shared predicates ────────────────────────────────────────────────── */

static uint64_t embc_query_fnv(const asngn_cache_entry *e) {
  const char *q = e->query != NULL ? e->query : "";
  return asngn_fnv1a64(q, strlen(q));
}

static bool embc_zero32(const uint8_t h[32]) {
  size_t i;
  for (i = 0; i < 32; i++)
    if (h[i] != 0) return false;
  return true;
}

/* An entry is written iff it carries a vector and a well-formed id; the
 * same predicate drives the count and the write pass so the header count
 * always matches the entries actually emitted. */
static bool embc_savable(const asngn_cache_entry *e, uint8_t uuid[16]) {
  if (e->vec == NULL) return false;
  return asngn_uuid_to_bytes(e->id, uuid);
}

static char *embc_path(asngn_ctx *c) {
  return os_path_join(c->cache_dir, "embeddings.bin");
}

/* ── load ─────────────────────────────────────────────────────────────── */

asngn_err asngn_embedcache_load(asngn_ctx *c) {
  char *path;
  uint8_t *data = NULL;
  uint8_t want_hash[32];
  size_t len = 0, entry_size, i, j, loaded = 0;
  uint64_t count;
  uint32_t dim;
  int want_dim;
  const uint8_t *p;
  const char *why;
  asngn_err e;

  if (c == NULL || c->cache_dir == NULL) return ASNGN_ERR_INVALID;
  want_dim = asngn_models_embed_dim(c);
  if (want_dim > 0) c->embed_dim = want_dim;

  path = embc_path(c);
  if (path == NULL)
    return asngn_seterr(c, ASNGN_ERR_NOMEM, "embed cache: out of memory");
  if (!os_file_exists(path)) {
    free(path);
    if (c->cache_n > 0)
      asngn_log(c, ASNGN_LOG_INFO, "cache",
                "embedding cache missing; %zu entr%s re-embed lazily",
                c->cache_n, c->cache_n == 1 ? "y" : "ies");
    return ASNGN_OK;
  }
  {
    char *fbuf = NULL;
    e = os_read_file(path, &fbuf, &len);
    free(path);
    if (e != ASNGN_OK) {
      asngn_log(c, ASNGN_LOG_WARN, "cache",
                "embedding cache unreadable; vectors rebuild lazily");
      return ASNGN_OK;
    }
    data = (uint8_t *)fbuf;
  }

  if (len < ASNGN_EMBED_HEADER ||
      memcmp(data, ASNGN_EMBED_MAGIC, 4) != 0) {
    why = "bad magic";
    goto rebuild;
  }
  if (embc_get_u32le(data + 4) != ASNGN_EMBED_VERSION) {
    why = "unsupported version";
    goto rebuild;
  }
  dim = embc_get_u32le(data + 8);
  if (want_dim <= 0 || dim != (uint32_t)want_dim) {
    why = "dimension mismatch";
    goto rebuild;
  }
  count = embc_get_u64le(data + 12);
  asngn_models_embed_hash(c, want_hash);
  if (embc_zero32(want_hash)) {
    why = "embedding model unknown";
    goto rebuild;
  }
  if (memcmp(data + 20, want_hash, 32) != 0) {
    why = "embedding model changed";
    goto rebuild;
  }
  entry_size = 16u + 8u + (size_t)dim * 4u;
  if (count > (uint64_t)(len - ASNGN_EMBED_HEADER) / entry_size ||
      (uint64_t)len != ASNGN_EMBED_HEADER + count * entry_size) {
    why = "truncated";
    goto rebuild;
  }

  p = data + ASNGN_EMBED_HEADER;
  for (i = 0; i < (size_t)count; i++, p += entry_size) {
    char id[37];
    asngn_uuid_from_bytes(p, id);
    for (j = 0; j < c->cache_n; j++) {
      asngn_cache_entry *ent = &c->cache[j];
      if (ent->vec != NULL || strcmp(ent->id, id) != 0) continue;
      if (embc_get_u64le(p + 16) != embc_query_fnv(ent))
        break; /* query text changed: leave NULL, re-embed lazily */
      ent->vec = malloc((size_t)dim * sizeof *ent->vec);
      if (ent->vec == NULL) {
        free(data);
        return asngn_seterr(c, ASNGN_ERR_NOMEM,
                            "embed cache: out of memory loading vectors");
      }
      embc_vec_decode(p + 24, ent->vec, (int)dim);
      loaded++;
      break;
    }
  }
  free(data);
  asngn_log(c, ASNGN_LOG_INFO, "cache",
            "embedding cache: %zu of %zu vector%s attached", loaded,
            c->cache_n, loaded == 1 ? "" : "s");
  return ASNGN_OK;

rebuild:
  free(data);
  asngn_log(c, ASNGN_LOG_WARN, "cache",
            "embedding cache invalid (%s); vectors rebuild lazily", why);
  return ASNGN_OK;
}

/* ── save ─────────────────────────────────────────────────────────────── */

asngn_err asngn_embedcache_save(asngn_ctx *c) {
  uint8_t header[ASNGN_EMBED_HEADER];
  uint8_t uuid[16];
  uint8_t hash[32];
  uint8_t *entry = NULL;
  char *path = NULL;
  asngn_buf snap;
  size_t entry_size, count = 0, i;
  int dim;
  asngn_err e = ASNGN_OK;

  if (c == NULL || c->cache_dir == NULL) return ASNGN_ERR_INVALID;
  dim = asngn_models_embed_dim(c);
  if (dim <= 0) dim = c->embed_dim; /* embedder gone: last known dim */
  if (dim <= 0) {
    asngn_log(c, ASNGN_LOG_DEBUG, "cache",
              "embedding cache save skipped: no known dimension");
    return ASNGN_OK; /* nothing derivable to write */
  }

  for (i = 0; i < c->cache_n; i++)
    if (embc_savable(&c->cache[i], uuid)) count++;

  memcpy(header, ASNGN_EMBED_MAGIC, 4);
  embc_put_u32le(header + 4, ASNGN_EMBED_VERSION);
  embc_put_u32le(header + 8, (uint32_t)dim);
  embc_put_u64le(header + 12, (uint64_t)count);
  asngn_models_embed_hash(c, hash); /* zeroes when unavailable: rebuilds */
  memcpy(header + 20, hash, 32);

  asngn_buf_init(&snap);
  e = asngn_buf_append(&snap, header, sizeof header);
  entry_size = 16u + 8u + (size_t)dim * 4u;
  if (e == ASNGN_OK) {
    entry = malloc(entry_size);
    if (entry == NULL) e = ASNGN_ERR_NOMEM;
  }
  for (i = 0; e == ASNGN_OK && i < c->cache_n; i++) {
    const asngn_cache_entry *ent = &c->cache[i];
    if (!embc_savable(ent, entry)) continue; /* uuid into entry[0..16) */
    embc_put_u64le(entry + 16, embc_query_fnv(ent));
    embc_vec_encode(ent->vec, entry + 24, dim);
    e = asngn_buf_append(&snap, entry, entry_size);
  }
  if (e == ASNGN_OK) {
    path = embc_path(c);
    if (path == NULL) e = ASNGN_ERR_NOMEM;
    else e = asngn_write_atomic(c, path, snap.data, snap.len);
  }
  free(path);
  free(entry);
  asngn_buf_free(&snap);
  if (e != ASNGN_OK) {
    asngn_log(c, ASNGN_LOG_WARN, "cache", "embedding cache save failed");
    if (e == ASNGN_ERR_NOMEM)
      return asngn_seterr(c, e, "embed cache: out of memory saving");
    return e;
  }
  asngn_log(c, ASNGN_LOG_INFO, "cache",
            "embedding cache saved: %zu entr%s", count,
            count == 1 ? "y" : "ies");
  return ASNGN_OK;
}
