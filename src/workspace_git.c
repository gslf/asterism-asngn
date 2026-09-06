/* Git identity is observed metadata, not proof that an object or tree exists. */
#include "workspace_git.h"
#include <stdlib.h>
#include <string.h>

static bool oid_valid(const char *text) {
  size_t n = strlen(text);
  return (n == 40 || n == 64) && strspn(text, "0123456789abcdef") == n;
}
static bool ref_valid(const char *text) {
  size_t n = strlen(text);
  if (strncmp(text, "refs/", 5) || n >= ASNGN_GIT_LINE_MAX ||
      strstr(text, "..") || strstr(text, "@{") || strpbrk(text, " ~^:?*[\\")) return false;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    if (*p < 32 || *p == 127) return false;
  for (const char *part = text; part;) {
    const char *end = strchr(part, '/');
    size_t len = end ? (size_t)(end - part) : strlen(part);
    if (!len || *part == '.' || part[len - 1] == '.' ||
        (len >= 5 && !memcmp(part + len - 5, ".lock", 5))) return false;
    part = end ? end + 1 : NULL;
  }
  return true;
}
static asngn_err packed_ref(const char *directory, const char *ref, char out[65]) {
  char *text = NULL;
  asngn_err e = asngn_git_read(directory, "packed-refs", ASNGN_GIT_PACKED_MAX, &text);
  if (e == ASNGN_ERR_NOT_FOUND) return ASNGN_OK; /* An unborn branch has no OID. */
  if (e != ASNGN_OK) return e;
  bool found = false;
  for (char *line = text; *line;) {
    char *next = strchr(line, '\n');
    if (next) *next++ = 0;
    size_t len = strlen(line);
    if (len && line[len - 1] == '\r') line[--len] = 0;
    if (*line && *line != '#' && *line != '^') {
      char *space = strchr(line, ' ');
      if (!space) { e = ASNGN_ERR_PARSE; break; }
      *space++ = 0;
      if (!oid_valid(line) || !ref_valid(space)) { e = ASNGN_ERR_PARSE; break; }
      if (!strcmp(space, ref)) {
        if (found) { e = ASNGN_ERR_PARSE; break; }
        strcpy(out, line);
        found = true;
      }
    } else if (*line == '^' && !oid_valid(line + 1)) { e = ASNGN_ERR_PARSE; break; }
    if (!next) break;
    line = next;
  }
  free(text);
  return e;
}
static asngn_err identity(const char *primary, const char *common, asngn_workspace_info *w) {
  char *text = NULL;
  /* Reftable is not the loose/packed ref format understood by this reader. */
  asngn_err e = asngn_git_read(common, "reftable/tables.list", ASNGN_GIT_LINE_MAX, &text);
  free(text);
  if (e == ASNGN_OK || e == ASNGN_ERR_LIMIT) return ASNGN_ERR_UNSUPPORTED;
  if (e != ASNGN_ERR_NOT_FOUND) return e;
  e = asngn_git_read(primary, "HEAD", ASNGN_GIT_LINE_MAX, &text);
  for (size_t hop = 0; e == ASNGN_OK; hop++) {
    e = asngn_git_line(text);
    if (e != ASNGN_OK) break;
    if (strncmp(text, "ref: ", 5)) {
      if (!oid_valid(text)) e = ASNGN_ERR_PARSE;
      else {
        strcpy(w->head, text);
        if (!hop) strcpy(w->branch, "detached");
      }
      break;
    }
    const char *ref = text + 5;
    if (!ref_valid(ref)) { e = ASNGN_ERR_PARSE; break; }
    if (hop >= 8) { e = ASNGN_ERR_LIMIT; break; }
    if (!hop) {
      const char *branch = !strncmp(ref, "refs/heads/", 11) ? ref + 11 : ref;
      if (strlen(branch) >= sizeof w->branch) { e = ASNGN_ERR_LIMIT; break; }
      strcpy(w->branch, branch);
    }
    char *next = NULL;
    const char *store = (!strncmp(ref, "refs/worktree/", 14) ||
                         !strncmp(ref, "refs/bisect/", 12) ||
                         !strncmp(ref, "refs/rewritten/", 15)) ? primary : common;
    e = asngn_git_read(store, ref, ASNGN_GIT_LINE_MAX, &next);
    if (e == ASNGN_ERR_NOT_FOUND) {
      e = packed_ref(store, ref, w->head);
      break;
    }
    free(text);
    text = next;
  }
  free(text);
  return e;
}
asngn_err asngn_git_identity(asngn_workspace_info *workspace) {
  char *primary = NULL, *common = NULL;
  workspace->head[0] = workspace->branch[0] = 0;
  asngn_err e = asngn_git_directories(workspace->repository_root, &primary, &common);
  if (e == ASNGN_ERR_NOT_FOUND) return ASNGN_OK;
  if (e == ASNGN_OK) e = identity(primary, common, workspace);
  if (e != ASNGN_OK) workspace->head[0] = workspace->branch[0] = 0;
  free(primary);
  free(common);
  return e;
}
