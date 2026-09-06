/* Repository evidence uses the same authorized enumeration as fingerprints. */
#include "retrieval_select.h"
#include "workspace_tree.h"
#include <stdlib.h>
#include <string.h>

void asngn_code_index_free(void *ptr) {
  code_index *ix = ptr;
  if (!ix)
    return;
  for (size_t i = 0; i < ix->n; i++) {
    free(ix->v[i].path);
    free(ix->v[i].text);
    free(ix->v[i].vec);
  }
  free(ix->v);
  free(ix);
}
static bool code_file(const char *path) {
  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *ext = strrchr(name, '.');
  static const char *const allowed[] = {".c",    ".h",   ".cpp",  ".hpp",   ".rs",   ".go", ".py",
                                        ".js",   ".jsx", ".ts",   ".tsx",   ".java", ".cs", ".sh",
                                        ".lua",  ".rb",  ".sql",  ".swift", ".kt",   ".md", ".json",
                                        ".yaml", ".yml", ".toml", ".cmake"};
  if (!strcmp(name, "CMakeLists.txt") || !strcmp(name, "Makefile") || !strcmp(name, "Dockerfile"))
    return true;
  if (!ext || name[0] == '.')
    return false;
  for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
    if (!strcmp(ext, allowed[i]))
      return true;
  return false;
}
bool asngn_code_stopped(asngn_ctx *c, asngn_turn_state *t) {
  return t->cancel || (t->deadline_mono > 0 && asngn_clock_mono_ms(&c->clock) >= t->deadline_mono);
}
static bool scan_stopped(void *userdata) {
  code_selection *s = userdata;
  return asngn_code_stopped(s->c, s->turn);
}
static asngn_err scan_file(const char *path, FILE *file, size_t bytes, void *userdata) {
  code_selection *s = userdata;
  bool recheck = s->active_read && !strcmp(path, s->turn->s->active_file);
  if (!recheck && !code_file(path))
    return ASNGN_OK;
  if (bytes > CODE_FILE_BYTES) {
    s->skipped_large++;
    return recheck ? ASNGN_ERR_BUSY : ASNGN_OK;
  }
  /* Reserve one full active-file read, even if it occurs after budget pressure. */
  size_t reserve = s->active_read && !s->active_rechecked && !recheck ? CODE_FILE_BYTES : 0;
  size_t cap = CODE_SCAN_BYTES - reserve;
  if (s->read_bytes > cap || bytes > cap - s->read_bytes) {
    s->skipped_budget++;
    return ASNGN_OK;
  }
  char *text = NULL;
  size_t len = 0;
  asngn_err e = asngn_tree_read(file, bytes, CODE_FILE_BYTES, &text, &len);
  if (e == ASNGN_OK) {
    s->read_bytes += len;
    s->read_files++;
    e = asngn_code_select_file(s, path, text, len, false);
    if (recheck && e == ASNGN_OK)
      s->active_rechecked = true;
  }
  free(text);
  return e;
}
asngn_err asngn_code_scan(asngn_ctx *c, asngn_turn_state *t, code_index *ix) {
  const char *active = t->s->active_file;
  const char *root = t->s->workspace.canonical_root;
  code_selection state = {.c = c, .turn = t, .index = ix};
  asngn_code_terms(&state, t->retrieval_query);
  if (active && *active && !asngn_tree_ignored_path(active) && !asngn_code_stopped(c, t)) {
    char *text = NULL;
    size_t len = 0;
    if (asngn_workspace_read(root, active, CODE_FILE_BYTES, &text, &len) == ASNGN_OK) {
      state.read_bytes += len;
      state.read_files++;
      asngn_err e = asngn_code_select_file(&state, active, text, len, true);
      free(text);
      if (e != ASNGN_OK)
        return e;
    }
  }
  asngn_tree_limits limits = asngn_tree_default_limits;
  /* Metadata for non-code files consumes no content budget. */
  limits.file_bytes = limits.total_bytes = SIZE_MAX;
  asngn_tree_stats stats = {0};
  asngn_err walk = asngn_tree_walk(root, &limits, scan_file, scan_stopped, &state, &stats);
  if (walk == ASNGN_ERR_CANCELLED && !t->cancel)
    walk = ASNGN_ERR_TIMEOUT;
  if ((walk == ASNGN_OK || walk == ASNGN_ERR_LIMIT) && state.active_read && !state.active_rechecked)
    walk = ASNGN_ERR_BUSY;
  bool partial = state.candidates > ix->n || state.skipped_large || state.skipped_binary ||
                 state.skipped_budget || stats.excluded;
  asngn_err e = walk == ASNGN_OK && partial ? ASNGN_ERR_LIMIT : walk;
  asngn_code_selection_sort(&state);
  char trace[768];
  snprintf(
      trace, sizeof trace,
      "{schema:2, policy:\"code-admission-v2\", entries:%zu, files:%zu, excluded:%zu, ignored:%zu, "
      "read_bytes:%zu, reads:%zu, candidates:%zu, chunks:%zu, pinned:%zu, skipped_large:%zu, "
      "skipped_binary:%zu, skipped_budget:%zu, query_terms:%zu, query_truncated:%s, "
      "traversal_complete:%s, complete:%s, error:\"%s\"}",
      stats.entries, stats.files, stats.excluded, stats.ignored, state.read_bytes, state.read_files,
      state.candidates, ix->n, state.pinned, state.skipped_large, state.skipped_binary,
      state.skipped_budget, state.term_count, state.terms_truncated ? "true" : "false",
      walk == ASNGN_OK ? "true" : "false", e == ASNGN_OK ? "true" : "false", asngn_err_name(e));
  asngn_tele_emit(c, "retrieval_scan", NULL, NULL, t->s->slug, t->led.turn, trace);
  return e;
}
