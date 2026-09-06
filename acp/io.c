/* Polling remains responsive to cancellation and bounded output backpressure. */
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

asngn_err acp_io_open(acp_io *io, int input, int output) {
  memset(io, 0, sizeof *io);
  io->input = input;
  io->output = output;
  io->input_flags = fcntl(input, F_GETFL);
  io->output_flags = fcntl(output, F_GETFL);
  if (io->input_flags < 0 || io->output_flags < 0 ||
      fcntl(input, F_SETFL, io->input_flags | O_NONBLOCK) < 0 ||
      fcntl(output, F_SETFL, io->output_flags | O_NONBLOCK) < 0) {
    acp_io_close(io);
    return ASNGN_ERR_IO;
  }
  return ASNGN_OK;
}
void acp_io_close(acp_io *io) {
  if (io->input_flags >= 0)
    (void)fcntl(io->input, F_SETFL, io->input_flags);
  if (io->output_flags >= 0)
    (void)fcntl(io->output, F_SETFL, io->output_flags);
  asngn_buf_free(&io->incoming);
  asngn_buf_free(&io->outgoing);
}
asngn_err acp_io_queue(acp_io *io, const char *text, size_t length) {
  if (length > ACP_FRAME_BYTES || memchr(text, '\n', length))
    return ASNGN_ERR_LIMIT;
  size_t pending = io->outgoing.len - io->written;
  if (pending >= ACP_QUEUE_BYTES || length + 1 > ACP_QUEUE_BYTES - pending)
    return ASNGN_ERR_LIMIT;
  if (io->written) {
    memmove(io->outgoing.data, io->outgoing.data + io->written, pending);
    io->outgoing.data[pending] = 0;
    io->outgoing.len = pending;
    io->written = 0;
  }
  if (!pending)
    io->progress = os_monotonic_ms();
  asngn_err e = asngn_buf_append(&io->outgoing, text, length);
  if (e == ASNGN_OK)
    e = asngn_buf_appendc(&io->outgoing, '\n');
  return e;
}
asngn_err acp_io_next(acp_io *io, char **text, size_t *length) {
  *text = NULL;
  *length = 0;
  char *end = io->incoming.len ? memchr(io->incoming.data, '\n', io->incoming.len) : NULL;
  if (!end)
    return io->eof && io->incoming.len ? ASNGN_ERR_PARSE : ASNGN_ERR_BUSY;
  size_t n = (size_t)(end - io->incoming.data);
  if (n > ACP_FRAME_BYTES)
    return ASNGN_ERR_LIMIT;
  *text = asngn_strndup(io->incoming.data, n);
  if (!*text)
    return ASNGN_ERR_NOMEM;
  *length = n;
  io->incoming.len -= n + 1;
  memmove(io->incoming.data, end + 1, io->incoming.len);
  io->incoming.data[io->incoming.len] = 0;
  return ASNGN_OK;
}
asngn_err acp_io_pump(acp_io *io, int wait_ms) {
  bool line = io->incoming.len && memchr(io->incoming.data, '\n', io->incoming.len);
  if (!line && io->incoming.len > ACP_FRAME_BYTES)
    return ASNGN_ERR_LIMIT;
  bool pending = io->outgoing.len > io->written;
  if (pending && os_monotonic_ms() - io->progress > 10000)
    return ASNGN_ERR_TIMEOUT;
  struct pollfd fds[2] = {{io->eof || line ? -1 : io->input, POLLIN, 0},
                          {pending ? io->output : -1, POLLOUT, 0}};
  int ready = poll(fds, 2, line ? 0 : wait_ms);
  if (ready < 0)
    return errno == EINTR ? ASNGN_OK : ASNGN_ERR_IO;
  if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLNVAL))
    return ASNGN_ERR_IO;
  if (fds[1].revents & POLLHUP)
    return ASNGN_ERR_IO;
  if (fds[1].revents & POLLOUT) {
    size_t n = io->outgoing.len - io->written;
    if (n > 65536)
      n = 65536;
    ssize_t sent = write(io->output, io->outgoing.data + io->written, n);
    if (sent > 0) {
      io->written += (size_t)sent;
      io->progress = os_monotonic_ms();
      if (io->written == io->outgoing.len) {
        io->written = io->outgoing.len = 0;
        io->outgoing.data[0] = 0;
      }
    } else if (!sent || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
      return ASNGN_ERR_IO;
  }
  if (fds[0].revents & (POLLIN | POLLHUP)) {
    char bytes[8192];
    size_t room = ACP_FRAME_BYTES + 1 - io->incoming.len;
    if (room > sizeof bytes)
      room = sizeof bytes;
    ssize_t got = read(io->input, bytes, room);
    if (got > 0)
      return asngn_buf_append(&io->incoming, bytes, (size_t)got);
    if (!got)
      io->eof = true;
    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
      return ASNGN_ERR_IO;
  }
  return ASNGN_OK;
}
