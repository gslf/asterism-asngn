/* Confirmation gate for actions that can mutate user state. */
#include "execution.h"
#include <stdlib.h>
#include <string.h>

/* Confirmation gate. Returns 1 allow, 0 deny; fills deny_code. */
int asngn_call_confirm(asngn_ctx *c, asngn_turn_state *t, const char *ref, const char *cmd,
                       const char *args, const asngn_tool_note *note, const char **deny_code) {
  asngn_session *s = t->s;
  char label[132];
  size_t i;

  *deny_code = "astools/denied";
  if (note->read_only && !note->destructive) return 1;

  snprintf(label, sizeof label, "%s.%s", ref, cmd);
  switch (c->cfg.autoconfirm) {
  case ASNGN_CONFIRM_ALLOW:
    return 1;
  case ASNGN_CONFIRM_DENY:
    *deny_code = "asngn/confirm-required";
    return 0;
  case ASNGN_CONFIRM_PROMPT:
    break;
  }
  if (t->security_profile == ASNGN_SECURITY_AUTOMATION_CI && !note->destructive) return 1;
  for (i = 0; i < s->allow_n; i++)
    if (strcmp(s->allow[i], label) == 0) return 1;

  /* raise the confirm event and block on asngn_confirm */
  {
    char id[37];
    asngn_buf data;
    int allow = 0, session_wide = 0, decided = 0;
    asngn_uuid_v4(id);
    os_mutex_lock(&c->confirm.mu);
    snprintf(c->confirm.id, sizeof c->confirm.id, "%s", id);
    snprintf(c->confirm.ref, sizeof c->confirm.ref, "%s", ref);
    snprintf(c->confirm.cmd, sizeof c->confirm.cmd, "%s", cmd);
    c->confirm.decided = 0;
    c->confirm.allow = 0;
    c->confirm.session_wide = 0;
    os_mutex_unlock(&c->confirm.mu);

    asngn_buf_init(&data);
    {
      /* telemetry is always redacted; the snippet lands inside a
       * double-quoted xcdn string, so quotes and backslashes need
       * real escaping (not a char swap: that turns the `\"` pairs
       * models emit into `\'`, an invalid escape that makes the
       * whole event line unparseable) and control bytes flatten to
       * spaces. The +2 bound keeps truncation from splitting an
       * escape pair. */
      char safe[161];
      char *masked = NULL;
      size_t nm = 0, si = 0;
      const char *asrc = args != NULL ? args : "{}";
      if (asngn_redact(asrc, strlen(asrc), &masked, &nm) == ASNGN_OK && masked != NULL)
        asrc = masked;
      for (; *asrc != '\0' && si + 2 < sizeof safe; asrc++) {
        unsigned char ch = (unsigned char)*asrc;
        if (ch == '"' || ch == '\\') {
          safe[si++] = '\\';
          safe[si++] = (char)ch;
        } else {
          safe[si++] = ch < 0x20 ? ' ' : (char)ch;
        }
      }
      safe[si] = '\0';
      if (asngn_buf_printf(&data,
                           "{confirm_id: u\"%s\", tool: \"%s\", command: "
                           "\"%s\", destructive: %s, read_only: %s, args: "
                           "\"%s\"}",
                           id, ref, cmd, note->destructive ? "true" : "false",
                           note->read_only ? "true" : "false", safe) == ASNGN_OK)
        asngn_tele_emit(c, "confirm", t->span_root, NULL, s->slug, t->led.turn, data.data);
      free(masked);
    }
    asngn_buf_free(&data);

    os_mutex_lock(&c->confirm.mu);
    while (!c->confirm.decided && !t->cancel && !asngn_turn_expired(c, t))
      os_cond_timedwait(&c->confirm.cv, &c->confirm.mu, 100);
    decided = c->confirm.decided;
    allow = c->confirm.allow;
    session_wide = c->confirm.session_wide;
    c->confirm.id[0] = '\0';
    c->confirm.decided = 0;
    os_mutex_unlock(&c->confirm.mu);

    if (!decided) {
      *deny_code = t->cancel ? "astools/cancelled" : "asngn/confirm-timeout";
      return 0;
    }
    if (allow && session_wide) {
      char **na = realloc(s->allow, (s->allow_n + 1) * sizeof *na);
      if (na != NULL) {
        s->allow = na;
        s->allow[s->allow_n] = asngn_strdup(label);
        if (s->allow[s->allow_n] != NULL) s->allow_n++;
      }
    }
    if (!allow) *deny_code = "astools/denied";
    return allow;
  }
}
