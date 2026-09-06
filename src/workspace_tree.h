/* Shared workspace enumeration. Callbacks borrow an already authorized file. */
#ifndef ASNGN_WORKSPACE_TREE_H
#define ASNGN_WORKSPACE_TREE_H
#include "asngn_internal.h"
typedef struct {
  size_t entries, file_bytes, total_bytes;
  unsigned depth;
} asngn_tree_limits;
typedef struct {
  size_t entries, files, bytes, excluded, ignored;
} asngn_tree_stats;
typedef asngn_err (*asngn_tree_visit)(const char *relative, FILE *file, size_t bytes,
                                      void *userdata);
typedef bool (*asngn_tree_stop)(void *userdata);
extern const asngn_tree_limits asngn_tree_default_limits;
asngn_err asngn_tree_walk(const char *root, const asngn_tree_limits *limits, asngn_tree_visit visit,
                          asngn_tree_stop stop, void *userdata, asngn_tree_stats *stats);
asngn_err asngn_tree_read(FILE *file, size_t bytes, size_t cap, char **text, size_t *len);
asngn_err asngn_workspace_tree_hash(const char *root, asngn_sha256_ctx *hash,
                                    asngn_repo_stats *stats);

/* Platform implementation contract. Directory names share a global quota. */
typedef struct {
  const asngn_tree_limits *limits;
  asngn_tree_visit visit;
  asngn_tree_stop stop;
  void *userdata;
  asngn_tree_stats stats;
  size_t name_bytes;
} asngn_tree_scan;
typedef struct {
  char **items;
  size_t count, capacity;
} asngn_tree_names;
bool asngn_tree_ignored(const char *name);
bool asngn_tree_ignored_path(const char *relative);
asngn_err asngn_tree_name(asngn_tree_scan *scan, asngn_tree_names *names, const char *name);
void asngn_tree_sort(asngn_tree_names *names);
void asngn_tree_names_free(asngn_tree_names *names);
asngn_err asngn_tree_file(asngn_tree_scan *scan, const char *relative, FILE *file, uint64_t bytes);
asngn_err asngn_tree_walk_platform(const char *root, asngn_tree_scan *scan);
#endif
