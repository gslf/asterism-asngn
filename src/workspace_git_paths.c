/* A linked worktree must point back to this checkout and its common Git store. */
#include "workspace_git.h"
#include <stdlib.h>
#include <string.h>

asngn_err asngn_git_read(const char *directory, const char *name, size_t cap, char **out) {
  size_t len = 0;
  asngn_err e;
  *out = NULL;
  if (!directory || !os_path_is_abs(directory)) return ASNGN_ERR_INVALID;
#ifndef _WIN32
  /* Anchor every component, including ancestors of a worktree metadata store. */
  char *path = os_path_join(directory, name);
  if (!path) { *out = NULL; return ASNGN_ERR_NOMEM; }
  e = asngn_workspace_read("/", path[0] == '/' ? path + 1 : path, cap, out, &len);
  free(path);
#else
  e = asngn_workspace_read(directory, name, cap, out, &len);
#endif
  if (e == ASNGN_OK && (memchr(*out, 0, len) || !asngn_utf8_valid(*out, len))) {
    free(*out);
    *out = NULL;
    e = ASNGN_ERR_PARSE;
  }
  return e;
}
asngn_err asngn_git_line(char *text) {
  size_t n = strlen(text);
  if (n && text[n - 1] == '\n') text[--n] = 0;
  if (n && text[n - 1] == '\r') text[--n] = 0;
  return n && !strpbrk(text, "\r\n") ? ASNGN_OK : ASNGN_ERR_PARSE;
}
static char *canonical(const char *base, const char *value) {
  char *path = os_path_is_abs(value) ? asngn_strdup(value) : os_path_join(base, value);
  char *real = path ? os_realpath(path) : NULL;
  free(path);
  if (real && strlen(real) >= ASNGN_WORKSPACE_PATH_MAX) { free(real); return NULL; }
  if (real) for (char *p = real; *p; p++) if (*p == '\\') *p = '/';
  return real;
}
static asngn_err points_to(const char *directory, const char *name, const char *expected) {
  char *text = NULL, *actual = NULL;
  asngn_err e = asngn_git_read(directory, name, ASNGN_GIT_LINE_MAX, &text);
  if (e == ASNGN_OK) e = asngn_git_line(text);
  if (e == ASNGN_OK) {
    actual = canonical(directory, text);
    if (!actual || strcmp(actual, expected)) e = ASNGN_ERR_DENIED;
  }
  free(actual);
  free(text);
  return e;
}
asngn_err asngn_git_directories(const char *repository, char **primary, char **common) {
  char *marker = NULL, *text = NULL, *git = NULL, *shared = NULL;
  *primary = *common = NULL;
  asngn_err e = asngn_git_read(repository, ".git", ASNGN_GIT_LINE_MAX, &text);
  bool marker_present = e != ASNGN_ERR_NOT_FOUND;
  if (e == ASNGN_ERR_DENIED) {
    /* A directory cannot be read as a regular marker. Opening HEAD through the
     * repository descriptor also rejects a symlink in the .git component. */
    e = asngn_git_read(repository, ".git/HEAD", ASNGN_GIT_LINE_MAX, &text);
    if (e == ASNGN_OK) {
      git = os_path_join(repository, ".git");
      shared = git ? asngn_strdup(git) : NULL;
      if (!git || !shared) e = ASNGN_ERR_NOMEM;
      else {
        char *redirect = NULL;
        e = asngn_git_read(git, "commondir", ASNGN_GIT_LINE_MAX, &redirect);
        free(redirect);
        if (e == ASNGN_ERR_NOT_FOUND) e = ASNGN_OK;
        else if (e == ASNGN_OK) e = ASNGN_ERR_UNSUPPORTED;
      }
    } else if (e == ASNGN_ERR_NOT_FOUND) e = ASNGN_ERR_PARSE;
  } else if (e == ASNGN_OK) {
    e = asngn_git_line(text);
    if (e == ASNGN_OK && (strncmp(text, "gitdir: ", 8) || !text[8])) e = ASNGN_ERR_PARSE;
    if (e == ASNGN_OK) {
      git = canonical(repository, text + 8);
      shared = git ? asngn_strdup(git) : NULL;
      marker = canonical(repository, ".git");
      if (!git || !shared || !marker) e = ASNGN_ERR_INVALID;
    }
    if (e == ASNGN_OK) {
      /* Derive the common store from the closed worktrees/<id> layout before
       * reading any path nominated by commondir or a symbolic reference. */
      char *id = strrchr(shared, '/');
      if (!id || !id[1]) e = ASNGN_ERR_UNSUPPORTED;
      else {
        *id = 0;
        char *parent = strrchr(shared, '/');
        if (!parent || strcmp(parent, "/worktrees")) e = ASNGN_ERR_UNSUPPORTED;
        else *parent = 0;
      }
    }
    if (e == ASNGN_OK) e = points_to(git, "gitdir", marker);
    if (e == ASNGN_OK) e = points_to(git, "commondir", shared);
  }
  if (marker_present && e == ASNGN_ERR_NOT_FOUND) e = ASNGN_ERR_PARSE;
  if (e == ASNGN_OK) { *primary = git; *common = shared; }
  else { free(git); free(shared); }
  free(marker);
  free(text);
  return e;
}
