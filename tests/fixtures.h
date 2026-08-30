/*
 * fixtures.h — sibling-fixture writers for the asngn integration tests,
 * modeled on astools' tests/fakes.c writers.
 *
 * Three plain-text emitters: an astools registry package around the
 * scripted asngn_fake_tool binary, an #astools_config with one FULL-trust
 * root (absolute argv[0] entries require full trust, astools), and
 * the engine's own #asngn_config wiring the 4-fake pool, the astools
 * integration, and any caller-supplied extra sections.
 *
 * All paths are taken and written verbatim — pass absolute paths
 * (integration.astools.* and registry paths pass absolute values through
 * root_join / sib_join untouched).
 *
 * MIT License — per aspera ad astra.
 */

#ifndef ASNGN_FIXTURES_H
#define ASNGN_FIXTURES_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The models.pool array literal written by asngn_fix_engine_config,
 * exposed so a test that must REPLACE the whole models section via
 * `extra` (xCDN duplicate keys replace, so a second top-level models
 * section must re-declare the pool) stays in sync with the fixture:
 *
 *   "models: { pool: " ASNGN_FIX_POOL ", roles: { generator: \"light\" } },"
 */
#define ASNGN_FIX_POOL                                                     \
  "[\n"                                                                    \
  "      { id: \"nano\",  path: \"fake-nano.gguf\",  ctx: 8192 },\n"       \
  "      { id: \"light\", path: \"fake-light.gguf\", ctx: 32768 },\n"      \
  "      { id: \"std\",   path: \"fake-std.gguf\",   ctx: 32768 },\n"      \
  "      { id: \"embed\", path: \"fake-embed.gguf\", ctx: 512,"            \
  " embedding: true, dim: 16 },\n"                                         \
  "    ]"

/*
 * Write <root>/<tool_id>/manifest.xcdn: an executable oneshot tool whose
 * runtime entry (current platform) is [tool_bin_abs, behavior], with
 * permissions fs ${workspace} read-write and TWO commands:
 *
 *   "run" {msg: string required}  annotations { read_only, idempotent }
 *   "mut" {msg: string required}  annotations { destructive }
 *
 * 1 on success, 0 on failure.
 */
int asngn_fix_registry(const char *root, const char *tool_id,
                       const char *tool_bin_abs, const char *behavior);

/*
 * Write an #astools_config at `path`: one FULL-trust registry root
 * (needed because the fake tool's argv[0] is absolute), watch off,
 * pinning off, workspace root as given. 1 on success, 0 on failure.
 */
int asngn_fix_astools_config(const char *path, const char *registry_root,
                             const char *workspace);

/*
 * Write the engine config.xcdn at `path`: models.pool of 4 (nano / light
 * / std / embed dim 16, see ASNGN_FIX_POOL), integration asper off +
 * astools on (root / workspace / config as given, absolute), validation
 * judge off, cache disabled, routing.classifier "model". `extra` (may be
 * NULL) is appended verbatim before the closing brace: additional
 * top-level sections, each ending with a comma. 1 on success, 0 on
 * failure.
 */
int asngn_fix_engine_config(const char *path, const char *astools_cfg_path,
                            const char *registry_root, const char *workspace,
                            const char *extra);
/* Same fixture, with Asper enabled and its store rooted at `memory`. */
int asngn_fix_engine_config_asper(const char *path,
                                  const char *astools_cfg_path,
                                  const char *registry_root,
                                  const char *workspace,
                                  const char *extra);

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_FIXTURES_H */
