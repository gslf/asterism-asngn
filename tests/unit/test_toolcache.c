/*
 * test_toolcache.c — the exact-key tool-result cache: put/get
 * round-trips under TTL on the fake clock, expiry, clearing, LRU
 * capacity eviction, the 64 KiB size gate, and reload from tools.xcdn.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"
#include "fakes.h"

/* ── shared fixture ───────────────────────────────────────────────────── */

typedef struct {
  char        root[256];
  char        cfg[300];
  fake_model  nano, light, stdm, embed;
  fake_clock  clk;
  asngn_ctx  *c;
} fx;

static int fx_write_config(fx *f, const char *extra) {
  FILE *fp;
  int ok;
  snprintf(f->cfg, sizeof f->cfg, "%s/config.xcdn", f->root);
  fp = fopen(f->cfg, "wb");
  if (fp == NULL) return 0;
  ok = fprintf(fp,
               "#asngn_config {\n"
               "  integration: { asper: { enable: false }, "
               "astools: { enable: false } },\n"
               "  validation: { judge: \"off\" },\n"
               "  routing: { classifier: \"heuristic\" },\n"
               "  models: { pool: [\n"
               "    { id: \"nano\", path: \"none.gguf\" },\n"
               "    { id: \"light\", path: \"none.gguf\" },\n"
               "    { id: \"std\", path: \"none.gguf\" },\n"
               "    { id: \"embed\", path: \"none.gguf\", "
               "embedding: true, dim: 16 },\n"
               "  ] },\n"
               "%s"
               "}\n",
               extra != NULL ? extra : "") > 0;
  fclose(fp);
  return ok;
}

static int fx_open_ctx(fx *f) {
  asngn_model_iface ifaces[4];
  const char *ids[4] = { "nano", "light", "std", "embed" };
  asngn_clock clk;
  asngn_open_params p;

  ifaces[0] = fake_model_iface(&f->nano);
  ifaces[1] = fake_model_iface(&f->light);
  ifaces[2] = fake_model_iface(&f->stdm);
  ifaces[3] = fake_model_iface(&f->embed);
  clk = fake_clock_make(&f->clk);
  memset(&p, 0, sizeof p);
  p.engine_root = f->root;
  p.config_path = f->cfg;
  if (asngn_open_with(&p, ifaces, 4, ids, &clk, &f->c) != ASNGN_OK)
    return 0;
  asngn_set_logger(f->c, NULL, NULL);
  return 1;
}

static int fx_setup(fx *f, const char *extra) {
  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->root)) return 0;
  fake_model_init(&f->nano);
  fake_model_init(&f->light);
  fake_model_init(&f->stdm);
  fake_model_init(&f->embed);
  fake_clock_set(&f->clk, 1755150000);
  if (!fx_write_config(f, extra)) return 0;
  return fx_open_ctx(f);
}

static void fx_drop(fx *f) {
  asngn_close(f->c);
  f->c = NULL;
  fake_model_dispose(&f->nano);
  fake_model_dispose(&f->light);
  fake_model_dispose(&f->stdm);
  fake_model_dispose(&f->embed);
  asngn_test_rmtree(f->root);
}

static int fx_reopen_ctx(fx *f) {
  asngn_close(f->c);
  f->c = NULL;
  return fx_open_ctx(f);
}

/* Deterministic 32-byte key from a short label. */
static void fx_key(const char *label, uint8_t out[32]) {
  asngn_sha256(label, strlen(label), out);
}

static const char CACHE_OFF[] = "  cache: { enable: false },\n";

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(put_get_roundtrip) {
  fx f;
  uint8_t k[32];
  char *out = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  fx_key("fs.read|1.0|read|{path:\"a\"}", k);
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k, &out));
  ASSERT_TRUE(out == NULL);

  asngn_toolcache_put(f.c, k, "#tool_result { ok: true, bytes: 12 }");
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  ASSERT_EQ_STR(out, "#tool_result { ok: true, bytes: 12 }");
  free(out);
  out = NULL;

  /* a second put on the same key supersedes the stored result */
  asngn_toolcache_put(f.c, k, "#tool_result { ok: true, bytes: 99 }");
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  ASSERT_EQ_STR(out, "#tool_result { ok: true, bytes: 99 }");
  free(out);
  fx_drop(&f);
}

TEST(ttl_expiry) {
  fx f;
  uint8_t k[32];
  char *out = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  fx_key("k-ttl", k);
  asngn_toolcache_put(f.c, k, "vivo");
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  free(out);
  out = NULL;

  /* default tool_ttl is PT5M: 400 s later the entry is a miss */
  fake_clock_advance(&f.clk, 400);
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k, &out));
  ASSERT_TRUE(out == NULL);

  /* a fresh put after the expiry is alive again */
  asngn_toolcache_put(f.c, k, "di nuovo");
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  ASSERT_EQ_STR(out, "di nuovo");
  free(out);
  fx_drop(&f);
}

TEST(clear_empties) {
  fx f;
  uint8_t k1[32], k2[32];
  char *out = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  fx_key("k1", k1);
  fx_key("k2", k2);
  asngn_toolcache_put(f.c, k1, "uno");
  asngn_toolcache_put(f.c, k2, "due");
  asngn_toolcache_clear(f.c);
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k1, &out));
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k2, &out));
  fx_drop(&f);
}

TEST(capacity_lru_eviction) {
  fx f;
  uint8_t k1[32], k2[32], k3[32];
  char *out = NULL;

  ASSERT_TRUE(fx_setup(&f,
                       "  cache: { enable: false, "
                       "tool_max_entries: 2 },\n"));
  fx_key("k1", k1);
  fx_key("k2", k2);
  fx_key("k3", k3);
  asngn_toolcache_put(f.c, k1, "uno");
  fake_clock_advance(&f.clk, 1);
  asngn_toolcache_put(f.c, k2, "due");
  fake_clock_advance(&f.clk, 1);
  asngn_toolcache_put(f.c, k3, "tre"); /* evicts k1, the LRU entry */

  ASSERT_TRUE(!asngn_toolcache_get(f.c, k1, &out));
  ASSERT_TRUE(asngn_toolcache_get(f.c, k2, &out));
  ASSERT_EQ_STR(out, "due");
  free(out);
  out = NULL;
  ASSERT_TRUE(asngn_toolcache_get(f.c, k3, &out));
  ASSERT_EQ_STR(out, "tre");
  free(out);
  fx_drop(&f);
}

TEST(oversize_not_cached) {
  fx f;
  uint8_t k[32];
  char *big, *out = NULL;
  size_t n = 64u * 1024u + 1u;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  fx_key("k-big", k);
  big = malloc(n + 1);
  ASSERT_TRUE(big != NULL);
  memset(big, 'x', n);
  big[n] = '\0';
  asngn_toolcache_put(f.c, k, big); /* > 64 KiB: silently not cached */
  free(big);
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k, &out));
  ASSERT_TRUE(out == NULL);

  /* exactly 64 KiB still caches */
  big = malloc(n);
  ASSERT_TRUE(big != NULL);
  memset(big, 'y', n - 1);
  big[n - 1] = '\0';
  asngn_toolcache_put(f.c, k, big);
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  ASSERT_EQ_INT((long long)strlen(out), (long long)(n - 1));
  free(out);
  free(big);
  fx_drop(&f);
}

TEST(reload_from_stream) {
  fx f;
  uint8_t k[32];
  char *out = NULL;

  ASSERT_TRUE(fx_setup(&f, CACHE_OFF));
  fx_key("k-persist", k);
  asngn_toolcache_put(f.c, k, "persistito");
  ASSERT_TRUE(fx_reopen_ctx(&f));
  ASSERT_TRUE(asngn_toolcache_get(f.c, k, &out));
  ASSERT_EQ_STR(out, "persistito");
  free(out);
  out = NULL;

  /* entries expired on disk are dropped by the reload */
  fake_clock_advance(&f.clk, 400);
  ASSERT_TRUE(fx_reopen_ctx(&f));
  ASSERT_TRUE(!asngn_toolcache_get(f.c, k, &out));
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(put_get_roundtrip),
  TEST_ENTRY(ttl_expiry),
  TEST_ENTRY(clear_empties),
  TEST_ENTRY(capacity_lru_eviction),
  TEST_ENTRY(oversize_not_cached),
  TEST_ENTRY(reload_from_stream),
};

RUN_ALL_TESTS()
