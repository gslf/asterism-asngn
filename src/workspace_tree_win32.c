/* Enumerate opened directories; held parents deny rename until traversal ends. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "workspace_tree.h"
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  FILE_BASIC_INFO basic;
  FILE_STANDARD_INFO standard;
} file_state;
static bool state_read(HANDLE h, file_state *out) {
  return GetFileInformationByHandleEx(h, FileBasicInfo, &out->basic, sizeof out->basic) &&
         GetFileInformationByHandleEx(h, FileStandardInfo, &out->standard, sizeof out->standard);
}
static bool unchanged(const file_state *a, const file_state *b) {
  return a->basic.LastWriteTime.QuadPart == b->basic.LastWriteTime.QuadPart &&
         a->basic.ChangeTime.QuadPart == b->basic.ChangeTime.QuadPart &&
         a->basic.FileAttributes == b->basic.FileAttributes &&
         a->standard.EndOfFile.QuadPart == b->standard.EndOfFile.QuadPart &&
         a->standard.NumberOfLinks == b->standard.NumberOfLinks && !b->standard.DeletePending;
}
static HANDLE open_path(const char *path) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (!n) return INVALID_HANDLE_VALUE;
  wchar_t *wide = malloc((size_t)n * sizeof *wide);
  if (!wide) return INVALID_HANDLE_VALUE;
  HANDLE h = INVALID_HANDLE_VALUE;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n))
    h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  return h;
}
static asngn_err names_read(HANDLE dir, asngn_tree_scan *scan, asngn_tree_names *names) {
  const DWORD capacity = 64 * 1024;
  unsigned char *buffer = malloc(capacity);
  if (!buffer) return ASNGN_ERR_NOMEM;
  asngn_err e = ASNGN_OK;
  FILE_INFO_BY_HANDLE_CLASS query = FileIdBothDirectoryRestartInfo;
  for (;;) {
    if (!GetFileInformationByHandleEx(dir, query, buffer, capacity)) {
      if (GetLastError() != ERROR_NO_MORE_FILES) e = ASNGN_ERR_IO;
      break;
    }
    query = FileIdBothDirectoryInfo;
    size_t offset = 0;
    for (;;) {
      size_t prefix = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (offset > capacity - prefix) {
        e = ASNGN_ERR_IO;
        break;
      }
      const FILE_ID_BOTH_DIR_INFO *entry = (const void *)(buffer + offset);
      size_t bytes = entry->FileNameLength;
      if (!bytes || bytes % sizeof(wchar_t) || bytes > capacity - offset - prefix) {
        e = ASNGN_ERR_IO;
        break;
      }
      int chars = (int)(bytes / sizeof(wchar_t));
      int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName, chars, NULL, 0,
                                     NULL, NULL);
      if (!size) {
        e = ASNGN_ERR_INVALID;
        break;
      }
      char *name = malloc((size_t)size + 1);
      if (!name) {
        e = ASNGN_ERR_NOMEM;
        break;
      }
      if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->FileName, chars, name, size,
                               NULL, NULL))
        e = ASNGN_ERR_INVALID;
      else {
        name[size] = 0;
        size_t previous = names->count;
        e = memchr(name, 0, (size_t)size) ? ASNGN_ERR_INVALID : asngn_tree_name(scan, names, name);
        /* Count every entry, but do not open ignored directories or aliases. */
        if (e == ASNGN_OK && names->count > previous &&
            (entry->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          scan->stats.excluded++;
          free(names->items[--names->count]);
        } else if (e == ASNGN_OK && names->count > previous &&
                   (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                   asngn_tree_ignored(name)) {
          scan->stats.ignored++;
          free(names->items[--names->count]);
        }
      }
      free(name);
      if (e != ASNGN_OK || !entry->NextEntryOffset) break;
      if (entry->NextEntryOffset % 8 || entry->NextEntryOffset < prefix + bytes ||
          entry->NextEntryOffset > capacity - offset) {
        e = ASNGN_ERR_IO;
        break;
      }
      offset += entry->NextEntryOffset;
    }
    if (e != ASNGN_OK) break;
  }
  free(buffer);
  if (e == ASNGN_OK) asngn_tree_sort(names);
  return e;
}
static asngn_err walk_dir(HANDLE dir, const char *root, const char *relative, unsigned depth,
                          asngn_tree_scan *scan) {
  if (depth > scan->limits->depth) return ASNGN_ERR_LIMIT;
  if (scan->stop && scan->stop(scan->userdata)) return ASNGN_ERR_CANCELLED;
  file_state before, after;
  if (!state_read(dir, &before)) return ASNGN_ERR_IO;
  asngn_tree_names names = {0};
  asngn_err e = names_read(dir, scan, &names);
  for (size_t i = 0; e == ASNGN_OK && i < names.count; i++) {
    if (scan->stop && scan->stop(scan->userdata)) {
      e = ASNGN_ERR_CANCELLED;
      break;
    }
    char *relative_path =
        *relative ? os_path_join(relative, names.items[i]) : asngn_strdup(names.items[i]);
    char *path = relative_path ? os_path_join(root, relative_path) : NULL;
    if (!path) {
      free(relative_path);
      e = ASNGN_ERR_NOMEM;
      break;
    }
    if (strlen(relative_path) >= 4096) {
      free(relative_path);
      free(path);
      e = ASNGN_ERR_LIMIT;
      break;
    }
    HANDLE h = open_path(path);
    free(path);
    file_state opened;
    if (h == INVALID_HANDLE_VALUE) e = ASNGN_ERR_DENIED;
    else if (!state_read(h, &opened)) e = ASNGN_ERR_IO;
    else if (opened.basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) scan->stats.excluded++;
    else if (opened.standard.Directory) {
      if (asngn_tree_ignored(names.items[i])) scan->stats.ignored++;
      else e = walk_dir(h, root, relative_path, depth + 1, scan);
    } else {
      int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
      if (fd < 0) e = ASNGN_ERR_IO;
      else {
        FILE *file = _fdopen(fd, "rb");
        if (!file) {
          _close(fd);
          e = ASNGN_ERR_IO;
        } else {
          e = opened.standard.EndOfFile.QuadPart < 0
                  ? ASNGN_ERR_IO
                  : asngn_tree_file(scan, relative_path, file,
                                    (uint64_t)opened.standard.EndOfFile.QuadPart);
          if ((e == ASNGN_OK || e == ASNGN_ERR_LIMIT) &&
              (!state_read(h, &after) || !unchanged(&opened, &after)))
            e = ASNGN_ERR_BUSY;
          fclose(file);
        }
        h = INVALID_HANDLE_VALUE;
      }
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(relative_path);
  }
  asngn_tree_names_free(&names);
  if ((e == ASNGN_OK || e == ASNGN_ERR_LIMIT) &&
      (!state_read(dir, &after) || !unchanged(&before, &after)))
    e = ASNGN_ERR_BUSY;
  return e;
}
asngn_err asngn_tree_walk_platform(const char *root, asngn_tree_scan *scan) {
  HANDLE dir = open_path(root);
  if (dir == INVALID_HANDLE_VALUE) return ASNGN_ERR_DENIED;
  file_state state;
  asngn_err e = ASNGN_ERR_DENIED;
  if (state_read(dir, &state) && state.standard.Directory &&
      !(state.basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    e = walk_dir(dir, root, "", 0, scan);
  CloseHandle(dir);
  return e;
}
