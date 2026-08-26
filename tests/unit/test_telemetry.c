/*
 * test_telemetry.c — event ring + file sink: ring capacity and
 * overwrite-oldest, oldest-first tail, the batched file sink after
 * flush, and the host event-sink callback.
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
               "  cache: { enable: false },\n"
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

static int fx_setup(fx *f, const char *extra) {
  asngn_model_iface ifaces[4];
  const char *ids[4] = { "nano", "light", "std", "embed" };
  asngn_clock clk;
  asngn_open_params p;

  memset(f, 0, sizeof *f);
  if (!asngn_test_tmpdir(f->root)) return 0;
  fake_model_init(&f->nano);
  fake_model_init(&f->light);
  fake_model_init(&f->stdm);
  fake_model_init(&f->embed);
  fake_clock_set(&f->clk, 1755150000);
  if (!fx_write_config(f, extra)) return 0;

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

static void fx_drop(fx *f) {
  asngn_close(f->c);
  f->c = NULL;
  fake_model_dispose(&f->nano);
  fake_model_dispose(&f->light);
  fake_model_dispose(&f->stdm);
  fake_model_dispose(&f->embed);
  asngn_test_rmtree(f->root);
}

/* Emit `n` events with numbered kinds k00, k01, ... */
static void fx_emit_n(fx *f, int n) {
  int i;
  char kind[8];
  for (i = 0; i < n; i++) {
    snprintf(kind, sizeof kind, "k%02d", i);
    asngn_tele_emit(f->c, kind, NULL, NULL, NULL, 0, NULL);
  }
}

/* ── event sink callback ──────────────────────────────────────────────── */

typedef struct {
  int n;
  int bad;
} sink_state;

static void sink_cb(const char *event_xcdn, void *ud) {
  sink_state *st = ud;
  st->n++;
  if (event_xcdn == NULL ||
      strncmp(event_xcdn, "#asngn_event {", 14) != 0)
    st->bad++;
}

/* ── tests ────────────────────────────────────────────────────────────── */

TEST(ring_overwrites_oldest) {
  fx f;
  char **v = NULL;
  size_t n = 0, i;
  char want[24];

  ASSERT_TRUE(fx_setup(&f, "  telemetry: { ring: 8 },\n"));
  fx_emit_n(&f, 12);

  /* 12 events into a ring of 8: k00..k03 were overwritten */
  ASSERT_OK(asngn_telemetry_tail(f.c, 100, &v, &n));
  ASSERT_EQ_INT((long long)n, 8);
  for (i = 0; i < n; i++) {
    ASSERT_TRUE(strncmp(v[i], "#asngn_event {", 14) == 0);
    snprintf(want, sizeof want, "kind: \"k%02d\"", (int)(i + 4));
    ASSERT_TRUE(strstr(v[i], want) != NULL); /* oldest first */
    ASSERT_TRUE(strstr(v[i], "kind: \"k00\"") == NULL);
  }
  asngn_strings_free(v, n);
  v = NULL;

  /* a smaller tail returns the newest slice, still oldest first */
  ASSERT_OK(asngn_telemetry_tail(f.c, 3, &v, &n));
  ASSERT_EQ_INT((long long)n, 3);
  ASSERT_TRUE(strstr(v[0], "kind: \"k09\"") != NULL);
  ASSERT_TRUE(strstr(v[2], "kind: \"k11\"") != NULL);
  asngn_strings_free(v, n);
  fx_drop(&f);
}

TEST(file_sink_flush) {
  fx f;
  char path[400];
  char *text;
  size_t i, lines = 0;

  ASSERT_TRUE(fx_setup(&f,
                       "  telemetry: { ring: 8, "
                       "path: \"telemetry/tele.xcdn\" },\n"));
  fx_emit_n(&f, 12);
  asngn_tele_flush(f.c);

  /* the file batch got every event, not just the ring's last 8 */
  snprintf(path, sizeof path, "%s/telemetry/tele.xcdn", f.root);
  text = asngn_test_read_file(path, NULL);
  ASSERT_TRUE(text != NULL);
  ASSERT_TRUE(strstr(text, "kind: \"k00\"") != NULL);
  ASSERT_TRUE(strstr(text, "kind: \"k11\"") != NULL);
  ASSERT_TRUE(strncmp(text, "#asngn_event {", 14) == 0);
  for (i = 0; text[i] != '\0'; i++)
    if (text[i] == '\n') lines++;
  ASSERT_EQ_INT((long long)lines, 12);
  free(text);
  fx_drop(&f);
}

TEST(event_sink_callback) {
  fx f;
  sink_state st;

  ASSERT_TRUE(fx_setup(&f, "  telemetry: { ring: 8 },\n"));
  memset(&st, 0, sizeof st);
  asngn_set_event_sink(f.c, sink_cb, &st);
  fx_emit_n(&f, 5);
  ASSERT_EQ_INT(st.n, 5);
  ASSERT_EQ_INT(st.bad, 0);

  /* a NULL sink disables the callback */
  asngn_set_event_sink(f.c, NULL, NULL);
  fx_emit_n(&f, 3);
  ASSERT_EQ_INT(st.n, 5);
  fx_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(ring_overwrites_oldest),
  TEST_ENTRY(file_sink_flush),
  TEST_ENTRY(event_sink_callback),
};

RUN_ALL_TESTS()
