/* Repository evidence uses the same authorized enumeration as fingerprints. */
#include "retrieval.h"
#include "workspace_tree.h"
#include <stdlib.h>
#include <string.h>

void asngn_code_index_free(void *ptr) {
  code_index *ix = ptr;
  if (!ix) return;
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
  if (!ext || name[0] == '.') return false;
  for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
    if (!strcmp(ext, allowed[i])) return true;
  return false;
}
bool asngn_code_stopped(asngn_ctx *c, asngn_turn_state *t) {
  return t->cancel || (t->deadline_mono > 0 && asngn_clock_mono_ms(&c->clock) >= t->deadline_mono);
}
static asngn_err chunks_add(code_index *ix, const char *path, const char *data, size_t len) {
  if (memchr(data, 0, len) || !asngn_utf8_valid(data, len)) return ASNGN_OK;
  uint8_t file_hash[32];
  asngn_sha256(data, len, file_hash);
  for (size_t i = 0; i < ix->n; i++)
    if (!strcmp(ix->v[i].path, path))
      return memcmp(ix->v[i].file_hash, file_hash, 32) ? ASNGN_ERR_BUSY : ASNGN_OK;
  size_t off = 0, line = 1;
  while (off < len && ix->n < CODE_MAX) {
    size_t end = len - off > CODE_CHUNK ? off + CODE_CHUNK : len;
    if (end < len) {
      while (end > off && data[end - 1] != '\n')
        end--;
      if (end == off) end = off + CODE_CHUNK;
    }
    while (end < len && ((unsigned char)data[end] & 0xc0) == 0x80)
      end++;
    chunk *v = &ix->v[ix->n];
    v->path = asngn_strdup(path);
    v->text = asngn_strndup(data + off, end - off);
    v->line = line;
    v->offset = off;
    if (!v->path || !v->text) {
      free(v->path);
      free(v->text);
      memset(v, 0, sizeof *v);
      return ASNGN_ERR_NOMEM;
    }
    memcpy(v->file_hash, file_hash, 32);
    asngn_sha256(v->text, end - off, v->hash);
    ix->n++;
    for (; off < end; off++)
      if (data[off] == '\n') line++;
    v->end_line = data[end - 1] == '\n' ? line - 1 : line;
  }
  return off < len ? ASNGN_ERR_LIMIT : ASNGN_OK;
}
typedef struct {
  asngn_ctx *c;
  asngn_turn_state *t;
  code_index *ix;
} scan_state;
static bool scan_stopped(void *userdata) {
  scan_state *s = userdata;
  return asngn_code_stopped(s->c, s->t);
}
static asngn_err scan_file(const char *path, FILE *file, size_t bytes, void *userdata) {
  scan_state *s = userdata;
  if (s->ix->n >= CODE_MAX) return ASNGN_ERR_LIMIT;
  if (!code_file(path) || bytes > 262144) return ASNGN_OK;
  char *text = NULL;
  size_t len = 0;
  asngn_err e = asngn_tree_read(file, bytes, 262144, &text, &len);
  if (e == ASNGN_OK) e = chunks_add(s->ix, path, text, len);
  free(text);
  return e;
}
asngn_err asngn_code_scan(asngn_ctx *c, asngn_turn_state *t, code_index *ix) {
  const char *active = t->s->active_file;
  const char *root = t->s->workspace.canonical_root;
  if (active && *active && !asngn_tree_ignored_path(active) && !asngn_code_stopped(c, t)) {
    char *text = NULL;
    size_t len = 0;
    if (asngn_workspace_read(root, active, 262144, &text, &len) == ASNGN_OK) {
      asngn_err e = chunks_add(ix, active, text, len);
      free(text);
      if (e != ASNGN_OK) return e;
    }
  }
  scan_state state = {c, t, ix};
  asngn_tree_limits limits = asngn_tree_default_limits;
  /* Metadata for large non-code files consumes no content budget. */
  limits.file_bytes = limits.total_bytes = SIZE_MAX;
  asngn_tree_stats stats = {0};
  asngn_err e = asngn_tree_walk(root, &limits, scan_file, scan_stopped, &state, &stats);
  if (e == ASNGN_ERR_CANCELLED && !t->cancel) e = ASNGN_ERR_TIMEOUT;
  char trace[384];
  snprintf(
      trace, sizeof trace,
      "{schema:1, policy:\"workspace-tree-v1\", entries:%zu, files:%zu, excluded:%zu, ignored:%zu, "
      "chunks:%zu, complete:%s, error:\"%s\"}",
      stats.entries, stats.files, stats.excluded, stats.ignored, ix->n,
      e == ASNGN_OK ? "true" : "false", asngn_err_name(e));
  asngn_tele_emit(c, "retrieval_scan", NULL, NULL, t->s->slug, t->led.turn, trace);
  return e;
}
