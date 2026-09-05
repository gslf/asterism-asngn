/*
 * worker.c — bounded session scheduler and isolated runtime workers.
 *
 * The worker pool drains a bounded FIFO of submitted turns and runs each one
 * through asngn_loop_run; llama.cpp calls block on this thread with the
 * stall watchdog and the cancellation flag cascading into the backend.
 * The background worker owns cache sweeps, telemetry flushes, model warmup
 * and the optional stall-watchdog tick. Asper owns memory curation on its
 * own worker.
 *
 * ASNGN_NO_THREADS builds run turns synchronously on the caller and do
 * background work inside asngn_tick.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* ── task execution ───────────────────────────────────────────────────── */

static void task_finish(asngn_ctx *c, asngn_task *task, asngn_err verdict) {
  asngn_turn_state *t = task->turn;
  asngn_session *s = task->session;
  char terminal[128];

  /* A turn has exactly one terminal event, regardless of which phase
   * returned.  Emitting it here (rather than only on the success/cancel
   * paths in loop.c) also closes the TUI live-phase marker after model,
   * context, protocol, and token-limit failures.  Publish it before the
   * task condition variable: once task_wait observes completion, every
   * consumer has already been told that the live phase ended. */
  snprintf(terminal, sizeof terminal,
           "{ok: %s, cancelled: %s, error: \"%s\"}",
           verdict == ASNGN_OK ? "true" : "false",
           verdict == ASNGN_ERR_CANCELLED ? "true" : "false",
           verdict == ASNGN_OK ? "" : asngn_err_name(verdict));
  asngn_tele_emit(c, "turn_end", t != NULL ? t->span_root : NULL, NULL,
                  s != NULL ? s->slug : NULL,
                  t != NULL ? t->led.turn : 0, terminal);
  asngn_tele_flush(c);

  os_mutex_lock(&task->mu);
  task->verdict = verdict;
  memset(&task->result, 0, sizeof task->result);
  if (t != NULL) {
    task->result.answer = t->answer;
    t->answer = NULL;
    task->result.turn = t->led.turn;
    snprintf(task->result.klass, sizeof task->result.klass, "%s",
             t->led.klass);
    snprintf(task->result.detail, sizeof task->result.detail, "%s",
             t->led.detail);
    snprintf(task->result.tier, sizeof task->result.tier, "%s",
             t->led.tier);
    snprintf(task->result.cache, sizeof task->result.cache, "%s",
             t->led.cache);
    task->result.capped = t->capped ? 1 : 0;
    task->result.clarify = t->clarify ? 1 : 0;
    task->result.tokens_prompt = t->led.pt_system + t->led.pt_memory +
                                 t->led.pt_catalog + t->led.pt_summary +
                                 t->led.pt_verbatim + t->led.pt_working;
    task->result.tokens_gen =
        t->led.gt_decision + t->led.gt_answer + t->led.gt_aux;
    task->result.tokens_saved = t->led.sv_cache + t->led.sv_digest;
    task->result.duration_ms = t->led.duration_ms;
  }
  /* Publish session idleness before waking waiters.  Once task_wait returns,
   * the documented one-turn-per-session invariant must permit an immediate
   * follow-up submission. */
  if (s != NULL) {
    os_rwlock_wrlock(&s->lock);
    if (t && t->tx_started && !t->tx_committed) {
      while (s->log_n > t->log_before) free(s->log[--s->log_n].text);
      s->turns=t->turns_before;
    }
    s->busy = false;
    s->running = NULL;
    os_rwlock_wrunlock(&s->lock);
  }
  task->done = true;
  os_cond_broadcast(&task->cv);
  os_mutex_unlock(&task->mu);
}

#ifdef ASNGN_NO_THREADS
static void task_run(asngn_ctx *c, asngn_task *task) {
  asngn_err e = asngn_loop_run(c, task->turn);
  task_finish(c, task, e);
}

#endif

/* ── background maintenance ───────────────────────────────────────────── */

asngn_err asngn_run_due_work(asngn_ctx *c) {
  int64_t now = asngn_clock_mono_ms(&c->clock);
  bool warm;

  /* one-shot model preload queued by asngn_open */
  os_mutex_lock(&c->q_mu);
  warm = c->bg_due_warm;
  c->bg_due_warm = false;
  os_mutex_unlock(&c->q_mu);
  if (warm) asngn_models_warm(c);

  /* daily cache sweep */
  if (c->last_sweep_ms == 0 || now - c->last_sweep_ms > 86400000) {
    c->last_sweep_ms = now;
    asngn_cache_sweep(c);
    os_rwlock_wrlock(&c->lock);
    c->stats.last_sweep_at = (long long)asngn_clock_now(&c->clock);
    os_rwlock_wrunlock(&c->lock);
  }

  asngn_tele_flush(c);
  return ASNGN_OK;
}

/* Optional stall watchdog plus cancellation relay. A zero stall timeout never
 * aborts inference automatically; user cancellation is always relayed. */
#ifndef ASNGN_NO_THREADS
static void stall_tick(asngn_ctx *c) {
  int64_t now, last, started, limit, silence = 0;
  bool stalled = false;
  /* the whole check-and-cancel runs under q_mu, the same mutex
   * watched_generate arms/disarms the watch under — a call that ends
   * concurrently can never be cancelled retroactively */
  os_mutex_lock(&c->q_mu);
  if (c->call_active) {
    now = asngn_clock_mono_ms(&c->clock);
    last = c->call_last_ms;
    started = c->call_started_ms;
    if (c->cfg.stall_timeout_s > 0) {
      limit = c->cfg.stall_timeout_s * 1000;
      if (last <= started) limit *= 2; /* still evaluating the prompt */
      silence = now - (last > started ? last : started);
      if (silence > limit) {
        c->call_cancel = 1;
        stalled = true;
      }
    }
    /* a cancelled turn also aborts its in-flight call */
    if (c->call_turn != NULL && c->call_turn->cancel) c->call_cancel = 1;
  }
  os_mutex_unlock(&c->q_mu);
  if (stalled)
    asngn_log(c, ASNGN_LOG_WARN, "model",
              "stall guard: model call aborted after %lld ms of silence",
              (long long)silence);
}
#endif /* !ASNGN_NO_THREADS */

/* ── threads ──────────────────────────────────────────────────────────── */

#ifndef ASNGN_NO_THREADS

static void *agent_main(void *arg) {
  asngn_ctx *runtime = arg;
  asngn_ctx *c = runtime->owner ? runtime->owner : runtime;
  for (;;) {
    asngn_task *task;
    os_mutex_lock(&c->q_mu);
    while (!c->stop && c->q_head == NULL)
      os_cond_wait(&c->q_cv, &c->q_mu);
    if (c->stop && c->q_head == NULL) {
      os_mutex_unlock(&c->q_mu);
      break;
    }
    task = c->q_head;
    c->q_head = task->next;
    c->q_n--;
    if (c->q_head == NULL) c->q_tail = NULL;
    runtime->active_task=task;
    os_mutex_unlock(&c->q_mu);
    asngn_err e;
    if (task->turn->cancel) e=ASNGN_ERR_CANCELLED;
    else if (task->turn->deadline_mono>0 &&
        asngn_clock_mono_ms(&c->clock)>=task->turn->deadline_mono) e=ASNGN_ERR_TIMEOUT;
    else e=asngn_loop_run(runtime,task->turn);
    os_mutex_lock(&c->q_mu);runtime->active_task=NULL;os_mutex_unlock(&c->q_mu);
    if (runtime->owner) {
      if (e!=ASNGN_OK) asngn_seterr(c,e,"%s",asngn_last_error(runtime));
      os_rwlock_wrlock(&c->lock);
      c->stats.guard_trips+=runtime->stats.guard_trips;
      c->stats.tool_calls+=runtime->stats.tool_calls;
      c->stats.tool_cache_hits+=runtime->stats.tool_cache_hits;
      c->stats.cache_hits+=runtime->stats.cache_hits;
      c->stats.cache_misses+=runtime->stats.cache_misses;
      c->stats.cache_adapts+=runtime->stats.cache_adapts;
      c->stats.escalations+=runtime->stats.escalations;
      memset(&runtime->stats,0,sizeof runtime->stats);
      os_rwlock_wrunlock(&c->lock);
    }
    task_finish(c,task,e);
  }
  return NULL;
}

static void *bg_main(void *arg) {
  asngn_ctx *c = arg;
  for (;;) {
    os_mutex_lock(&c->q_mu);
    if (!c->stop) os_cond_timedwait(&c->q_cv, &c->q_mu, 1000);
    if (c->stop) {
      os_mutex_unlock(&c->q_mu);
      break;
    }
    os_mutex_unlock(&c->q_mu);
    if (c->lanes_n>1) {
      for (size_t i=0;i<c->lanes_n;i++) stall_tick(c->lanes[i]);
    } else stall_tick(c);
    asngn_run_due_work(c);
  }
  return NULL;
}

#endif /* !ASNGN_NO_THREADS */

/* Admission reserves every potentially resident model plus context/KV
 * overhead for each isolated lane and for the coordinator's memory models.
 * Unknown local resources use one lane. Remote servers own GPU admission. */
size_t asngn_scheduler_capacity(const asngn_ctx *c) {
  size_t n=(size_t)c->cfg.scheduler_workers, ram=512,vram=0;
  bool local=false;
  for (size_t i=0;i<c->models_n;i++) {
    const asngn_pool_entry *p=&c->models[i].cfg;
    if (p->backend!=ASMODEL_BACKEND_EMBEDDED) continue;
    local=true;ram+=p->ram_mb;
    if (p->gpu_layers!=0) vram+=p->vram_mb ? p->vram_mb : p->ram_mb+512;
  }
  if (n<1) n=1;
  if (n>8) n=8;
  if (local) {
    if (c->cfg.max_ram_mb<=0 || (vram && c->cfg.max_vram_mb<=0)) return 1;
    size_t slots=(size_t)c->cfg.max_ram_mb/ram;
    if (slots<2) return 1;
    if (n>slots-1) n=slots-1;
    if (vram) {
      slots=(size_t)c->cfg.max_vram_mb/vram;
      if (slots<2) return 1;
      if (n>slots-1) n=slots-1;
    }
  }
  return n;
}
asngn_err asngn_workers_start(asngn_ctx *c) {
  if (c->cfg.scheduler_workers<1 || c->cfg.scheduler_workers>8 ||
      c->cfg.queue_capacity<1 || c->cfg.queue_capacity>65536)
    return asngn_seterr(c,ASNGN_ERR_CONFIG,"engine workers must be 1..8; queue_capacity 1..65536");
#ifdef ASNGN_NO_THREADS
  c->no_threads = true;
  return ASNGN_OK;
#else
  size_t count=asngn_scheduler_capacity(c), started=0;
  asngn_err e=ASNGN_OK;
  c->stop=false;
  for (size_t i=0;i<count;i++) {
    if (count==1) c->lanes[i]=c;
    else { e=asngn_runtime_create(c,&c->lanes[i]);if (e!=ASNGN_OK) goto fail; }
    c->lanes_n++;
  }
  for (size_t i=0;i<count;i++) {
    e=os_thread_start(&c->lanes[i]->agent_thread,agent_main,c->lanes[i]);
    if (e!=ASNGN_OK) goto fail;
    started++;
  }
  e=os_thread_start(&c->bg_thread,bg_main,c);
  if (e==ASNGN_OK) return e;
fail:
  os_mutex_lock(&c->q_mu);c->stop=true;os_cond_broadcast(&c->q_cv);os_mutex_unlock(&c->q_mu);
  for (size_t i=0;i<started;i++) os_thread_join(&c->lanes[i]->agent_thread);
  for (size_t i=0;i<c->lanes_n;i++) if (c->lanes[i]!=c) asngn_runtime_free(c->lanes[i]);
  c->lanes_n=0;
  return asngn_seterr(c,e,"cannot start isolated session workers");
#endif
}

void asngn_workers_stop(asngn_ctx *c) {
  asngn_task *task;
#ifdef ASNGN_NO_THREADS
  (void)task;
  asngn_run_due_work(c);
  return;
#else
  os_mutex_lock(&c->q_mu);
  c->stop = true;
  c->bg_cancel = 1; /* abort an in-flight model warmup */
  /* cancel anything still queued */
  for (task = c->q_head; task != NULL; task = task->next)
    if (task->turn != NULL) task->turn->cancel = 1;
  for (size_t i=0;i<c->lanes_n;i++) {
    if (c->lanes[i]->active_task) c->lanes[i]->active_task->turn->cancel=1;
    c->lanes[i]->call_cancel=1;
  }
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
  for (size_t i=0;i<c->lanes_n;i++) os_thread_join(&c->lanes[i]->agent_thread);
  os_thread_join(&c->bg_thread);
  for (size_t i=0;i<c->lanes_n;i++) if (c->lanes[i]!=c) asngn_runtime_free(c->lanes[i]);
  c->lanes_n=0;
  /* finish queued tasks that never ran */
  while (c->q_head != NULL) {
    task = c->q_head;
    c->q_head = task->next;
    task_finish(c, task, ASNGN_ERR_CANCELLED);
  }
  c->q_tail = NULL;
  asngn_tele_flush(c);
#endif
}

asngn_err asngn_worker_submit(asngn_ctx *c, asngn_task *task) {
#ifdef ASNGN_NO_THREADS
  asngn_err e=asngn_turn_journal(task->turn,"started",task->turn->user_msg);
  if (e!=ASNGN_OK) return e;
  task_run(c, task);
  asngn_run_due_work(c);
  return ASNGN_OK;
#else
  os_mutex_lock(&c->q_mu);
  if (c->stop || c->q_n >= (size_t)c->cfg.queue_capacity) {
    os_mutex_unlock(&c->q_mu);
    return asngn_seterr(c, ASNGN_ERR_BUSY, "engine queue full or shutting down; retry later");
  }
  asngn_err e=asngn_turn_journal(task->turn,"started",task->turn->user_msg);
  if (e!=ASNGN_OK) { os_mutex_unlock(&c->q_mu);return e; }
  task->next = NULL;
  if (c->q_tail != NULL) c->q_tail->next = task;
  else c->q_head = task;
  c->q_tail = task;
  c->q_n++;
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
  return ASNGN_OK;
#endif
}

void asngn_background_kick(asngn_ctx *c) {
  os_mutex_lock(&c->q_mu);
  os_cond_broadcast(&c->q_cv);
  os_mutex_unlock(&c->q_mu);
}
