/* A linked worktree must point back to this checkout and its common Git store. */
#include "workspace_git.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
#ifdef _WIN32
static bool path_identity(const char *path, BY_HANDLE_FILE_INFORMATION *info) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (!n) return false;
  wchar_t *wide = malloc((size_t)n * sizeof *wide);
  if (!wide) return false;
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) {
    free(wide);
    return false;
  }
  HANDLE handle = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) return false;
  bool ok = GetFileInformationByHandle(handle, info) != 0;
  CloseHandle(handle);
  return ok;
}
#endif
static bool same_path(const char *actual, const char *expected) {
#ifdef _WIN32
  /* GetFullPathName does not expand 8.3 aliases or normalize path casing.
   * Compare the opened objects, so a Git-written long path can match TEMP's
   * short spelling without accepting a different worktree registration. */
  BY_HANDLE_FILE_INFORMATION a, b;
  return path_identity(actual, &a) && path_identity(expected, &b) &&
         a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
         a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
#else
  return !strcmp(actual, expected);
#endif
}
static asngn_err points_to(const char *directory, const char *name, const char *expected) {
  char *text = NULL, *actual = NULL;
  asngn_err e = asngn_git_read(directory, name, ASNGN_GIT_LINE_MAX, &text);
  if (e == ASNGN_OK) e = asngn_git_line(text);
  if (e == ASNGN_OK) {
    actual = canonical(directory, text);
    if (!actual || !same_path(actual, expected)) e = ASNGN_ERR_DENIED;
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
