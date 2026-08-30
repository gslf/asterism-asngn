/*
 * fixtures.c — sibling-fixture writers for the asngn integration tests
 * (fixtures.h). Modeled on astools' tests/fakes.c fake_registry_write /
 * fake_config_write.
 *
 * Links asngn_static for asngn_buf / os_* only; the emitted text is
 * ordinary #astools_tool / #astools_config / #asngn_config source that
 * the libraries under test parse on their own.
 */

#include "fixtures.h"

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* Current platform, matching astools' registry.c compile-time values. */
static const char *fix_plat_os(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

static const char *fix_plat_arch(void) {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  return "arm64";
#else
  return "x86_64";
#endif
}

/* Append s as the body of a double-quoted xCDN string literal. */
static asngn_err append_escaped(asngn_buf *b, const char *s) {
  asngn_err e = ASNGN_OK;
  if (!s) return ASNGN_OK;
  for (; *s != '\0' && e == ASNGN_OK; s++) {
    if (*s == '"' || *s == '\\') e = asngn_buf_appendc(b, '\\');
    if (e == ASNGN_OK) e = asngn_buf_appendc(b, *s);
  }
  return e;
}

/* Write buf to path via a plain truncating write (tests only). */
static int write_text(const char *path, const asngn_buf *b) {
  return os_write_file(path, b->data != NULL ? b->data : "", b->len) ==
         ASNGN_OK;
}

/* ---- registry ------------------------------------------------------------ */

static const char FIX_COMMANDS[] =
    "    #command {\n"
    "      name: \"run\",\n"
    "      summary: \"Run the scripted behavior (read-only).\",\n"
    "      annotations: { read_only: true, idempotent: true },\n"
    "      params: [\n"
    "        #param { name: \"msg\", type: #type { kind: \"string\" },"
    " required: true },\n"
    "      ],\n"
    "    },\n"
    "    #command {\n"
    "      name: \"mut\",\n"
    "      summary: \"Run the scripted behavior (destructive).\",\n"
    "      annotations: { destructive: true },\n"
    "      params: [\n"
    "        #param { name: \"msg\", type: #type { kind: \"string\" },"
    " required: true },\n"
    "      ],\n"
    "    },\n";

static const char FIX_FS_COMMANDS[] =
    "    #command {\n"
    "      name: \"write\",\n"
    "      summary: \"Write a generated UTF-8 file payload.\",\n"
    "      annotations: { destructive: true },\n"
    "      params: [\n"
    "        #param { name: \"path\", type: #type { kind: \"path\", "
    "access: \"write\" }, required: true },\n"
    "        #param { name: \"content\", type: #type { kind: \"string\" "
    "}, required: true },\n"
    "      ],\n"
    "    },\n";

int asngn_fix_registry(const char *root, const char *tool_id,
                       const char *tool_bin_abs, const char *behavior) {
  asngn_buf b;
  asngn_err e;
  char *pkg_dir = NULL, *manifest_path = NULL;
  int ok = 0;

  if (!root || !tool_id || !tool_bin_abs) return 0;
  if (!behavior) behavior = "echo";

  asngn_buf_init(&b);
  e = asngn_buf_appends(&b,
                        "#astools_tool {\n"
                        "  manifest_version: 1,\n"
                        "  id: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, tool_id);
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          "\",\n"
                          "  version: \"1.0.0\",\n"
                          "  title: \"Fake tool\",\n"
                          "  summary: \"Deterministic scripted tool for "
                          "the asngn test-suite.\",\n"
                          "  kind: \"executable\",\n");
  if (e == ASNGN_OK)
    e = asngn_buf_printf(&b, "  platforms: [\"%s\"],\n", fix_plat_os());
  if (e == ASNGN_OK)
    e = asngn_buf_printf(&b,
                         "  runtime: {\n"
                         "    mode: \"oneshot\",\n"
                         "    entry: [\n"
                         "      { os: \"%s\", arch: \"%s\", argv: [\"",
                         fix_plat_os(), fix_plat_arch());
  if (e == ASNGN_OK) e = append_escaped(&b, tool_bin_abs);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "\", \"");
  if (e == ASNGN_OK) e = append_escaped(&b, behavior);
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          "\"] },\n"
                          "    ],\n"
                          "  },\n"
                          "  permissions: {\n"
                          "    fs: [ { path: \"${workspace}\", access: "
                          "\"read-write\" } ],\n"
                          "    net: false,\n"
                          "    proc: false,\n"
                          "    env: [],\n"
                          "  },\n"
                          "  commands: [\n");
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          strcmp(tool_id, "fs") == 0 ? FIX_FS_COMMANDS
                                                     : FIX_COMMANDS);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "  ],\n}\n");
  if (e != ASNGN_OK) goto done;

  pkg_dir = os_path_join(root, tool_id);
  if (!pkg_dir) goto done;
  if (os_mkdir_p(pkg_dir) != ASNGN_OK) goto done;
  manifest_path = os_path_join(pkg_dir, "manifest.xcdn");
  if (!manifest_path) goto done;
  ok = write_text(manifest_path, &b);

done:
  free(pkg_dir);
  free(manifest_path);
  asngn_buf_free(&b);
  return ok;
}

/* ---- astools config ------------------------------------------------------ */

int asngn_fix_astools_config(const char *path, const char *registry_root,
                             const char *workspace) {
  asngn_buf b;
  asngn_err e;
  int ok = 0;

  if (!path || !registry_root || !workspace) return 0;

  asngn_buf_init(&b);
  e = asngn_buf_appends(&b,
                        "#astools_config {\n"
                        "  registry: {\n"
                        "    paths: [ { path: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, registry_root);
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          "\", trust: \"full\" } ],\n"
                          "    watch: \"off\",\n"
                          "    pinning: \"off\",\n"
                          "  },\n"
                          "  workspace: { root: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, workspace);
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          "\" },\n"
                          "  logging: { level: \"debug\" },\n"
#ifdef ASNGN_TEST_SANITIZERS
                          /* ASan reserves terabytes of virtual address space;
                           * RLIMIT_AS would prevent the instrumented fake tool
                           * from starting before its test code runs. */
                          "  sandbox: { default_level: \"none\" },\n"
#endif
                          "}\n");
  if (e != ASNGN_OK) goto done;

  ok = write_text(path, &b);

done:
  asngn_buf_free(&b);
  return ok;
}

/* ---- engine config ------------------------------------------------------- */

static int fix_engine_config(const char *path, const char *astools_cfg_path,
                             const char *registry_root,
                             const char *workspace, const char *extra,
                             int asper_enable) {
  asngn_buf b;
  asngn_err e;
  int ok = 0;

  if (!path || !astools_cfg_path || !registry_root || !workspace) return 0;

  asngn_buf_init(&b);
  e = asngn_buf_appends(&b,
                        "#asngn_config {\n"
                        "  models: {\n"
                        "    pool: " ASNGN_FIX_POOL ",\n"
                        "  },\n"
                        "  integration: {\n"
                        "    asper: { enable: ");
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b, asper_enable ? "true, root: \"memory\" },\n"
                                              : "false },\n");
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                        "    astools: {\n"
                        "      enable: true,\n"
                        "      root: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, registry_root);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "\",\n      workspace: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, workspace);
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "\",\n      config: \"");
  if (e == ASNGN_OK) e = append_escaped(&b, astools_cfg_path);
  if (e == ASNGN_OK)
    e = asngn_buf_appends(&b,
                          "\",\n"
                          "    },\n"
                          "  },\n"
                          "  validation: { judge: \"off\" },\n"
                          "  cache: { enable: false },\n"
                          "  routing: { classifier: \"model\" },\n");
  if (e == ASNGN_OK && extra != NULL) {
    e = asngn_buf_appends(&b, "  ");
    if (e == ASNGN_OK) e = asngn_buf_appends(&b, extra);
    if (e == ASNGN_OK) e = asngn_buf_appendc(&b, '\n');
  }
  if (e == ASNGN_OK) e = asngn_buf_appends(&b, "}\n");
  if (e != ASNGN_OK) goto done;

  ok = write_text(path, &b);

done:
  asngn_buf_free(&b);
  return ok;
}

int asngn_fix_engine_config(const char *path, const char *astools_cfg_path,
                            const char *registry_root, const char *workspace,
                            const char *extra) {
  return fix_engine_config(path, astools_cfg_path, registry_root, workspace,
                           extra, 0);
}

int asngn_fix_engine_config_asper(const char *path,
                                  const char *astools_cfg_path,
                                  const char *registry_root,
                                  const char *workspace,
                                  const char *extra) {
  return fix_engine_config(path, astools_cfg_path, registry_root, workspace,
                           extra, 1);
}
