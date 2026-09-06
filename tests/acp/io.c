/* Exercise real pipes: frame boundaries, output quotas, EOF and stalled peers. */
#include "io.h"
#include "asngn_test.h"
#include <fcntl.h>
#include <signal.h>

typedef struct {
  int input[2], output[2];
  acp_io io;
} pipes;
static int setup(pipes *p) {
  return pipe(p->input) == 0 && pipe(p->output) == 0 &&
         acp_io_open(&p->io, p->input[0], p->output[1]) == ASNGN_OK;
}
static void drop(pipes *p) {
  acp_io_close(&p->io);
  close(p->input[0]);
  close(p->input[1]);
  close(p->output[0]);
  close(p->output[1]);
}
TEST(exact_frame_limit_and_following_frame) {
  pipes p;
  ASSERT_TRUE(setup(&p));
  char bytes[4096];
  memset(bytes, 'x', sizeof bytes);
  for (size_t sent = 0; sent < ACP_FRAME_BYTES; sent += sizeof bytes) {
    ASSERT_EQ_INT(write(p.input[1], bytes, sizeof bytes), sizeof bytes);
    ASSERT_OK(acp_io_pump(&p.io, 0));
  }
  ASSERT_EQ_INT(write(p.input[1], "\n{}\n", 4), 4);
  ASSERT_OK(acp_io_pump(&p.io, 0));
  char *frame = NULL;
  size_t length;
  ASSERT_OK(acp_io_next(&p.io, &frame, &length));
  ASSERT_EQ_INT(length, ACP_FRAME_BYTES);
  free(frame);
  ASSERT_OK(acp_io_pump(&p.io, 0));
  ASSERT_OK(acp_io_next(&p.io, &frame, &length));
  ASSERT_EQ_STR(frame, "{}");
  free(frame);
  drop(&p);
}
TEST(oversized_unterminated_frame_and_eof) {
  pipes p;
  ASSERT_TRUE(setup(&p));
  ASSERT_EQ_INT(write(p.input[1], "{", 1), 1);
  ASSERT_OK(acp_io_pump(&p.io, 0));
  char *frame = NULL;
  size_t length;
  ASSERT_ERR(acp_io_next(&p.io, &frame, &length), ASNGN_ERR_BUSY);
  close(p.input[1]);
  p.input[1] = -1;
  ASSERT_OK(acp_io_pump(&p.io, 0));
  ASSERT_TRUE(p.io.eof);
  ASSERT_ERR(acp_io_next(&p.io, &frame, &length), ASNGN_ERR_PARSE);
  drop(&p);
}
TEST(output_quota_is_checked_before_appending) {
  pipes p;
  ASSERT_TRUE(setup(&p));
  char *text = malloc(ACP_FRAME_BYTES + 1);
  ASSERT_TRUE(text != NULL);
  memset(text, 'x', ACP_FRAME_BYTES);
  text[ACP_FRAME_BYTES] = 0;
  for (int i = 0; i < 3; i++)
    ASSERT_OK(acp_io_queue(&p.io, text, ACP_FRAME_BYTES));
  size_t before = p.io.outgoing.len;
  ASSERT_ERR(acp_io_queue(&p.io, text, ACP_FRAME_BYTES), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(p.io.outgoing.len, before);
  ASSERT_ERR(acp_io_queue(&p.io, "x\ny", 3), ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(p.io.outgoing.len, before);
  free(text);
  drop(&p);
}
TEST(output_stall_and_broken_pipe_do_not_block) {
  pipes p;
  ASSERT_TRUE(setup(&p));
  char bytes[4096];
  memset(bytes, 'x', sizeof bytes);
  for (int i = 0; i < 64; i++)
    ASSERT_OK(acp_io_queue(&p.io, bytes, sizeof bytes));
  for (int i = 0; i < 64; i++)
    ASSERT_OK(acp_io_pump(&p.io, 0));
  ASSERT_TRUE(p.io.written && p.io.written < p.io.outgoing.len);
  p.io.progress = os_monotonic_ms() - 10001;
  ASSERT_ERR(acp_io_pump(&p.io, 0), ASNGN_ERR_TIMEOUT);
  p.io.progress = os_monotonic_ms();
  close(p.output[0]);
  p.output[0] = -1;
  ASSERT_ERR(acp_io_pump(&p.io, 0), ASNGN_ERR_IO);
  drop(&p);
}
TEST(borrowed_descriptors_restore_original_flags) {
  pipes p;
  ASSERT_TRUE(setup(&p));
  int input_flags = p.io.input_flags, output_flags = p.io.output_flags;
  ASSERT_TRUE(fcntl(p.input[0], F_GETFL) & O_NONBLOCK);
  ASSERT_TRUE(fcntl(p.output[1], F_GETFL) & O_NONBLOCK);
  acp_io_close(&p.io);
  ASSERT_EQ_INT(fcntl(p.input[0], F_GETFL), input_flags);
  ASSERT_EQ_INT(fcntl(p.output[1], F_GETFL), output_flags);
  close(p.input[0]);
  close(p.input[1]);
  close(p.output[0]);
  close(p.output[1]);
}
TEST_LIST = {TEST_ENTRY(exact_frame_limit_and_following_frame),
             TEST_ENTRY(oversized_unterminated_frame_and_eof),
             TEST_ENTRY(output_quota_is_checked_before_appending),
             TEST_ENTRY(output_stall_and_broken_pipe_do_not_block),
             TEST_ENTRY(borrowed_descriptors_restore_original_flags)};
RUN_ALL_TESTS()
