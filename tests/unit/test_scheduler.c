#include "asngn_test.h"
#include "asngn_internal.h"

#ifndef ASNGN_NO_THREADS
typedef struct {
  os_mutex mu;
  os_cond cv;
  int entered, release;
} barrier;
static int count(void *ud, const char *s) {
  (void)ud;
  return (int)strlen(s) / 4 + 1;
}
static asngn_err generate(void *ud, const char *sys, const char *usr,
                          const char *grammar, const asngn_gen_params *p,
                          asngn_token_fn cb, void *cbud, volatile int *cancel,
                          char **out, int *in, int *gen) {
  barrier *b = ud;
  (void)sys;
  (void)usr;
  (void)grammar;
  (void)p;
  (void)cb;
  (void)cbud;
  os_mutex_lock(&b->mu);
  b->entered++;
  os_cond_broadcast(&b->cv);
  int64_t until = os_monotonic_ms() + 5000;
  while (!b->release && !*cancel && os_monotonic_ms() < until)
    os_cond_timedwait(&b->cv, &b->mu, 20);
  int released = b->release;
  os_mutex_unlock(&b->mu);
  if (*cancel)
    return ASNGN_ERR_CANCELLED;
  if (!released)
    return ASNGN_ERR_TIMEOUT;
  *out = asngn_strdup("Hello.");
  *in = 12;
  *gen = 2;
  return ASNGN_OK;
}
TEST(concurrent_sessions_backpressure_and_queue_deadline) {
  char root[256], path[512];
  asngn_ctx *c = NULL;
  asngn_open_params p;
  asngn_session *s[5] = {0};
  asngn_task *t[4] = {0}, *rejected = NULL;
  asngn_model_iface iface;
  const char *ids[] = {"std"};
  barrier b;
  memset(&b, 0, sizeof b);
  os_mutex_init(&b.mu);
  os_cond_init(&b.cv);
  ASSERT_TRUE(asngn_test_tmpdir(root));
  snprintf(path, sizeof path, "%s/config.xcdn", root);
  const char *config =
      "{engine:{workers:2,queue_capacity:1},"
      "integration:{asper:{enable:false},astools:{enable:false,workspace:"
      "\"session\"}},"
      "cache:{enable:false},validation:{judge:\"off\"},routing:{classifier:"
      "\"heuristic\"},"
      "models:{pool:[{id:\"std\",backend:\"openai\",base_url:\"http://"
      "localhost:1/v1\",model:\"fake\",ctx:32768}],"
      "roles:{router:\"std\",planner:\"std\",generator:\"std\",compressor:"
      "\"std\",adapter:\"std\",judge:\"std\",embedder:\"\"}}}";
  ASSERT_OK(os_write_file(path, config, strlen(config)));
  memset(&iface, 0, sizeof iface);
  iface.ud = &b;
  iface.generate = generate;
  iface.count_tokens = count;
  memset(&p, 0, sizeof p);
  p.engine_root = root;
  p.config_path = path;
  p.allow_degraded = 1;
  ASSERT_OK(asngn_open_with(&p, &iface, 1, ids, NULL, &c));
  ASSERT_EQ_INT(c->lanes_n, 2);
  for (int i = 0; i < 5; i++) {
    char slug[16];
    snprintf(slug, sizeof slug, "session%d", i);
    ASSERT_OK(asngn_session_open(c, slug, &s[i]));
    ASSERT_OK(asngn_session_set_mode(s[i], ASNGN_USAGE_CHAT));
  }
  ASSERT_OK(asngn_submit(s[0], "hello", NULL, NULL, NULL, &t[0]));
  /* Wait until the first lane owns its task before filling the bounded queue.
   */
  os_mutex_lock(&b.mu);
  int64_t until = os_monotonic_ms() + 3000;
  while (b.entered < 1 && os_monotonic_ms() < until)
    os_cond_timedwait(&b.cv, &b.mu, 20);
  os_mutex_unlock(&b.mu);
  ASSERT_OK(asngn_submit(s[1], "hello", NULL, NULL, NULL, &t[1]));
  os_mutex_lock(&b.mu);
  until = os_monotonic_ms() + 3000;
  while (b.entered < 2 && os_monotonic_ms() < until)
    os_cond_timedwait(&b.cv, &b.mu, 20);
  int entered = b.entered;
  os_mutex_unlock(&b.mu);
  ASSERT_EQ_INT(entered, 2); /* fails under a global worker or model mutex */
  ASSERT_TRUE(strcmp(c->lanes[0]->workspace.canonical_root,
                     c->lanes[1]->workspace.canonical_root) != 0);
  ASSERT_TRUE(strcmp(c->workspace.canonical_root,
                     c->lanes[0]->workspace.canonical_root) != 0);
  ASSERT_ERR(asngn_submit(s[0], "overlap", NULL, NULL, NULL, &rejected),
             ASNGN_ERR_BUSY);
  asngn_submit_opts opts;
  memset(&opts, 0, sizeof opts);
  opts.deadline_ms = 10;
  ASSERT_OK(asngn_submit(s[2], "hello", &opts, NULL, NULL, &t[2]));
  ASSERT_ERR(asngn_submit(s[3], "overflow", NULL, NULL, NULL, &rejected),
             ASNGN_ERR_BUSY);
  ASSERT_TRUE(!s[3]->busy);
  os_sleep_ms(30);
  os_mutex_lock(&b.mu);
  b.release = 1;
  os_cond_broadcast(&b.cv);
  os_mutex_unlock(&b.mu);
  for (int i = 0; i < 3; i++) {
    asngn_turn_result r;
    memset(&r, 0, sizeof r);
    asngn_err e = asngn_task_wait(t[i], 10000, &r);
    ASSERT_EQ_INT(e, i == 2 ? ASNGN_ERR_TIMEOUT : ASNGN_OK);
    asngn_turn_result_free(&r);
    asngn_task_free(t[i]);
  }
  ASSERT_EQ_INT(b.entered, 2);
  ASSERT_EQ_INT(s[2]->log_n, 0);
  ASSERT_OK(asngn_submit(s[0], "hello again", NULL, NULL, NULL, &t[3]));
  asngn_turn_result r;
  memset(&r, 0, sizeof r);
  ASSERT_OK(asngn_task_wait(t[3], 10000, &r));
  asngn_turn_result_free(&r);
  asngn_task_free(t[3]);
  for (int i = 0; i < 5; i++)
    asngn_session_close(s[i]);
  asngn_close(c);
  os_cond_destroy(&b.cv);
  os_mutex_destroy(&b.mu);
  asngn_test_rmtree(root);
}
#endif
TEST(resource_admission) {
  asngn_ctx c;
  memset(&c, 0, sizeof c);
  c.cfg.scheduler_workers = 4;
  c.models_n = 1;
  c.models[0].cfg.ram_mb = 512;
  c.models[0].cfg.gpu_layers = 0;
  ASSERT_EQ_INT(asngn_scheduler_capacity(&c), 1);
  c.cfg.max_ram_mb = 4096;
  ASSERT_EQ_INT(asngn_scheduler_capacity(&c), 3);
  c.models[0].cfg.gpu_layers = -1;
  c.models[0].cfg.vram_mb = 512;
  c.cfg.max_vram_mb = 1024;
  ASSERT_EQ_INT(asngn_scheduler_capacity(&c), 1);
  c.cfg.max_vram_mb = 2048;
  ASSERT_EQ_INT(asngn_scheduler_capacity(&c), 3);
}
TEST_LIST = {
#ifndef ASNGN_NO_THREADS
    TEST_ENTRY(concurrent_sessions_backpressure_and_queue_deadline),
#endif
    TEST_ENTRY(resource_admission)};
RUN_ALL_TESTS()
