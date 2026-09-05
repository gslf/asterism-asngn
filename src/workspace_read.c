/* Workspace reads use opened objects, never a canonicalize-then-open check. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "asngn_internal.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

asngn_err asngn_workspace_read(const char *root, const char *relative,
                                size_t cap, char **out, size_t *len) {
  asngn_err e = ASNGN_ERR_DENIED;
  char *data = NULL;
  FILE *f = NULL;
  size_t n;
  *out = NULL;
  *len = 0;
  if (!root || !relative || !*relative || os_path_is_abs(relative) ||
      strchr(relative, '\\') || strchr(relative, ':') || cap == SIZE_MAX)
    return ASNGN_ERR_INVALID;
#ifndef _WIN32
  {
    char *copy = asngn_strdup(relative), *part, *next;
    int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (!copy) { if (dir >= 0) close(dir); return ASNGN_ERR_NOMEM; }
    part = copy;
    while (dir >= 0) {
      int fd;
      next = strchr(part, '/');
      if (next) *next++ = 0;
      if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) break;
      fd = openat(dir, part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                             (next ? O_DIRECTORY : O_NONBLOCK));
      close(dir);
      dir = -1;
      if (fd < 0) { e = errno == ENOENT ? ASNGN_ERR_NOT_FOUND : ASNGN_ERR_DENIED; break; }
      if (next) { dir = fd; part = next; continue; }
      struct stat st;
      if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) f = fdopen(fd, "rb");
      if (!f) close(fd);
      break;
    }
    if (dir >= 0) close(dir);
    free(copy);
  }
#else
  /* Verify the final opened handle against the opened root. Reparse points
   * outside the root are rejected before any file content is read. */
  {
    char *path = os_path_join(root, relative);
    wchar_t wr[32768], wf[32768];
    HANDLE rh = INVALID_HANDLE_VALUE;
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, wr, 32768);
    if (size) rh = CreateFileW(wr, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    f = path ? os_fopen(path, "rb") : NULL;
    if (rh != INVALID_HANDLE_VALUE && f) {
      DWORD rn = GetFinalPathNameByHandleW(rh, wr, 32768, FILE_NAME_NORMALIZED);
      DWORD fn = GetFinalPathNameByHandleW((HANDLE)_get_osfhandle(_fileno(f)), wf,
                                          32768, FILE_NAME_NORMALIZED);
      if (!rn || rn >= 32768 || !fn || fn >= 32768 || fn <= rn ||
          _wcsnicmp(wr, wf, rn) || (wf[rn] != L'\\' && wf[rn] != L'/')) {
        fclose(f); f = NULL;
      }
    } else if (f) { fclose(f); f = NULL; }
    if (rh != INVALID_HANDLE_VALUE) CloseHandle(rh);
    free(path);
  }
#endif
  if (!f) return e;
  data = malloc(cap + 1);
  if (!data) { fclose(f); return ASNGN_ERR_NOMEM; }
  n = fread(data, 1, cap + 1, f);
  e = ferror(f) ? ASNGN_ERR_IO : n > cap ? ASNGN_ERR_LIMIT : ASNGN_OK;
  fclose(f);
  if (e != ASNGN_OK) { free(data); return e; }
  data[n] = 0;
  *out = data;
  *len = n;
  return ASNGN_OK;
}
