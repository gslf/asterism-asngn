/* Closed, bounded Git metadata reads. No repository configuration is executed. */
#ifndef ASNGN_WORKSPACE_GIT_H
#define ASNGN_WORKSPACE_GIT_H
#include "asngn_internal.h"

#define ASNGN_GIT_LINE_MAX 1024
#define ASNGN_GIT_PACKED_MAX (4 * 1024 * 1024)
asngn_err asngn_git_read(const char *directory, const char *name, size_t cap,
                         char **out);
asngn_err asngn_git_line(char *text);
/* Returns allocated primary/common metadata directories, or NOT_FOUND for no
 * marker. External metadata is accepted only for a registered linked worktree. */
asngn_err asngn_git_directories(const char *repository, char **primary, char **common);
asngn_err asngn_git_identity(asngn_workspace_info *workspace);
#endif
