/* Local editor host. Stdout is reserved for newline-delimited ACP JSON-RPC. */
#include "server.h"
#include <signal.h>
#include <string.h>

int main(int argc, char **argv) {
  asngn_open_params params = {0};
  const char *usage = "usage: asngn-acp --workspace <directory> [--root <directory>] "
                      "[--config <file>] [--allow-degraded]\n";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) {
      fputs(usage, stdout);
      return 0;
    }
    if (!strcmp(argv[i], "--version")) {
      puts(asngn_version());
      return 0;
    }
    if (!strcmp(argv[i], "--allow-degraded")) {
      params.allow_degraded = 1;
      continue;
    }
    const char **target = !strcmp(argv[i], "--workspace") ? &params.workspace_root
                          : !strcmp(argv[i], "--root")    ? &params.engine_root
                          : !strcmp(argv[i], "--config")  ? &params.config_path
                                                          : NULL;
    if (!target || *target || i + 1 == argc) {
      fputs(usage, stderr);
      return 2;
    }
    *target = argv[++i];
  }
  if (!params.workspace_root || params.workspace_root[0] != '/') {
    fputs(usage, stderr);
    return 2;
  }
  (void)signal(SIGPIPE, SIG_IGN);
  asngn_ctx *engine = NULL;
  asngn_err e = asngn_open(&params, &engine);
  if (e != ASNGN_OK) {
    fprintf(stderr, "asngn-acp: startup failed: %s\n", asngn_err_name(e));
    return 1;
  }
  asngn_set_logger(engine, NULL, NULL);
  int status = asngn_acp_run(engine, 0, 1);
  asngn_close(engine);
  return status;
}
