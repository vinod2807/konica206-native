/*
 * konica_watch.h
 *
 * In-app stuck-job watchdog for konica206-native.
 *
 * Replaces the external konica-stuck-watch.sh helper: instead of parsing
 * log files and restarting the server process, the watchdog tracks
 * per-job activity timestamps in-process and cancels jobs that go silent.
 *
 * Policy (mirrors the shell script):
 *   - a job counts as stuck only after SILENCE seconds with zero progress;
 *   - long healthy jobs that keep making progress are never touched;
 *   - first sighting cancels the job; the job thread then unwinds through
 *     the normal papplJobIsCanceled() checks and releases the USB device;
 *   - no server restart is performed; PAPPL returns the queue to idle.
 *
 * Silence threshold: KONICA_STUCK_SECS env (default 600, 0 disables),
 * overridable at runtime with the server option "stuck-watch-secs".
 */

#ifndef KONICA_WATCH_H
#define KONICA_WATCH_H

#include <pappl/pappl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the watchdog thread once. Safe to call repeatedly. */
void konica_watch_init(void);

/* Override the silence threshold in seconds (0 disables). */
void konica_watch_set_secs(long secs);

/* Track / progress / untrack one active job. */
void konica_watch_job_start(pappl_job_t *job);
void konica_watch_ping(pappl_job_t *job);
void konica_watch_job_done(pappl_job_t *job);

#ifdef __cplusplus
}
#endif

#endif /* KONICA_WATCH_H */
