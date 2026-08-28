/* workspace.c — canonical coding-workspace identity and live fingerprint. */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

static int name_cmp(const void *a, const void *b) {
  const char *const *sa = (const char *const *)a;
  const char *const *sb = (const char *const *)b;
  return strcmp(*sa, *sb);
}

static bool skipped_dir(const char *name) {
  static const char *const skip[] = {
      ".git", ".hg", ".svn", ".asterism", "node_modules", "build",
      "out", "dist", "target", "__pycache__", NULL};
  size_t i;
  for (i = 0; skip[i] != NULL; i++)
    if (strcmp(name, skip[i]) == 0) return true;
  return false;
}

/* Code-language census by file extension, tallied during the fingerprint
 * walk. .h counts toward C — indistinguishable from C++ headers without
 * parsing, and close enough for a routing signal. */
static const struct { const char *ext; const char *lang; } ws_langs[] = {
    {"c", "c"},      {"h", "c"},       {"cpp", "cpp"},  {"cc", "cpp"},
    {"cxx", "cpp"},  {"hpp", "cpp"},   {"hh", "cpp"},   {"py", "python"},
    {"rs", "rust"},  {"go", "go"},     {"js", "js"},    {"jsx", "js"},
    {"mjs", "js"},   {"ts", "ts"},     {"tsx", "ts"},   {"java", "java"},
    {"cs", "csharp"},{"rb", "ruby"},   {"php", "php"},  {"sh", "shell"},
    {"swift", "swift"}, {"kt", "kotlin"}, {"zig", "zig"}, {NULL, NULL}};

#define WS_LANG_N (sizeof ws_langs / sizeof ws_langs[0] - 1)

typedef struct {
  size_t files, bytes;
  size_t ext_count[WS_LANG_N];
} ws_scan;

static void ws_scan_file(ws_scan *scan, const char *name, size_t len) {
  const char *dot = strrchr(name, '.');
  size_t i;
  scan->files++;
  scan->bytes += len;
  if (dot == NULL || dot[1] == '\0') return;
  for (i = 0; i < WS_LANG_N; i++) {
    const char *e = ws_langs[i].ext, *p = dot + 1;
    while (*e != '\0' && *p != '\0' &&
           (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p) == *e) {
      e++; p++;
    }
    if (*e == '\0' && *p == '\0') { scan->ext_count[i]++; return; }
  }
}

static void ws_scan_finish(const ws_scan *scan, asngn_repo_stats *out) {
  /* dominant language = argmax of per-language counts (extensions that
   * map to one language pool their tallies) */
  size_t lang_count[WS_LANG_N];
  size_t i, j, best = 0, best_count = 0;
  memset(lang_count, 0, sizeof lang_count);
  for (i = 0; i < WS_LANG_N; i++) {
    for (j = 0; j <= i; j++)
      if (strcmp(ws_langs[j].lang, ws_langs[i].lang) == 0) break;
    lang_count[j] += scan->ext_count[i];
  }
  out->files = scan->files;
  out->bytes = scan->bytes;
  out->language[0] = '\0';
  for (i = 0; i < WS_LANG_N; i++) {
    if (lang_count[i] > best_count) { best = i; best_count = lang_count[i]; }
  }
  if (best_count > 0)
    snprintf(out->language, sizeof out->language, "%s", ws_langs[best].lang);
  out->loaded = true;
}

static void hash_tree(asngn_sha256_ctx *h, ws_scan *scan, const char *root,
                      const char *relative, unsigned depth) {
  char *dir = relative[0] != '\0' ? os_path_join(root, relative)
                                   : asngn_strdup(root);
  char **files = NULL, **dirs = NULL;
  size_t nf = 0, nd = 0, i;
  if (dir == NULL || depth > 64) { free(dir); return; }
  if (os_list_dir(dir, &files, &nf) != ASNGN_OK) nf = 0;
  if (os_list_dirs(dir, &dirs, &nd) != ASNGN_OK) nd = 0;
  qsort(files, nf, sizeof *files, name_cmp);
  qsort(dirs, nd, sizeof *dirs, name_cmp);
  for (i = 0; i < nf; i++) {
    char *rel = relative[0] != '\0' ? os_path_join(relative, files[i])
                                     : asngn_strdup(files[i]);
    char *full = rel != NULL ? os_path_join(root, rel) : NULL;
    char *data = NULL;
    size_t len = 0;
    if (full != NULL && os_read_file(full, &data, &len) == ASNGN_OK) {
      uint64_t n = (uint64_t)len;
      asngn_sha256_update(h, rel, strlen(rel) + 1);
      asngn_sha256_update(h, &n, sizeof n);
      asngn_sha256_update(h, data, len);
      ws_scan_file(scan, files[i], len);
    }
    free(data); free(full); free(rel); free(files[i]);
  }
  for (i = 0; i < nd; i++) {
    if (!skipped_dir(dirs[i])) {
      char *rel = relative[0] != '\0' ? os_path_join(relative, dirs[i])
                                       : asngn_strdup(dirs[i]);
      if (rel != NULL) hash_tree(h, scan, root, rel, depth + 1);
      free(rel);
    }
    free(dirs[i]);
  }
  free(files); free(dirs); free(dir);
}

static void copy_trimmed(char *dst, size_t cap, const char *src) {
  size_t n = src != NULL ? strcspn(src, "\r\n") : 0;
  if (n >= cap) n = cap - 1;
  memcpy(dst, src != NULL ? src : "", n);
  dst[n] = '\0';
}

static char *find_repository_root(const char *workspace) {
  char *cur = asngn_strdup(workspace);
  if (cur == NULL) return NULL;
  for (;;) {
    char *marker = os_path_join(cur, ".git");
    int found = marker != NULL && os_file_exists(marker);
    char *a, *b, *slash;
    free(marker);
    if (found) return cur;
    a = strrchr(cur, '/');
    b = strrchr(cur, '\\');
    slash = a == NULL ? b : (b == NULL || a > b ? a : b);
    if (slash == NULL || slash == cur ||
        (slash == cur + 2 && cur[1] == ':')) break;
    *slash = '\0';
  }
  free(cur);
  return asngn_strdup(workspace);
}

static char *resolve_git_dir(const char *repository_root) {
  char *marker = os_path_join(repository_root, ".git");
  char *text = NULL, *path = NULL, *real = NULL;
  size_t len = 0;
  if (marker == NULL) return NULL;
  if (os_read_file(marker, &text, &len) == ASNGN_OK &&
      strncmp(text, "gitdir:", 7) == 0) {
    char *value = text + 7;
    char *end;
    while (*value == ' ' || *value == '\t') value++;
    end = value + strcspn(value, "\r\n");
    *end = '\0';
    path = os_path_is_abs(value) ? asngn_strdup(value)
                                 : os_path_join(repository_root, value);
    if (path != NULL) real = os_realpath(path);
  }
  free(path);
  free(text);
  if (real != NULL) { free(marker); return real; }
  return marker;
}

static void packed_ref(const char *git, const char *ref, char *out,
                       size_t out_cap) {
  char *path = os_path_join(git, "packed-refs");
  char *text = NULL, *line;
  if (path == NULL || os_read_file(path, &text, NULL) != ASNGN_OK) {
    free(path); free(text); return;
  }
  line = text;
  while (*line != '\0') {
    char *end = strpbrk(line, "\r\n");
    char *space = strchr(line, ' ');
    if (end == NULL) end = line + strlen(line);
    if (space != NULL && space < end &&
        (size_t)(end - space - 1) == strlen(ref) &&
        memcmp(space + 1, ref, strlen(ref)) == 0) {
      size_t n = (size_t)(space - line);
      if (n >= out_cap) n = out_cap - 1;
      memcpy(out, line, n); out[n] = '\0';
      break;
    }
    line = end;
    while (*line == '\r' || *line == '\n') line++;
  }
  free(text); free(path);
}

static void git_identity(asngn_workspace_info *w) {
  char *git = resolve_git_dir(w->repository_root);
  char *headp = git != NULL ? os_path_join(git, "HEAD") : NULL;
  char *text = NULL;
  size_t len = 0;
  if (headp != NULL && os_read_file(headp, &text, &len) == ASNGN_OK) {
    if (strncmp(text, "ref: ", 5) == 0) {
      const char *ref = text + 5;
      const char *slash = strrchr(ref, '/');
      char refbuf[512];
      copy_trimmed(w->branch, sizeof w->branch, slash != NULL ? slash + 1 : ref);
      copy_trimmed(refbuf, sizeof refbuf, ref);
      {
        char *rp = os_path_join(git, refbuf);
        char *oid = NULL;
        if (rp != NULL && os_read_file(rp, &oid, NULL) == ASNGN_OK)
          copy_trimmed(w->head, sizeof w->head, oid);
        else
          packed_ref(git, refbuf, w->head, sizeof w->head);
        free(oid); free(rp);
      }
    } else {
      copy_trimmed(w->head, sizeof w->head, text);
      snprintf(w->branch, sizeof w->branch, "detached");
    }
  }
  free(text); free(headp); free(git);
}

static void hash_optional_file(asngn_sha256_ctx *h, const char *root,
                               const char *name) {
  char *path = os_path_join(root, name);
  char *data = NULL;
  size_t len = 0;
  asngn_sha256_update(h, name, strlen(name) + 1);
  if (path != NULL && os_read_file(path, &data, &len) == ASNGN_OK) {
    uint64_t n = (uint64_t)len;
    asngn_sha256_update(h, &n, sizeof n);
    asngn_sha256_update(h, data, len);
  }
  free(data); free(path);
}

static void derive_project(char out[65], const char *root) {
  const char *p = root + strlen(root);
  size_t n = 0;
  while (p > root && (p[-1] == '/' || p[-1] == '\\')) p--;
  { const char *end = p; while (p > root && p[-1] != '/' && p[-1] != '\\') p--;
    while (p < end && n < 64) {
      unsigned char ch = (unsigned char)*p++;
      out[n++] = (char)(isalnum(ch) ? tolower(ch) : '-');
    }
  }
  while (n > 0 && out[n - 1] == '-') n--;
  if (n == 0) { memcpy(out, "workspace", 9); n = 9; }
  out[n] = '\0';
}

static void detect_adapter(asngn_workspace_info *w) {
  static const struct { const char *file; const char *name; } candidates[] = {
      {"CMakeLists.txt", "cmake"}, {"Cargo.toml", "cargo"},
      {"package.json", "npm"}, {"pyproject.toml", "python"},
      {"go.mod", "go"}, {"Makefile", "make"}, {NULL, NULL}};
  size_t i;
  for (i = 0; candidates[i].file != NULL; i++) {
    char *p = os_path_join(w->repository_root, candidates[i].file);
    int exists = p != NULL && os_file_exists(p);
    free(p);
    if (exists) { snprintf(w->build_adapter, sizeof w->build_adapter, "%s",
                           candidates[i].name); return; }
  }
  snprintf(w->build_adapter, sizeof w->build_adapter, "none");
}

asngn_err asngn_workspace_info_refresh(asngn_ctx *c,
                                       asngn_workspace_info *workspace) {
  asngn_sha256_ctx h;
  uint8_t digest[32];
  ws_scan scan;
  if (c == NULL || workspace == NULL ||
      workspace->canonical_root[0] == '\0')
    return ASNGN_ERR_INVALID;
  memset(&scan, 0, sizeof scan);
  workspace->head[0] = workspace->branch[0] = '\0';
  git_identity(workspace);
  asngn_sha256_init(&h);
  asngn_sha256_update(&h, "asngn-workspace-v1", 18);
  asngn_sha256_update(&h, workspace->canonical_root,
                      strlen(workspace->canonical_root) + 1);
  asngn_sha256_update(&h, workspace->repository_root,
                      strlen(workspace->repository_root) + 1);
  asngn_sha256_update(&h, workspace->head, strlen(workspace->head) + 1);
  asngn_sha256_update(&h, workspace->branch,
                      strlen(workspace->branch) + 1);
  asngn_sha256_update(&h, workspace->project_id,
                      strlen(workspace->project_id) + 1);
  asngn_sha256_update(&h, workspace->ignore_rules,
                      strlen(workspace->ignore_rules) + 1);
  asngn_sha256_update(&h, workspace->build_adapter,
                      strlen(workspace->build_adapter) + 1);
  hash_optional_file(&h, workspace->repository_root, ".gitignore");
  hash_optional_file(&h, workspace->canonical_root, ".asterismignore");
  hash_tree(&h, &scan, workspace->canonical_root, "", 0);
  asngn_sha256_final(&h, digest);
  asngn_sha256_hex(digest, sizeof digest, workspace->fingerprint);
  ws_scan_finish(&scan, &c->repo_stats);
  return ASNGN_OK;
}

asngn_err asngn_workspace_refresh(asngn_ctx *c) {
  if (c == NULL) return ASNGN_ERR_INVALID;
  return asngn_workspace_info_refresh(c, &c->workspace);
}

static asngn_err workspace_info_build(asngn_ctx *c, const char *selected,
                                      const char *project_id,
                                      const char *build_adapter,
                                      asngn_workspace_info *out) {
  char *joined = NULL, *real = NULL, *repository = NULL;
  asngn_err e = ASNGN_OK;
  if (c == NULL || out == NULL || selected == NULL || selected[0] == '\0')
    return asngn_seterr(c, ASNGN_ERR_CONFIG, "workspace root is required");
  joined = os_path_is_abs(selected) ? asngn_strdup(selected)
                                    : os_path_join(c->root, selected);
  if (joined == NULL) return ASNGN_ERR_NOMEM;
  if (os_mkdir_p(joined) != ASNGN_OK || (real = os_realpath(joined)) == NULL) {
    free(joined);
    return asngn_seterr(c, ASNGN_ERR_IO, "workspace cannot be canonicalized");
  }
  free(joined);
  if (strlen(real) >= sizeof out->canonical_root) {
    free(real);
    return asngn_seterr(c, ASNGN_ERR_INVALID, "workspace path too long");
  }
  memset(out, 0, sizeof *out);
  snprintf(out->canonical_root, sizeof out->canonical_root, "%s", real);
  repository = find_repository_root(real);
  if (repository == NULL || strlen(repository) >= sizeof out->repository_root) {
    free(repository);
    free(real);
    return asngn_seterr(c, ASNGN_ERR_INVALID, "repository path too long");
  }
  snprintf(out->repository_root, sizeof out->repository_root, "%s",
           repository);
  snprintf(out->ignore_rules, sizeof out->ignore_rules,
           ".gitignore;.asterismignore;built-in-generated-dirs");
  if (project_id != NULL && project_id[0] != '\0')
    snprintf(out->project_id, sizeof out->project_id, "%s", project_id);
  else
    derive_project(out->project_id, real);
  if (build_adapter != NULL && build_adapter[0] != '\0')
    snprintf(out->build_adapter, sizeof out->build_adapter, "%s",
             build_adapter);
  else
    detect_adapter(out);
  free(repository);
  free(real);
  e = asngn_workspace_info_refresh(c, out);
  return e;
}

asngn_err asngn_workspace_info_init(asngn_ctx *c, const char *root,
                                    asngn_workspace_info *out) {
  return workspace_info_build(c, root, NULL, NULL, out);
}

asngn_err asngn_workspace_init(asngn_ctx *c, const asngn_open_params *p) {
  const char *selected = p->workspace_root != NULL ? p->workspace_root
                                                   : c->cfg.astools_workspace;
  /* "session" is a mode, not a literal directory.  The engine opens
   * astools against the sessions container for startup readiness and binds
   * it to sessions/<slug>/workspace before each turn. */
  if (p->workspace_root == NULL && selected != NULL &&
      strcmp(selected, "session") == 0)
    selected = "sessions";
  return workspace_info_build(c, selected, p->project_id, p->build_adapter,
                              &c->workspace);
}

void asngn_workspace_hash(asngn_ctx *c, uint8_t out[32]) {
  size_t i;
  memset(out, 0, 32);
  if (c == NULL || asngn_workspace_refresh(c) != ASNGN_OK) return;
  for (i = 0; i < 32; i++) {
    char pair[3] = {c->workspace.fingerprint[i * 2],
                    c->workspace.fingerprint[i * 2 + 1], 0};
    out[i] = (uint8_t)strtoul(pair, NULL, 16);
  }
}
