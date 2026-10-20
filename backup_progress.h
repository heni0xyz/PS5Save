/* Cycle progress: one unit per changed file, save image or daily snapshot.
 * Save images split their unit between staging and decrypted-file copying.
 * Only this state is shared with the notification worker. */
#ifndef BACKUP_PROGRESS_H
#define BACKUP_PROGRESS_H

#include <pthread.h>

static pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress_cond = PTHREAD_COND_INITIALIZER;
static pthread_t progress_thread;
static int progress_live, progress_running, progress_current;
static uint64_t progress_total, progress_completed, progress_bytes, progress_target;
static double progress_offset, progress_span;
static unsigned progress_last;
static int g_progress_planning, g_progress_image;

static unsigned
progress_percent_locked(void) {
  double fraction = progress_target ? (double)progress_bytes / progress_target : 0;
  if(fraction > 1) fraction = 1;
  double done = (double)progress_completed;
  if(progress_current) done += progress_offset + progress_span * fraction;
  unsigned percent = progress_total ? (unsigned)(100.0 * done / progress_total) : 0;
  if(percent > 99) percent = 99; /* completion is announced only after cleanup */
  if(percent < progress_last) percent = progress_last;
  progress_last = percent;
  return percent;
}

static void *
progress_worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&progress_mutex);
  while(progress_running) {
    struct timespec deadline;
    if(clock_gettime(CLOCK_REALTIME, &deadline)) break;
    deadline.tv_sec += 5;
    int rc = 0;
    while(progress_running && rc == 0)
      rc = pthread_cond_timedwait(&progress_cond, &progress_mutex, &deadline);
    if(!progress_running) break;
    if(rc != ETIMEDOUT) break;
    unsigned percent = progress_percent_locked();
    pthread_mutex_unlock(&progress_mutex);
    notify("Backing up... (%u%%)", percent);
    pthread_mutex_lock(&progress_mutex);
  }
  pthread_mutex_unlock(&progress_mutex);
  return NULL;
}

static void
progress_stop(void) {
  if(!progress_live) return;
  pthread_mutex_lock(&progress_mutex);
  progress_running = 0;
  pthread_cond_signal(&progress_cond);
  pthread_mutex_unlock(&progress_mutex);
  /* Also wait for any notification already being sent before the final toast. */
  pthread_join(progress_thread, NULL);
  progress_live = 0;
}

static void
progress_reset(void) {
  progress_stop();
  progress_total = progress_completed = progress_bytes = progress_target = 0;
  progress_current = progress_last = 0;
  g_progress_planning = g_progress_image = 0;
}

/* Planning happens before the worker starts and never writes or mounts saves. */
static void
progress_plan(uint64_t count) {
  progress_total += count;
}

static void
progress_start(void) {
  if(progress_live) return;
  progress_running = 1;
  int rc = pthread_create(&progress_thread, NULL, progress_worker, NULL);
  if(rc) {
    progress_running = 0;
    logq("Progress notifications unavailable: %s", strerror(rc));
    return;
  }
  progress_live = 1;
}

static void
progress_stage(uint64_t bytes, double offset, double span) {
  pthread_mutex_lock(&progress_mutex);
  progress_bytes = 0;
  progress_target = bytes;
  progress_offset = offset;
  progress_span = span;
  pthread_mutex_unlock(&progress_mutex);
}

static void
progress_begin(uint64_t bytes) {
  pthread_mutex_lock(&progress_mutex);
  if(progress_total <= progress_completed) progress_total = progress_completed + 1;
  progress_current = 1;
  progress_bytes = 0;
  progress_target = bytes;
  progress_offset = 0;
  progress_span = 1;
  pthread_mutex_unlock(&progress_mutex);
}

static void
progress_advance(uint64_t bytes) {
  pthread_mutex_lock(&progress_mutex);
  if(progress_current) {
    uint64_t left = progress_target > progress_bytes ? progress_target - progress_bytes : 0;
    progress_bytes += bytes < left ? bytes : left;
  }
  pthread_mutex_unlock(&progress_mutex);
}

static void
progress_finish(void) {
  pthread_mutex_lock(&progress_mutex);
  if(progress_current) progress_completed++;
  progress_current = 0;
  pthread_mutex_unlock(&progress_mutex);
}

#endif
