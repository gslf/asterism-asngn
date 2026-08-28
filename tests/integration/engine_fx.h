/*
 * engine_fx.h — the shared end-to-end fixture of the asngn integration
 * tests: a full engine over 4 scripted fake
 * models (fakes.h) plus the REAL astools registry around the scripted
 * asngn_fake_tool binary (fixtures.h).
 *
 * Each TEST gets fresh tmpdirs, fresh fake queues, and a fresh context:
 * call eng_setup at the top and eng_drop at the bottom. The Asper sibling
 * is disabled by the fixture config; astools opens over a full-trust
 * fixture root whose single tool "fake" dispatches the ASNGN_TEST_TOOL_PATH
 * binary with the scripted behavior as argv[1].
 *
 * Include asngn_test.h BEFORE this header (feature-test macros).
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_ENGINE_FX_H
#define ASNGN_ENGINE_FX_H

#include "asngn_internal.h"
#include "fakes.h"
#include "fixtures.h"

#ifndef ASNGN_TEST_TOOL_PATH
#define ASNGN_TEST_TOOL_PATH ""
#endif

/* Compile definition first (always a real path when CMake set it); the
 * ASNGN_TEST_TOOL env var is the manual-run fallback. */
static const char *eng_tool_path(void) {
  static const char defined[] = ASNGN_TEST_TOOL_PATH;
  const char *env;
  if (defined[0] != '\0') return defined;
  env = getenv("ASNGN_TEST_TOOL");
  return env != NULL ? env : "";
}

typedef struct {
  char root_raw[256]; /* engine root                  */
  char reg_raw[256];  /* astools registry root        */
  char ws_raw[256];   /* astools workspace            */
  char cfg_raw[256];  /* config files live here       */
  char astools_cfg[512], engine_cfg[512];
  fake_model nano, light, stdm, embed;
  fake_clock clk;
  asngn_ctx *c;
  asngn_session *s;
} eng_fx;

/* Build the registry (tool id "fake" running `behavior`), the astools
 * config, the engine config (+ caller `extra` sections), then open the
 * engine with the 4 fakes and the fake clock and open session "s1".
 * 1 on success. */
static int eng_setup_tool(eng_fx *f, const char *tool_id,
                          const char *behavior, const char *extra) {
  asngn_open_params p;
  asngn_model_iface ifs[4];
  const char *const ids[4] = { "nano", "light", "std", "embed" };
  asngn_clock ck;

  memset(f, 0, sizeof *f);
  if (eng_tool_path()[0] == '\0') return 0;
  if (!asngn_test_tmpdir(f->root_raw) || !asngn_test_tmpdir(f->reg_raw) ||
      !asngn_test_tmpdir(f->ws_raw) || !asngn_test_tmpdir(f->cfg_raw))
    return 0;
  if (!asngn_fix_registry(f->reg_raw, tool_id, eng_tool_path(), behavior))
    return 0;
  snprintf(f->astools_cfg, sizeof f->astools_cfg, "%s/astools.xcdn",
           f->cfg_raw);
  snprintf(f->engine_cfg, sizeof f->engine_cfg, "%s/config.xcdn",
           f->cfg_raw);
  if (!asngn_fix_astools_config(f->astools_cfg, f->reg_raw, f->ws_raw))
    return 0;
  if (!asngn_fix_engine_config(f->engine_cfg, f->astools_cfg, f->reg_raw,
                               f->ws_raw, extra))
    return 0;

  fake_model_init(&f->nano);
  fake_model_init(&f->light);
  fake_model_init(&f->stdm);
  fake_model_init(&f->embed);
  fake_clock_set(&f->clk, 1755168000); /* fixed instant; time is frozen */
  ck = fake_clock_make(&f->clk);
  ifs[0] = fake_model_iface(&f->nano);
  ifs[1] = fake_model_iface(&f->light);
  ifs[2] = fake_model_iface(&f->stdm);
  ifs[3] = fake_model_iface(&f->embed);

  memset(&p, 0, sizeof p);
  p.engine_root = f->root_raw;
  p.config_path = f->engine_cfg;
  if (asngn_open_with(&p, ifs, 4, ids, &ck, &f->c) != ASNGN_OK) {
    f->c = NULL;
    return 0;
  }
  if (asngn_session_open(f->c, "s1", &f->s) != ASNGN_OK) {
    f->s = NULL;
    return 0;
  }
  return 1;
}

static int eng_setup(eng_fx *f, const char *behavior, const char *extra) {
  return eng_setup_tool(f, "fake", behavior, extra);
}

static void eng_drop(eng_fx *f) {
  if (f->s != NULL) asngn_session_close(f->s);
  if (f->c != NULL) asngn_close(f->c);
  f->s = NULL;
  f->c = NULL;
  fake_model_dispose(&f->nano);
  fake_model_dispose(&f->light);
  fake_model_dispose(&f->stdm);
  fake_model_dispose(&f->embed);
  if (f->root_raw[0] != '\0') asngn_test_rmtree(f->root_raw);
  if (f->reg_raw[0] != '\0') asngn_test_rmtree(f->reg_raw);
  if (f->ws_raw[0] != '\0') asngn_test_rmtree(f->ws_raw);
  if (f->cfg_raw[0] != '\0') asngn_test_rmtree(f->cfg_raw);
}

/* ---- streaming sink ------------------------------------------------------ */

typedef struct {
  char buf[65536];
  size_t len;
} eng_sink;

static void eng_sink_cb(const char *piece, void *ud) {
  eng_sink *k = (eng_sink *)ud;
  size_t n = piece != NULL ? strlen(piece) : 0;
  if (n > 0 && k->len + n < sizeof k->buf) {
    memcpy(k->buf + k->len, piece, n);
    k->len += n;
    k->buf[k->len] = '\0';
  }
}

/* Submit one message and wait for its result (60 s real-time budget —
 * the fakes answer instantly; only the tool subprocess takes real time). */
static asngn_err eng_turn(eng_fx *f, const char *msg,
                          const asngn_submit_opts *opts, eng_sink *sink,
                          asngn_turn_result *out) {
  asngn_task *t = NULL;
  asngn_err e = asngn_submit(f->s, msg, opts,
                             sink != NULL ? eng_sink_cb : NULL, sink, &t);
  if (e != ASNGN_OK) return e;
  e = asngn_task_wait(t, 60000, out);
  asngn_task_free(t);
  return e;
}

/* ---- assertion sugar ----------------------------------------------------- */

#define ASSERT_CONTAINS(hay, needle)                                        \
  do {                                                                      \
    const char *h_ = (hay), *n_ = (needle);                                 \
    if (h_ == NULL || strstr(h_, n_) == NULL) {                             \
      ASNGN_FAILF("ASSERT_CONTAINS(%s, \"%s\"):\n   haystack: %.2000s",     \
                  #hay, n_, h_ != NULL ? h_ : "(null)");                    \
      return;                                                               \
    }                                                                       \
  } while (0)

#define ASSERT_NOT_CONTAINS(hay, needle)                                    \
  do {                                                                      \
    const char *h_ = (hay), *n_ = (needle);                                 \
    if (h_ != NULL && strstr(h_, n_) != NULL) {                             \
      ASNGN_FAILF("ASSERT_NOT_CONTAINS(%s, \"%s\"):\n   haystack: %.2000s", \
                  #hay, n_, h_);                                            \
      return;                                                               \
    }                                                                       \
  } while (0)

#endif /* ASNGN_ENGINE_FX_H */
