/* Enumerate through directory descriptors; never traverse a path alias. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Keep descriptor traversal's no-symlink protection available on Darwin. */
#define _DARWIN_C_SOURCE 1
#endif
#include "workspace_tree.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool unchanged(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
  struct timespec am = a->st_mtimespec, bm = b->st_mtimespec, ac = a->st_ctimespec,
                  bc = b->st_ctimespec;
#else
  struct timespec am = a->st_mtim, bm = b->st_mtim, ac = a->st_ctim, bc = b->st_ctim;
#endif
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
         a->st_size == b->st_size && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
         ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
static asngn_err names_read(int dir, asngn_tree_scan *scan, asngn_tree_names *names) {
  int copy = dup(dir);
  if (copy < 0) return ASNGN_ERR_IO;
  DIR *stream = fdopendir(copy);
  if (!stream) {
    close(copy);
    return ASNGN_ERR_IO;
  }
  asngn_err e = ASNGN_OK;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(stream);
    if (!entry) {
      if (errno) e = ASNGN_ERR_IO;
      break;
    }
    e = asngn_tree_name(scan, names, entry->d_name);
    if (e != ASNGN_OK) break;
  }
  closedir(stream);
  if (e == ASNGN_OK) asngn_tree_sort(names);
  return e;
}
static asngn_err walk_dir(int dir, const char *relative, unsigned depth, asngn_tree_scan *scan) {
  if (depth > scan->limits->depth) return ASNGN_ERR_LIMIT;
  if (scan->stop && scan->stop(scan->userdata)) return ASNGN_ERR_CANCELLED;
  struct stat before, after;
  if (fstat(dir, &before) != 0) return ASNGN_ERR_IO;
  asngn_tree_names names = {0};
  asngn_err e = names_read(dir, scan, &names);
  for (size_t i = 0; e == ASNGN_OK && i < names.count; i++) {
    struct stat entry;
    const char *name = names.items[i];
    if (scan->stop && scan->stop(scan->userdata)) {
      e = ASNGN_ERR_CANCELLED;
      break;
    }
    if (fstatat(dir, name, &entry, AT_SYMLINK_NOFOLLOW) != 0) {
      e = ASNGN_ERR_IO;
      break;
    }
    bool directory = S_ISDIR(entry.st_mode);
    if (directory && asngn_tree_ignored(name)) {
      scan->stats.ignored++;
      continue;
    }
    if (!directory && !S_ISREG(entry.st_mode)) {
      scan->stats.excluded++;
      continue;
    }
    char *path = *relative ? os_path_join(relative, name) : asngn_strdup(name);
    if (!path) {
      e = ASNGN_ERR_NOMEM;
      break;
    }
    if (strlen(path) >= 4096) {
      free(path);
      e = ASNGN_ERR_LIMIT;
      break;
    }
    int fd = openat(dir, name,
                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (directory ? O_DIRECTORY : 0));
    if (fd < 0) {
      free(path);
      e = ASNGN_ERR_DENIED;
      break;
    }
    struct stat opened;
    if (fstat(fd, &opened) || opened.st_dev != entry.st_dev || opened.st_ino != entry.st_ino ||
        opened.st_mode != entry.st_mode)
      e = ASNGN_ERR_BUSY;
    else if (directory) e = walk_dir(fd, path, depth + 1, scan);
    else {
      FILE *file = fdopen(fd, "rb");
      if (!file) e = ASNGN_ERR_IO;
      else {
        e = opened.st_size < 0 ? ASNGN_ERR_IO
                               : asngn_tree_file(scan, path, file, (uint64_t)opened.st_size);
        if ((e == ASNGN_OK || e == ASNGN_ERR_LIMIT) &&
            (fstat(fd, &after) || !unchanged(&opened, &after)))
          e = ASNGN_ERR_BUSY;
        fclose(file);
        fd = -1;
      }
    }
    if (fd >= 0) close(fd);
    free(path);
  }
  asngn_tree_names_free(&names);
  if ((e == ASNGN_OK || e == ASNGN_ERR_LIMIT) &&
      (fstat(dir, &after) || !unchanged(&before, &after)))
    e = ASNGN_ERR_BUSY;
  return e;
}
asngn_err asngn_tree_walk_platform(const char *root, asngn_tree_scan *scan) {
  int dir = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir < 0) return ASNGN_ERR_DENIED;
  asngn_err e = walk_dir(dir, "", 0, scan);
  close(dir);
  return e;
}
