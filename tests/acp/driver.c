/* Test-only scripted models over the production ACP host and actual tool runtime. */
#include "asngn_test.h"
#include "integration/engine_fx.h"
#include "server.h"
#include <signal.h>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  eng_fx f;
  if (!eng_setup(&f, "echo", NULL))
    return 3;
  (void)asngn_test_case_failed;
  (void)asngn_test_read_file;
  (void)signal(SIGPIPE, SIG_IGN);
  asngn_set_logger(f.c, NULL, NULL);
  asngn_session_close(f.s);
  f.s = NULL;
  bool action = !strcmp(argv[1], "mutate") || !strcmp(argv[1], "read");
  bool long_answer = !strcmp(argv[1], "long");
  int ok = 1;
  for (int i = 0; i < 3; i++) {
    ok &= fake_model_push(&f.nano,
                          action ? "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n"
                                 : "CLASS SIMPLE | DETAIL TERSE | MODE DIRECT | TASK CHAT\n");
    if (action) {
      char call[256];
      snprintf(call, sizeof call,
               "{action:\"call\",why:\"observe\",input:fake.%s {msg:\"review α payload %d\"},"
               "success:\"observed\",fallback:\"report\"}\n",
               !strcmp(argv[1], "mutate") ? "mut" : "run", i);
      ok &= fake_model_push(&f.light, call);
      ok &= fake_model_push(&f.light, "{action:\"answer\"}\n");
      ok &= fake_model_push(&f.stdm, "Action result reported: α 🍵.\n");
    } else if (!strcmp(argv[1], "error")) {
      ok &= fake_model_push_error(&f.light, ASNGN_ERR_MODEL);
      ok &= fake_model_push_error(&f.stdm, ASNGN_ERR_MODEL);
    } else if (long_answer) {
      asngn_buf answer = {0};
      for (int j = 0; j < 6000; j++)
        if (asngn_buf_appends(&answer, "α 🍵 evidence.\n") != ASNGN_OK)
          ok = 0;
      ok &= fake_model_push(&f.light, answer.data);
      asngn_buf_free(&answer);
    } else
      ok &= fake_model_push(&f.light, "Hello α 🍵.\n");
  }
  if (!ok) {
    eng_drop(&f);
    return 4;
  }
  asmodel_json_value *ready = asmodel_json_object();
  asmodel_json_object_set(ready, "fixtureWorkspace", asmodel_json_string(f.ws_raw));
  char *wire = asmodel_json_write(ready, 0);
  if (!wire) {
    asmodel_json_free(ready);
    eng_drop(&f);
    return 5;
  }
  puts(wire);
  fflush(stdout);
  free(wire);
  asmodel_json_free(ready);
  int status = asngn_acp_run(f.c, 0, 1);
  asngn_stats stats;
  asngn_get_stats(f.c, &stats);
  fprintf(stderr, "ACP fixture tool calls: %zu\n", stats.tool_calls);
  eng_drop(&f);
  return status;
}
