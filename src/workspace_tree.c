/* One ignore/limit policy for snapshots and repository evidence. */
#include "workspace_tree.h"
#include <stdlib.h>
#include <string.h>

const asngn_tree_limits asngn_tree_default_limits = {65536, 8 * 1024 * 1024, 256 * 1024 * 1024, 64};
static const char *const ignored[] = {".git",  ".hg", ".svn", ".asterism", "node_modules",
                                      "build", "out", "dist", "target",    "__pycache__"};
bool asngn_tree_ignored(const char *name) {
  for (size_t i = 0; i < sizeof ignored / sizeof ignored[0]; i++)
    if (!strcmp(name, ignored[i])) return true;
  return false;
}
bool asngn_tree_ignored_path(const char *relative) {
  for (const char *p = relative, *end; (end = strchr(p, '/')) != NULL; p = end + 1)
    for (size_t i = 0; i < sizeof ignored / sizeof ignored[0]; i++)
      if (strlen(ignored[i]) == (size_t)(end - p) && !memcmp(p, ignored[i], (size_t)(end - p)))
        return true;
  return false;
}
static int by_name(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
void asngn_tree_sort(asngn_tree_names *names) {
  if (names->count > 1) qsort(names->items, names->count, sizeof *names->items, by_name);
}
void asngn_tree_names_free(asngn_tree_names *names) {
  for (size_t i = 0; i < names->count; i++)
    free(names->items[i]);
  free(names->items);
  memset(names, 0, sizeof *names);
}
asngn_err asngn_tree_name(asngn_tree_scan *scan, asngn_tree_names *names, const char *name) {
  if (!strcmp(name, ".") || !strcmp(name, "..")) return ASNGN_OK;
  if (scan->stop && scan->stop(scan->userdata)) return ASNGN_ERR_CANCELLED;
  size_t bytes = strlen(name) + 1;
  if (!*name || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') ||
      !asngn_utf8_valid(name, bytes - 1))
    return ASNGN_ERR_INVALID;
  if (scan->stats.entries >= scan->limits->entries || bytes > 16 * 1024 * 1024 - scan->name_bytes)
    return ASNGN_ERR_LIMIT;
  if (names->count == names->capacity) {
    size_t capacity = names->capacity ? names->capacity * 2 : 16;
    char **items = realloc(names->items, capacity * sizeof *items);
    if (!items) return ASNGN_ERR_NOMEM;
    names->items = items;
    names->capacity = capacity;
  }
  char *copy = asngn_strdup(name);
  if (!copy) return ASNGN_ERR_NOMEM;
  names->items[names->count++] = copy;
  scan->stats.entries++;
  scan->name_bytes += bytes;
  return ASNGN_OK;
}
asngn_err asngn_tree_file(asngn_tree_scan *scan, const char *relative, FILE *file, uint64_t bytes) {
  if (scan->stop && scan->stop(scan->userdata)) return ASNGN_ERR_CANCELLED;
  if (bytes > scan->limits->file_bytes || bytes > scan->limits->total_bytes - scan->stats.bytes)
    return ASNGN_ERR_LIMIT;
  scan->stats.files++;
  scan->stats.bytes += (size_t)bytes;
  return scan->visit(relative, file, (size_t)bytes, scan->userdata);
}
asngn_err asngn_tree_read(FILE *file, size_t bytes, size_t cap, char **text, size_t *len) {
  *text = NULL;
  *len = 0;
  if (bytes > cap || bytes == SIZE_MAX) return ASNGN_ERR_LIMIT;
  char *data = malloc(bytes + 1);
  if (!data) return ASNGN_ERR_NOMEM;
  size_t n = fread(data, 1, bytes, file);
  if (n != bytes || ferror(file)) {
    free(data);
    return ASNGN_ERR_IO;
  }
  data[n] = 0;
  *text = data;
  *len = n;
  return ASNGN_OK;
}
asngn_err asngn_tree_walk(const char *root, const asngn_tree_limits *limits, asngn_tree_visit visit,
                          asngn_tree_stop stop, void *userdata, asngn_tree_stats *stats) {
  if (stats) memset(stats, 0, sizeof *stats);
  if (!limits) limits = &asngn_tree_default_limits;
  if (!root || !*root || !visit || !limits->entries || limits->entries > 65536 ||
      limits->depth > 64)
    return ASNGN_ERR_INVALID;
  asngn_tree_scan scan = {.limits = limits, .visit = visit, .stop = stop, .userdata = userdata};
  asngn_err e = asngn_tree_walk_platform(root, &scan);
  if (stats) *stats = scan.stats;
  return e;
}
