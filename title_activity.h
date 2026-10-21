/* Title protection uses all application processes (including suspended ones)
 * and sandbox mounts. Unknown/incomplete state protects every title. */
#ifndef TITLE_ACTIVITY_H
#define TITLE_ACTIVITY_H
#define MAX_ACTIVE_TITLES 512
#define CLOSE_POLL_SEC 2
static struct {
  char titles[MAX_ACTIVE_TITLES][10];
  char origins[MAX_ACTIVE_TITLES][128];
  size_t count;
  int unknown;
  char reason[160];
  double checked;
} activity;
static char g_watch_title[10];
static int g_pending_only, g_cycle_deferred;

typedef struct pending_title {
  struct pending_title *next;
  char uid[256], title[10];
  int ps5, ready;
  double retry_at, quiet_since;
} pending_title_t;
static pending_title_t *pending_titles;
static double pending_retry_after;

static int
title_valid(const char *title) {
  if(strlen(title) != 9) return 0;
  for(int i = 0; i < 9; i++)
    if(i < 4 ? !isalpha((unsigned char)title[i]) : !isdigit((unsigned char)title[i]))
      return 0;
  return 1;
}

static void
activity_unknown(const char *fmt, ...) {
  if(!activity.unknown) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(activity.reason, sizeof activity.reason, fmt, ap);
    va_end(ap);
  }
  activity.unknown = 1;
}

static void
activity_add(const char *title, const char *origin) {
  if(!strncmp(title, "NPXS", 4)) return;
  if(!title_valid(title)) {activity_unknown("invalid application title ID: %.32s", title); return;}
  for(size_t i = 0; i < activity.count; i++)
    if(!strcmp(activity.titles[i], title)) return;
  if(activity.count == MAX_ACTIVE_TITLES) {activity_unknown("too many active title IDs"); return;}
  memcpy(activity.titles[activity.count], title, 10);
  snprintf(activity.origins[activity.count], sizeof activity.origins[0], "%s", origin);
  activity.count++;
}

/* A sandbox name may have a suffix, e.g. PPSA12345_000. Never use a mere
 * substring match between two title IDs. Unidentified sandboxes fail closed. */
static void
activity_mount(const char *path) {
  const char *p = strstr(path, "/mnt/sandbox/");
  if(!p) return;
  p += strlen("/mnt/sandbox/");
  /* Covers both title sandboxes and /mnt/sandbox/pfsmnt/TITLE-app0. */
  while(*p) {
    if(!strncmp(p, "NPXS", 4)) return;
    if(strlen(p) >= 9) {
      char title[10];
      memcpy(title, p, 9); title[9] = 0;
      if(title_valid(title) && (!p[9] || p[9] == '_' || p[9] == '/' || p[9] == '-')) {
        activity_add(title, path);
        return;
      }
    }
    p = strchr(p, '/');
    if(!p) break;
    p++;
  }
  activity_unknown("unidentified sandbox mount: %.112s", path);
}

/* Shared directory exports are not game-liveness evidence. ShadowMountPlus
 * exposes global data, storage roots and system fonts through nullfs aliases.
 * Ignore only exact known source -> same-path sandbox exports. Process checks
 * still protect a live/suspended title; content/save mounts are never exempted.
 * Reference: ShadowMountPlus src/sm_fakelib.c, commit 98c524890176c7b474fcb03387bec88c4a559861. */
static int
activity_auxiliary_export(const char *path, const char *source, const char *type) {
  const char *prefix = "/mnt/sandbox/";
  if(strcmp(type, "nullfs") || strncmp(path, prefix, strlen(prefix))) return 0;
  const char *p = path + strlen(prefix);
  if(strlen(p) < 15) return 0;
  char title[10];
  memcpy(title, p, 9); title[9] = 0;
  if(!title_valid(title) || p[9] != '_') return 0;
  p += 10;
  if(!isdigit((unsigned char)*p)) return 0;
  while(isdigit((unsigned char)*p)) p++;
  if(!strcmp(p, "/data"))
    return !strcmp(source, "/user/data") || !strcmp(source, "/data");
  if(strcmp(source, p)) return 0;
  if(!strcmp(p, "/preinst/common/font") || !strcmp(p, "/mnt/disc")) return 1;
  if(strlen(p) == 9 && (!strncmp(p, "/mnt/usb", 8) || !strncmp(p, "/mnt/ext", 8))) {
    char maximum = p[5] == 'u' ? '7' : '1';
    return p[8] >= '0' && p[8] <= maximum;
  }
  return 0;
}

static void
activity_mount_record(const char *path, const char *source, const char *type) {
  if(activity_auxiliary_export(path, source, type)) return;
  activity_mount(path);
}

static void
activity_refresh(int force) {
  double now = now_seconds();
  if(!force && activity.checked && now - activity.checked < GAME_CHECK_SEC) return;
  activity.count = 0;
  activity.unknown = 0;
  activity.reason[0] = 0;
  activity.checked = now;
#ifndef BACKUP_HOST_TEST
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  char *buf = NULL;
  int ok = 0;
  for(int attempt = 0; attempt < 4 && !ok; attempt++) {
    if(sysctl(mib, 4, NULL, &size, NULL, 0) || !size) break;
    size += 64 * sizeof(struct kinfo_proc);
    free(buf);
    buf = malloc(size);
    if(!buf) break;
    if(!sysctl(mib, 4, buf, &size, NULL, 0)) ok = 1;
    else if(errno != ENOMEM) break;
  }
  if(!ok) activity_unknown("cannot read process list (errno %d)", errno);
  else {
    size_t pos = 0;
    /* Only the PID field is needed. Different record tails must not turn a
     * valid process list into an unknown state. Keep strict bounds on the
     * prefix we actually read, and use memcpy for potentially unaligned data. */
    const size_t pid_end = offsetof(struct kinfo_proc, ki_pid) + sizeof(pid_t);
    while(pos < size) {
      size_t remaining = size - pos;
      int entry_size;
      if(remaining < sizeof entry_size) {
        activity_unknown("truncated process-list header (%zu bytes)", remaining);
        break;
      }
      memcpy(&entry_size, buf + pos, sizeof entry_size);
      if(entry_size < 0 || (size_t)entry_size < pid_end || (size_t)entry_size > remaining) {
        activity_unknown("invalid process record: %d bytes, need %zu, remaining %zu",
                         entry_size, pid_end, remaining);
        break;
      }
      pid_t pid;
      memcpy(&pid, buf + pos + offsetof(struct kinfo_proc, ki_pid), sizeof pid);
      pos += (size_t)entry_size;
      if(pid == getpid()) continue;
      app_info_t ai;
      memset(&ai, 0, sizeof ai);
      /* Non-application processes normally have no app info; sandbox mounts
       * are checked independently, even when this call fails. */
      if(sceKernelGetAppInfo(pid, &ai)) continue;
      char title[sizeof ai.title_id + 1];
      memcpy(title, ai.title_id, sizeof ai.title_id);
      title[sizeof ai.title_id] = 0;
      if(title[0]) {
        char origin[64];
        snprintf(origin, sizeof origin, "application process pid %d", (int)pid);
        activity_add(title, origin);
      }
      else if(ai.app_id)
        activity_unknown("application has no title ID (pid %d, app_id 0x%08x)",
                         (int)pid, (unsigned)ai.app_id);
    }
  }
  free(buf);
  struct statfs *mounts = NULL;
  int count = getmntinfo(&mounts, MNT_NOWAIT);
  if(count <= 0 || !mounts) activity_unknown("cannot read mount table (errno %d)", errno);
  else for(int i = 0; i < count; i++)
    activity_mount_record(mounts[i].f_mntonname, mounts[i].f_mntfromname,
                          mounts[i].f_fstypename);
#else
  const char *state = getenv("TEST_ACTIVE_TITLES");
  FILE *f = state ? fopen(state, "r") : NULL;
  if(state && !f) activity_unknown("test activity file unavailable");
  if(f) {
    char line[256];
    while(fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      if(!strcmp(line, "UNKNOWN")) activity_unknown("test activity unknown");
      else if(!strncmp(line, "MOUNT\t", 6)) {
        char *path = line + 6, *source = strchr(path, '\t');
        char *type = NULL;
        if(source) {*source++ = 0; type = strchr(source, '\t');}
        if(type) {*type++ = 0; activity_mount_record(path, source, type);}
        else activity_unknown("invalid test mount record");
      }
      else if(line[0] == '/') activity_mount(line);
      else if(line[0]) activity_add(line, "test application process");
    }
    if(ferror(f)) activity_unknown("test activity read failed");
    fclose(f);
  }
#endif
  static char last_reason[160];
  if(strcmp(last_reason, activity.reason)) {
    logq("Activity detection: %s", activity.unknown ? activity.reason : "recovered");
    snprintf(last_reason, sizeof last_reason, "%s", activity.reason);
  }
}

static int
title_blocked(const char *title, char *why, size_t len, int force) {
  activity_refresh(force);
  if(!title_valid(title) || activity.unknown) {
    snprintf(why, len, "%s", activity.unknown ? activity.reason : "unsupported save title ID");
    return 1;
  }
  for(size_t i = 0; i < activity.count; i++) {
    if(!strcmp(activity.titles[i], title)) {
      snprintf(why, len, "%s: %s", title, activity.origins[i]);
      return 1;
    }
  }
  return 0;
}

static pending_title_t *
pending_find(const char *uid, const char *title, int ps5) {
  for(pending_title_t *p = pending_titles; p; p = p->next)
    if(p->ps5 == ps5 && !strcmp(p->uid, uid) && !strcmp(p->title, title)) return p;
  return NULL;
}

static void
pending_defer(const char *uid, const char *title, int ps5) {
  g_cycle_deferred = 1;
  if(!title_valid(title)) {logq("Unsupported title ID deferred: %s", title); return;}
  pending_title_t *p = pending_find(uid, title, ps5);
  if(!p) {
    p = calloc(1, sizeof *p);
    if(!p) {logq("Cannot queue %s; regular interval will retry", title); return;}
    snprintf(p->uid, sizeof p->uid, "%s", uid);
    snprintf(p->title, sizeof p->title, "%s", title);
    p->ps5 = ps5;
    p->next = pending_titles;
    pending_titles = p;
    notify("Save backup postponed (%s).", title);
  }
  p->ready = 0;
  p->retry_at = 0;
  p->quiet_since = 0;
}

static void
pending_remove(const char *uid, const char *title, int ps5) {
  pending_title_t **link = &pending_titles;
  while(*link) {
    pending_title_t *p = *link;
    if(p->ps5 == ps5 && !strcmp(p->uid, uid) && !strcmp(p->title, title)) {
      *link = p->next; free(p); return;
    }
    link = &p->next;
  }
}

static void
pending_disable(void) {
  pending_title_t **link = &pending_titles;
  while(*link) {
    pending_title_t *p = *link;
    if(p->ps5 ? !g_ps5saves : !g_ps4saves) {*link = p->next; free(p);}
    else link = &p->next;
  }
}

static int
pending_ready(void) {
  if(!pending_titles || g_mount_stuck || now_seconds() < pending_retry_after) return 0;
  activity_refresh(1);
  int ready = 0;
  char why[160];
  double now = now_seconds();
  for(pending_title_t *p = pending_titles; p; p = p->next) {
    if(title_blocked(p->title, why, sizeof why, 0)) {
      p->quiet_since = 0;
      p->ready = 0;
    } else {
      if(!p->quiet_since) p->quiet_since = now;
      p->ready = now >= p->retry_at && now - p->quiet_since >= CLOSE_POLL_SEC;
    }
    ready |= p->ready;
  }
  return ready;
}
#endif
