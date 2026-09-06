/* Bounded POSIX stdio transport. The executable owns SIGPIPE handling. */
#ifndef ASNGN_ACP_IO_H
#define ASNGN_ACP_IO_H
#include "asngn_internal.h"
#define ACP_FRAME_BYTES (1024u * 1024u)
#define ACP_QUEUE_BYTES (4u * 1024u * 1024u)
typedef struct {
  int input, output, input_flags, output_flags;
  bool eof;
  asngn_buf incoming, outgoing;
  size_t written;
  int64_t progress;
} acp_io;
asngn_err acp_io_open(acp_io *io, int input, int output);
void acp_io_close(acp_io *io);
asngn_err acp_io_pump(acp_io *io, int wait_ms);
asngn_err acp_io_next(acp_io *io, char **text, size_t *length);
asngn_err acp_io_queue(acp_io *io, const char *text, size_t length);
#endif
