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

static void hash_tree(asngn_sha256_ctx *h, const char *root,
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
    }
    free(data); free(full); free(rel); free(files[i]);
  }
  for (i = 0; i < nd; i++) {
    if (!skipped_dir(dirs[i])) {
      char *rel = relative[0] != '\0' ? os_path_join(relative, dirs[i])
                                       : asngn_strdup(dirs[i]);
      if (rel != NULL) hash_tree(h, root, rel, depth + 1);
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

asngn_err asngn_workspace_refresh(asngn_ctx *c) {
  asngn_sha256_ctx h;
  uint8_t digest[32];
  if (c == NULL || c->workspace.canonical_root[0] == '\0')
    return ASNGN_ERR_INVALID;
  c->workspace.head[0] = c->workspace.branch[0] = '\0';
  git_identity(&c->workspace);
  asngn_sha256_init(&h);
  asngn_sha256_update(&h, "asngn-workspace-v1", 18);
  asngn_sha256_update(&h, c->workspace.canonical_root,
                      strlen(c->workspace.canonical_root) + 1);
  asngn_sha256_update(&h, c->workspace.repository_root,
                      strlen(c->workspace.repository_root) + 1);
  asngn_sha256_update(&h, c->workspace.head, strlen(c->workspace.head) + 1);
  asngn_sha256_update(&h, c->workspace.branch,
                      strlen(c->workspace.branch) + 1);
  asngn_sha256_update(&h, c->workspace.project_id,
                      strlen(c->workspace.project_id) + 1);
  asngn_sha256_update(&h, c->workspace.ignore_rules,
                      strlen(c->workspace.ignore_rules) + 1);
  asngn_sha256_update(&h, c->workspace.build_adapter,
                      strlen(c->workspace.build_adapter) + 1);
  hash_optional_file(&h, c->workspace.repository_root, ".gitignore");
  hash_optional_file(&h, c->workspace.canonical_root, ".asterismignore");
  hash_tree(&h, c->workspace.canonical_root, "", 0);
  asngn_sha256_final(&h, digest);
  asngn_sha256_hex(digest, sizeof digest, c->workspace.fingerprint);
  return ASNGN_OK;
}

asngn_err asngn_workspace_init(asngn_ctx *c, const asngn_open_params *p) {
  const char *selected = p->workspace_root != NULL ? p->workspace_root
                                                   : c->cfg.astools_workspace;
  char *joined = NULL, *real = NULL, *repository = NULL;
  if (selected == NULL || selected[0] == '\0')
    return asngn_seterr(c, ASNGN_ERR_CONFIG, "workspace root is required");
  joined = os_path_is_abs(selected) ? asngn_strdup(selected)
                                    : os_path_join(c->root, selected);
  if (joined == NULL) return ASNGN_ERR_NOMEM;
  if (os_mkdir_p(joined) != ASNGN_OK || (real = os_realpath(joined)) == NULL) {
    free(joined);
    return asngn_seterr(c, ASNGN_ERR_IO, "workspace cannot be canonicalized");
  }
  free(joined);
  if (strlen(real) >= sizeof c->workspace.canonical_root) {
    free(real); return asngn_seterr(c, ASNGN_ERR_INVALID, "workspace path too long");
  }
  memset(&c->workspace, 0, sizeof c->workspace);
  snprintf(c->workspace.canonical_root, sizeof c->workspace.canonical_root,
           "%s", real);
  repository = find_repository_root(real);
  if (repository == NULL ||
      strlen(repository) >= sizeof c->workspace.repository_root) {
    free(repository); free(real);
    return asngn_seterr(c, ASNGN_ERR_INVALID, "repository path too long");
  }
  snprintf(c->workspace.repository_root, sizeof c->workspace.repository_root,
           "%s", repository);
  snprintf(c->workspace.ignore_rules, sizeof c->workspace.ignore_rules,
           ".gitignore;.asterismignore;built-in-generated-dirs");
  if (p->project_id != NULL && p->project_id[0] != '\0')
    snprintf(c->workspace.project_id, sizeof c->workspace.project_id, "%s",
             p->project_id);
  else derive_project(c->workspace.project_id, real);
  if (p->build_adapter != NULL && p->build_adapter[0] != '\0')
    snprintf(c->workspace.build_adapter, sizeof c->workspace.build_adapter,
             "%s", p->build_adapter);
  else detect_adapter(&c->workspace);
  free(repository); free(real);
  return asngn_workspace_refresh(c);
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
