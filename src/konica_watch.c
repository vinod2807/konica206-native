/*
 * konica_watch.c
 *
 * See konica_watch.h for the policy. Notes for reviewers (PAPPL 1.4):
 *
 * - papplJobCancel() is async-safe to call from this monitor thread: the
 *   job thread observes it via papplJobIsCanceled() in the render callbacks
 *   and the USB send loop, then unwinds and releases the device normally.
 * - The table holds at most KONICA_WATCH_MAX jobs; overflow jobs are left
 *   untracked (never force-cancelled) rather than evicting a live entry.
 * - Timestamps use CLOCK_MONOTONIC so wall-clock jumps cannot trigger a
 *   false cancel.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "konica_watch.h"

#define KONICA_WATCH_MAX     16
#define KONICA_WATCH_DEF_SECS 600L
#define KONICA_WATCH_POLL_SECS 30L

typedef struct
{
  pappl_job_t *job;
  int          job_id;
  struct timespec last;      /* last progress (monotonic) */
  bool         warned;       /* silence already acted on once */
  bool         used;
} watch_slot_t;

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static watch_slot_t    watch_slots[KONICA_WATCH_MAX];
static long            watch_secs = KONICA_WATCH_DEF_SECS;
static bool            watch_started = false;

static void
watch_now(struct timespec *ts)
{
  clock_gettime(CLOCK_MONOTONIC, ts);
}

static long
watch_elapsed(const struct timespec *since, const struct timespec *now)
{
  return (long)(now->tv_sec - since->tv_sec);
}

static watch_slot_t *
watch_find_locked(pappl_job_t *job)
{
  for (int i = 0; i < KONICA_WATCH_MAX; i++)
    if (watch_slots[i].used && watch_slots[i].job == job)
      return &watch_slots[i];
  return NULL;
}

static void *
watch_thread(void *ud)
{
  (void)ud;

  for (;;)
  {
    struct timespec now;
    long secs;

    /* Sleep in 1s steps so a reconfigure takes effect promptly. */
    for (long s = 0; s < KONICA_WATCH_POLL_SECS; s++)
    {
      struct timespec rq = { 1, 0 };
      nanosleep(&rq, NULL);
    }

    pthread_mutex_lock(&watch_lock);
    secs = watch_secs;
    if (secs <= 0)
    {
      pthread_mutex_unlock(&watch_lock);
      continue;
    }
    watch_now(&now);
    for (int i = 0; i < KONICA_WATCH_MAX; i++)
    {
      watch_slot_t *s = &watch_slots[i];
      pappl_job_t *job;
      int jid;

      if (!s->used)
        continue;
      if (watch_elapsed(&s->last, &now) < secs)
        continue;
      job = s->job;
      jid = s->job_id;
      if (s->warned)
      {
        /* Second consecutive silent period: the job thread has not
         * unwound after cancel. Log loudly but do not touch it again;
         * it (or its filter child) is wedged below PAPPL. */
        pthread_mutex_unlock(&watch_lock);
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR,
                    "Stuck watchdog: job %d still silent %ld s after cancel, needs operator attention",
                    jid, secs * 2);
        pthread_mutex_lock(&watch_lock);
        /* Stop tracking so one wedged job cannot fill the table. */
        s->used = false;
        continue;
      }
      s->warned = true;
      pthread_mutex_unlock(&watch_lock);
      papplLogJob(job, PAPPL_LOGLEVEL_WARN,
                  "Stuck watchdog: no progress for %ld s, canceling job %d",
                  secs, jid);
      papplJobCancel(job);
      pthread_mutex_lock(&watch_lock);
    }
    pthread_mutex_unlock(&watch_lock);
  }
  return NULL;
}

void
konica_watch_init(void)
{
  const char *env = getenv("KONICA_STUCK_SECS");
  pthread_t tid;
  pthread_attr_t attr;

  pthread_mutex_lock(&watch_lock);
  if (env && *env)
  {
    long v = atol(env);
    if (v >= 0)
      watch_secs = v;
  }
  if (watch_started)
  {
    pthread_mutex_unlock(&watch_lock);
    return;
  }
  watch_started = true;
  pthread_mutex_unlock(&watch_lock);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&tid, &attr, watch_thread, NULL);
  pthread_attr_destroy(&attr);
}

void
konica_watch_set_secs(long secs)
{
  if (secs < 0)
    return;
  pthread_mutex_lock(&watch_lock);
  watch_secs = secs;
  pthread_mutex_unlock(&watch_lock);
}

void
konica_watch_job_start(pappl_job_t *job)
{
  struct timespec now;

  if (!job)
    return;
  watch_now(&now);
  pthread_mutex_lock(&watch_lock);
  if (watch_find_locked(job) == NULL)
  {
    for (int i = 0; i < KONICA_WATCH_MAX; i++)
    {
      if (!watch_slots[i].used)
      {
        watch_slots[i].used = true;
        watch_slots[i].job = job;
        watch_slots[i].job_id = papplJobGetID(job);
        watch_slots[i].last = now;
        watch_slots[i].warned = false;
        break;
      }
    }
    /* Table full: leave untracked (never force-cancel unknown jobs). */
  }
  pthread_mutex_unlock(&watch_lock);
}

void
konica_watch_ping(pappl_job_t *job)
{
  struct timespec now;

  if (!job)
    return;
  watch_now(&now);
  pthread_mutex_lock(&watch_lock);
  {
    watch_slot_t *s = watch_find_locked(job);
    if (s)
    {
      s->last = now;
      s->warned = false;   /* progress resets the two-stage policy */
    }
  }
  pthread_mutex_unlock(&watch_lock);
}

void
konica_watch_job_done(pappl_job_t *job)
{
  if (!job)
    return;
  pthread_mutex_lock(&watch_lock);
  {
    watch_slot_t *s = watch_find_locked(job);
    if (s)
      s->used = false;
  }
  pthread_mutex_unlock(&watch_lock);
}
