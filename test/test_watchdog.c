/* Harness for konica_watch: stub PAPPL job calls, verify cancel policy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pappl/pappl.h>
#include "../src/konica_watch.h"
static int cancel_count = 0;
static int fake_id = 42;
int papplJobGetID(pappl_job_t *job) { (void)job; return fake_id; }
bool papplJobIsCanceled(pappl_job_t *job) { (void)job; return false; }
void papplJobCancel(pappl_job_t *job) { (void)job; cancel_count++; }
void papplLogJob(pappl_job_t *job, pappl_loglevel_t l, const char *fmt, ...) { (void)job; (void)l; (void)fmt; }
int main(void) {
  pappl_job_t *j = (pappl_job_t *)0x1234;
  setenv("KONICA_STUCK_SECS", "2", 1);
  konica_watch_init();
  /* Case 1: silent job must be cancelled. Poll max ~40 s. */
  konica_watch_job_start(j);
  int waited = 0;
  while (cancel_count == 0 && waited < 45) { sleep(1); waited++; }
  printf("case1 silent: cancel_count=%d after %ds %s\n", cancel_count, waited, cancel_count == 1 ? "OK" : "FAIL");
  konica_watch_job_done(j);
  /* Case 2: pinged job must survive two full periods. */
  cancel_count = 0;
  konica_watch_job_start(j);
  for (int i = 0; i < 70; i++) { konica_watch_ping(j); usleep(100000); }
  printf("case2 pinged 7s: cancel_count=%d %s\n", cancel_count, cancel_count == 0 ? "OK" : "FAIL");
  konica_watch_job_done(j);
  return (cancel_count == 0) ? 0 : 1;
}
