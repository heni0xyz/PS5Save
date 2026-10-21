/* Copyright (C) 2026

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/**
 * Save backup daemon for PS5.
 *
 * Reads /mnt/usb0/backup.conf and copies the listed files to
 * /mnt/usb0/backup/<original path>:
 *
 *   /data/PCSX2/Mcd001.ps2        one file
 *   /data/porpoise/saves/<star>   every file and folder below, recursively
 *   backup_interval_seconds=60      seconds between backups (default 60)
 *   backup_ps4_saves=true           enable decrypted PS4 saves (default false)
 *   backup_ps5_saves=true           enable decrypted PS5 saves (default false)
 *   max_title_size_mb=4096          size limit for all save images of one title
 *                                  in decimal MB; default 4096 = 4.096GB
 *   daily_snapshots_to_keep=3       keep a dated snapshot (a copy of the live
 *                                 backup) of the newest N days in
 *                                 /mnt/usb0/backup_daily/YYYY-MM-DD, using the
 *                                 console's local date. 0..10, default 0 = off.
 *                                 The first run on a date creates its snapshot
 *                                 and it is never overwritten that day; it is
 *                                 checked at every payload start, wake-up and
 *                                 at local midnight.
 *
 * Plain files larger than 100MB are skipped. A game's saves are backed up
 * completely or not at all: the size of the whole title id folder is checked
 * against max_title_size_mb, and the USB drive and internal storage are checked for
 * free space, before anything is mounted. The payload keeps running in the
 * background and only copies what changed. Starting it again replaces the
 * instance that is already running.
 *
 * PS4/PS5 saves are encrypted images. For each image that changed, a COPY of
 * the image is mounted (the original is only ever read), the decrypted files
 * are copied to /mnt/usb0/backup/saves/..., and the image is unmounted again.
 * A title is deferred while that same title is running, suspended or still
 * sandbox-mounted. Other titles may be backed up during gameplay. Activity is
 * checked before mounting and during copying; unknown activity fails closed.
 * Deferred titles are retried after closure, independently of the interval.
 *
 * Save mounting is based on the Playstation-5-Save-Mounter payload by n0llptr
 * (GPLv3), which in turn builds on research by cow and the garlic-savemgr
 * project by earthonion.
 **/

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "notification_icon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef BACKUP_HOST_TEST
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/sysctl.h>
#else
#include <sys/statfs.h>
#endif

#include <ps5/kernel.h>
#include <ps5/klog.h>


#ifndef USB_ROOT
#define USB_ROOT      "/mnt/usb0"
#endif
#ifndef HOME_ROOT
#define HOME_ROOT     "/user/home"
#endif
#ifndef TMP_DIR
#define TMP_DIR       "/data/save_files"
#endif
#ifndef PFS_ROOT
#define PFS_ROOT      "/mnt/pfs"
#endif
#ifndef PFSMGR_DEV
#define PFSMGR_DEV    "/dev/pfsmgr"
#endif

#define CONF_PATH     USB_ROOT "/backup.conf"
#define BACKUP_ROOT   USB_ROOT "/backup"
#define DAILY_ROOT    USB_ROOT "/backup_daily"
#define MAX_DAILY     10      /* highest value for daily_snapshots_to_keep= */
#define MAX_DAILY_DIRS 1024   /* date folders looked at when pruning */
#define TMP_IMG       TMP_DIR "/_backup_tmp_image"
#define MOUNT_POINT   PFS_ROOT "/savebackup_tmp"
#define BYTES_PER_MB  1000000ULL /* decimal megabytes */
#define MAX_FILE_SZ   (100ULL * BYTES_PER_MB)
#define DEF_GAME_MB   4096    /* default limit: all saves of one title id, in MB */
#ifndef MIN_GAME_MB
#define MIN_GAME_MB   64
#endif
#define MAX_GAME_MB   65536
#define MAX_IMAGES    512     /* save images per title id */
#define SPACE_MARGIN  (64ULL * 1024ULL * 1024ULL)
#define COPY_BUF_SZ   (1024 * 1024)

#define DEF_INTERVAL  60      /* seconds between backups */
#ifndef MIN_INTERVAL
#define MIN_INTERVAL  30
#endif
#define MAX_INTERVAL  3600
#ifndef SLICE_SEC
#define SLICE_SEC     10      /* wake-up granularity while waiting */
#endif
#ifndef LOCK_PORT
#define LOCK_PORT     9055    /* loopback port used to detect/replace a running instance */
#endif
#define MISSING_TRIES 6       /* conf missing this many slices in a row -> notify */
#define MAX_SEEN      256

#ifndef GAME_CHECK_SEC
#define GAME_CHECK_SEC 1.0    /* how often the game check is repeated while mounted */
#endif
#define SAVE_AUTHID   0x4800000000000010ULL


/* Rich notification transport follows ShadowMountPlus src/sm_log.c (GPLv3):
 * https://github.com/drakmor/ShadowMountPlus/blob/
 * 98c524890176c7b474fcb03387bec88c4a559861/src/sm_log.c
 * sceKernelSendNotificationRequest is the legacy debug-popup mechanism;
 * sceNotificationSend accepts the shell's rich JSON notification model. */
int sceNotificationSend(int user_id, bool is_logged, const char *payload);
#define NOTIFICATION_USER_ID 0xfe /* same local/broadcast user as SMP */
#define NOTIFICATION_JSON_SIZE 16384

#ifndef NOTIFICATION_ICON_DIR
#define NOTIFICATION_ICON_DIR "/user/data/ps5save"
#endif
#define NOTIFICATION_ICON_PATH NOTIFICATION_ICON_DIR "/notification.png"

static const char PS5SAVE_VERSION[] = "v2";

static int mkdir_p(const char *path);

/* Install the embedded, unmodified user artwork where ShellUI can read it.
 * No USB icon file or network service is required. Try again on a later
 * notification if installation fails. Never fall back to legacy debug popups. */
static int
prepare_notification_icon(void) {
  static int ready;
  const char *tmp = NOTIFICATION_ICON_PATH ".part";
  int fd;
  size_t pos = 0;

  if(ready && !access(NOTIFICATION_ICON_PATH, R_OK)) {
    return 1;
  }
  ready = 0;
  if(mkdir_p(NOTIFICATION_ICON_DIR)) {
    return 0;
  }
  if((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    return 0;
  }
  while(pos < sizeof notification_icon_png) {
    ssize_t n = write(fd, notification_icon_png + pos,
                      sizeof notification_icon_png - pos);
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      close(fd);
      unlink(tmp);
      return 0;
    }
    pos += (size_t)n;
  }
  int ok = fchmod(fd, 0644) == 0 && fsync(fd) == 0;
  if(close(fd)) {
    ok = 0;
  }
  if(!ok || rename(tmp, NOTIFICATION_ICON_PATH)) {
    unlink(tmp);
    return 0;
  }
  ready = 1;
  return 1;
}



/* App-info ABI used by kstuff-lite and ShadowMountPlus. The title starts at
 * 0x10: inserting an app_type field here shifts every title by four bytes.
 * https://github.com/EchoStretch/kstuff-lite/blob/main/ps5-kstuff-ldr/src/shellui_patch.c */
typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  char     title_id[14];
  char     unknown2[0x3c];
} app_info_t;
_Static_assert(offsetof(app_info_t, title_id) == 0x10, "Incorrect app-info title offset");

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);


/* save data mounting, from libSceFsInternalForVsh (see Save Mounter) */
typedef struct { uint8_t reserved; char *budgetid; } MountOpt;
typedef struct { uint8_t dummy; } UmountOpt;

int sceFsInitMountSaveDataOpt(MountOpt *opt);
int sceFsMountSaveData(MountOpt *opt, const char *path, const char *mount,
                       uint8_t *key);
int sceFsInitUmountSaveDataOpt(UmountOpt *opt);
int sceFsUmountSaveData(UmountOpt *opt, const char *mount, int handle,
                        int ignore);


typedef struct backup_stats {
  uint64_t bytes;
  unsigned copied;
  unsigned unchanged;
  unsigned skipped;
  unsigned failed;
} backup_stats_t;

typedef struct priv_state {
  uint64_t authid;
  uint8_t  caps[16];
  intptr_t jaildir;
} priv_state_t;

typedef enum { SV_OK, SV_SKIP, SV_FAILED, SV_GAME } sv_result_t;


static char *g_buf;
static long g_interval = DEF_INTERVAL;
static int g_last_failed = 0;
static char *g_seen[MAX_SEEN];
static int g_nseen = 0;

static uint64_t g_max_game = DEF_GAME_MB * BYTES_PER_MB;
static uint64_t g_file_cap = MAX_FILE_SZ;   /* per-file limit, 0 = none */
static int g_daily = 0;                     /* daily_snapshots_to_keep=N, 0 = off */
static int g_ps4saves = 0;
static int g_ps5saves = 0;
static int g_need_cleanup = 1;
static int g_mount_stuck = 0;
static int g_watch_game = 0;      /* protect the currently mounted title */
static int g_game_abort = 0;      /* abort this save image only */
static int g_backup_busy = 0;     /* one activity warning per copying pass */
static int g_start_notified = 0;  /* one startup notification per process */
static double g_last_watch = 0;
static char g_abort_why[192];


static void logq(const char *fmt, ...);

/* Escape every JSON control character and reject overflow rather than emit
 * truncated or malformed JSON. UTF-8 bytes are preserved. */
static int
notification_json_escape(char *dst, size_t size, const char *src) {
  static const char hex[] = "0123456789abcdef";
  size_t pos = 0;

  if(!size) {
    return 0;
  }
  for(const unsigned char *p = (const unsigned char *)src; *p; p++) {
    size_t need = *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    if(need >= size - pos) {
      dst[0] = 0;
      return 0;
    }
    if(*p < 0x20) {
      dst[pos++] = '\\';
      dst[pos++] = 'u';
      dst[pos++] = '0';
      dst[pos++] = '0';
      dst[pos++] = hex[*p >> 4];
      dst[pos++] = hex[*p & 15];
    } else {
      if(*p == '"' || *p == '\\') {
        dst[pos++] = '\\';
      }
      dst[pos++] = (char)*p;
    }
  }
  dst[pos] = 0;
  return 1;
}

/* Standard informative notification: status message, PS5Save label and image.
 * Use a plain local image path, as in the SMP rich notification transport.
 * No interactive actions, alternate views or legacy debug popups. */
static int
build_notification_payload(const char *message, const char *created_at,
                           const char *id, char *payload, size_t size) {
  size_t capacity = strlen(message) * 6 + 1;
  if(capacity > 2048 * 6 + 1) return 0;
  char *escaped = malloc(capacity);
  if(!escaped) return 0;
  if(!notification_json_escape(escaped, capacity, message)) {
    free(escaped);
    return 0;
  }
  int n = snprintf(payload, size,
    "{\"rawData\":{"
      "\"viewTemplateType\":\"ToastTemplateB\","
      "\"channelType\":\"SystemFeedback\","
      "\"bundleName\":\"PS5Save\",\"useCaseId\":\"IDC\","
      "\"soundEffect\":\"none\",\"toastOverwriteType\":\"No\","
      "\"isImmediate\":true,\"priority\":100,"
      "\"viewData\":{"
        "\"icon\":{\"type\":\"Url\",\"parameters\":{"
          "\"url\":\"" NOTIFICATION_ICON_PATH "\"}},"
        "\"message\":{\"body\":\"%s\"},"
        "\"subMessage\":{\"body\":\"PS5Save | Version %s\"}}},"
      "\"createdDateTime\":\"%s\",\"localNotificationId\":\"%s\"}",
    escaped, PS5SAVE_VERSION, created_at, id);
  free(escaped);
  return n >= 0 && (size_t)n < size;
}

/** Send every user-facing message through libSceNotification, with our icon. */
static void
notify_v(const char *fmt, va_list ap) {
  static unsigned serial;
  char message[2048];
  char *payload;
  char created_at[32];
  char id[96];
  struct timespec ts;
  struct tm utc;

  vsnprintf(message, sizeof message, fmt, ap);
  logq("%s", message);

  if(!prepare_notification_icon()) {
    logq("Rich notification not sent: icon installation failed (%s)",
         strerror(errno));
    return;
  }
  if(clock_gettime(CLOCK_REALTIME, &ts) ||
     !gmtime_r(&ts.tv_sec, &utc) ||
     !strftime(created_at, sizeof created_at, "%Y-%m-%dT%H:%M:%S.000Z", &utc)) {
    logq("Rich notification not sent: cannot obtain timestamp");
    return;
  }
  snprintf(id, sizeof id, "ps5save-%lld-%ld-%d-%u",
           (long long)ts.tv_sec, ts.tv_nsec, (int)getpid(), ++serial);
  payload = malloc(NOTIFICATION_JSON_SIZE);
  if(!payload) {
    logq("Rich notification not sent: not enough memory");
    return;
  }
  if(!build_notification_payload(message, created_at, id,
                                 payload, NOTIFICATION_JSON_SIZE)) {
    free(payload);
    logq("Rich notification not sent: payload too large");
    return;
  }
  int rc = sceNotificationSend(NOTIFICATION_USER_ID, true, payload);
  free(payload);
  if(rc != 0) {
    logq("sceNotificationSend failed (0x%08x)", (unsigned)rc);
  }
}


/* Serialize the timer and main thread, including icon setup and notification IDs. */
static void
notify(const char *fmt, ...) {
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  va_list ap;
  pthread_mutex_lock(&mutex);
  va_start(ap, fmt);
  notify_v(fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&mutex);
}

#include "backup_progress.h"


/* Announce actual copy work once per pass, including save-image staging
 * and daily snapshot copies. Unchanged/scanning-only passes stay quiet. */
static void
backup_activity_begin(void) {
  if(!g_backup_busy) {
    g_backup_busy = 1;
    notify("Backing up, don't unplug the USB");
    progress_start();
  }
}


/**
 * Quiet log: stdout and klog only, no on-screen notification.
 **/
static void
logq(const char *fmt, ...) {
  char msg[2048];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  printf("%s\n", msg);
  fflush(stdout);
  klog_printf("[backup] %s\n", msg);
}


/**
 * Returns non-zero if the path was reported before; remembers it otherwise.
 * Used so that a permanently oversized file is not announced every cycle.
 **/
static int
seen_before(const char *path) {
  for(int i = 0; i < g_nseen; i++) {
    if(!strcmp(g_seen[i], path)) {
      return 1;
    }
  }
  if(g_nseen < MAX_SEEN) {
    g_seen[g_nseen++] = strdup(path);
  }
  return 0;
}


static void
format_size(uint64_t bytes, char *out, size_t len) {
  if(bytes >= 1000ULL * 1000ULL * 1000ULL) {
    snprintf(out, len, "%.1fGB", (double)bytes / (1000.0 * 1000.0 * 1000.0));
  } else if(bytes >= 10ULL * 1000ULL * 1000ULL) {
    snprintf(out, len, "%.0fMB", (double)bytes / (1000.0 * 1000.0));
  } else if(bytes >= 1000ULL * 1000ULL) {
    snprintf(out, len, "%.1fMB", (double)bytes / (1000.0 * 1000.0));
  } else if(bytes >= 1000ULL) {
    snprintf(out, len, "%.0fkB", (double)bytes / 1000.0);
  } else {
    snprintf(out, len, "%lluB", (unsigned long long)bytes);
  }
}


static double
now_seconds(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


static time_t
wall_clock(void) {
  struct timespec ts;

#ifdef BACKUP_HOST_TEST
  /* the host test can set the clock through a file */
  const char *tf = getenv("TEST_NOW_FILE");
  if(tf) {
    FILE *fp = fopen(tf, "r");
    long long v;

    if(fp) {
      int ok = fscanf(fp, "%lld", &v) == 1;
      fclose(fp);
      if(ok) {
        return (time_t)v;
      }
    }
  }
#endif

  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec;
}


/**
 * mkdir -p
 **/
static int
mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t len;

  len = snprintf(tmp, sizeof tmp, "%s", path);
  if(len == 0 || len >= sizeof tmp) {
    return -1;
  }

  for(char *p = tmp + 1; *p; p++) {
    if(*p == '/') {
      *p = 0;
      if(mkdir(tmp, 0777) && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }

  if(mkdir(tmp, 0777) && errno != EEXIST) {
    return -1;
  }

  return 0;
}


/**
 * Free space (bytes) on the file system holding path.
 **/
static uint64_t
fs_free(const char *path, const char *test_env) {
  struct statfs sf;

#ifdef BACKUP_HOST_TEST
  const char *o = getenv(test_env);
  if(o) {
    return strtoull(o, NULL, 10);
  }
#else
  (void)test_env;
#endif

  if(statfs(path, &sf) || (int64_t)sf.f_bavail <= 0) {
    return 0;
  }
  return (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
}


#define usb_free()   fs_free(USB_ROOT, "TEST_USB_FREE")
#define data_free()  fs_free(TMP_DIR, "TEST_DATA_FREE")


/**
 * Total size of all regular files below path.
 **/
static void game_watch(void);

static uint64_t
dir_size(const char *path) {
  char sub[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  uint64_t total = 0;
  DIR *dir;

  if(!(dir = opendir(path))) {
    return 0;
  }
  while((dp = readdir(dir))) {
    game_watch();
    if(g_watch_game && g_game_abort) break;
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(snprintf(sub, sizeof sub, "%s/%s", path, dp->d_name) >= (int)sizeof sub ||
       lstat(sub, &st)) {
      continue;
    }
    if(S_ISDIR(st.st_mode)) {
      total += dir_size(sub);
    } else if(S_ISREG(st.st_mode)) {
      total += st.st_size;
    }
  }
  closedir(dir);
  return total;
}


/* ------------------------------------------------------------------------
 * Game detection
 * ---------------------------------------------------------------------- */

#include "title_activity.h"


/**
 * Called regularly while an image is copied or mounted. If its title appears,
 * g_game_abort is set and save-image copy loops stop as soon as possible.
 * Ordinary configured files are independent of game detection.
 **/
static void
game_watch(void) {
  char why[160];
  double t;

  if(!g_watch_game || g_game_abort) {
    return;
  }

  t = now_seconds();
  if(t - g_last_watch < GAME_CHECK_SEC) {
    return;
  }
  g_last_watch = t;

  if(title_blocked(g_watch_title, why, sizeof why, 0)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
  }
}


#include "change_index.h"


/* ------------------------------------------------------------------------
 * Plain file backup
 * ---------------------------------------------------------------------- */

/**
 * Copy a single file from src to dst. The data is written to a temporary file
 * first, so that an interrupted copy (full or removed USB drive) never
 * destroys the previous good backup.
 **/
static void
backup_file(const char *src, const char *dst, const struct stat *st,
            backup_stats_t *stats) {
  char tmp[PATH_MAX];
  char dir[PATH_MAX];
  const char *label = dst + strlen(BACKUP_ROOT);
  struct stat after;
  struct timeval tv[2];
  char *slash;
  ssize_t n;
  int in = -1;
  int out = -1;
  int ok = 0;
  int aborted = 0;
  unsigned reads = 0;

  int progress_started = 0;
  if(g_progress_planning) {
    if((!g_file_cap || (uint64_t)st->st_size <= g_file_cap) &&
       !file_current(dst, st))
      progress_plan(1);
    return;
  }

  /* per-file limit; not applied inside game saves, which are judged per game */
  if(g_file_cap && (uint64_t)st->st_size > g_file_cap) {
    if(!seen_before(label)) {
      logq("Abnormal size of %s; skipping backup", label);
      notify("File skipped: too large.");
    }
    stats->skipped++;
    return;
  }

  game_watch();
  if(g_watch_game && g_game_abort) {
    return;
  }

  tmp[0] = 0;
  if(snprintf(tmp, sizeof tmp, "%s.part", dst) >= (int)sizeof tmp) {
    tmp[0] = 0;
    goto done;
  }

  /* Exact source and destination metadata cached after a successful copy. */
  if(file_current(dst, st)) {
    stats->unchanged++;
    if(g_progress_image) progress_advance(st->st_size);
    return;
  }

  /* make sure the USB drive can hold the new file */
  if(usb_free() < (uint64_t)st->st_size + SPACE_MARGIN) {
    logq("Failed to backup %s (not enough space on USB)", label);
    stats->failed++;
    return;
  }

  snprintf(dir, sizeof dir, "%s", dst);
  if((slash = strrchr(dir, '/')) && slash != dir) {
    *slash = 0;
    if(mkdir_p(dir)) {
      goto done;
    }
  }

  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if(!g_progress_image) {
    progress_begin(st->st_size);
    progress_started = 1;
  }
  backup_activity_begin();
  if((out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    goto done;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
    if(++reads) {
      game_watch();
      if(g_watch_game && g_game_abort) {
        aborted = 1;
        goto done;
      }
    }
  }
  if(n < 0) {
    goto done;
  }

  if(fstat(in, &after) || !source_same(st, &after)) {
    logq("Source changed during copy: %s", src);
    goto done;
  }
  if(fsync(out)) {
    goto done;
  }
  if(close(out)) {
    out = -1;
    goto done;
  }
  out = -1;

  /* keep the source mtime so the next cycle can detect "unchanged" */
  tv[0].tv_sec = st->st_mtime;
  tv[0].tv_usec = 0;
  tv[1] = tv[0];
  utimes(tmp, tv);

  if(rename(tmp, dst)) {
    goto done;
  }

  index_update(dst, st);
  ok = 1;

done:
  if(progress_started) progress_finish();
  if(in >= 0) {
    close(in);
  }
  if(out >= 0) {
    close(out);
  }

  if(ok) {
    stats->copied++;
    stats->bytes += st->st_size;
  } else {
    if(tmp[0]) {
      unlink(tmp);
    }
    if(!aborted) {
      logq("Failed to backup %s", label);
      stats->failed++;
    }
  }
}


/**
 * Back up everything below the directory src into dst, recursively.
 **/
static void
backup_tree(const char *src, const char *dst, backup_stats_t *stats) {
  char spath[PATH_MAX];
  char dpath[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  DIR *dir;

  if(!(dir = opendir(src))) {
    logq("Failed to backup %s", dst + strlen(BACKUP_ROOT));
    stats->failed++;
    return;
  }

  while((dp = readdir(dir)) && !(g_watch_game && g_game_abort)) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }

    if(snprintf(spath, sizeof spath, "%s/%s", src, dp->d_name) >= (int)sizeof spath ||
       snprintf(dpath, sizeof dpath, "%s/%s", dst, dp->d_name) >= (int)sizeof dpath) {
      continue;
    }

    if(lstat(spath, &st)) {
      stats->failed++;
      continue;
    }

    if(S_ISDIR(st.st_mode)) {
      backup_tree(spath, dpath, stats);
    } else if(S_ISREG(st.st_mode)) {
      backup_file(spath, dpath, &st, stats);
    }
    /* symlinks and special files are ignored */
  }

  closedir(dir);
}


/* ------------------------------------------------------------------------
 * PS4 / PS5 save images
 * ---------------------------------------------------------------------- */

static void
priv_elevate(priv_state_t *old) {
  pid_t me = getpid();
  uint8_t caps[16];

  old->authid = kernel_get_ucred_authid(me);
  memset(old->caps, 0, sizeof old->caps);
  kernel_get_ucred_caps(me, old->caps);
  old->jaildir = kernel_get_proc_jaildir(me);

  kernel_set_ucred_authid(me, SAVE_AUTHID);
  memset(caps, 0xFF, sizeof caps);
  kernel_set_ucred_caps(me, caps);
  kernel_set_proc_rootdir(me, kernel_get_root_vnode());
  kernel_set_proc_jaildir(me, 0);
}


static void
priv_restore(const priv_state_t *old) {
  pid_t me = getpid();

  kernel_set_ucred_authid(me, old->authid);
  kernel_set_ucred_caps(me, old->caps);
  kernel_set_proc_jaildir(me, old->jaildir);
}


/* Configuration booleans accept exactly the documented lowercase tokens. */
static int
parse_bool(const char *value, int *out) {
  if(!strcmp(value, "true")) {
    *out = 1;
    return 1;
  }
  if(!strcmp(value, "false")) {
    *out = 0;
    return 1;
  }
  logq("Invalid boolean setting: %s (expected false or true)", value);
  return 0;
}

static int
is_hex_name(const char *s) {
  size_t len = strlen(s);

  if(len == 0 || len > 8) {
    return 0;
  }
  for(size_t i = 0; i < len; i++) {
    if(!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}


/**
 * Free space check for the temporary image copy in internal storage.
 **/
static int
enough_space(uint64_t need) {
  mkdir_p(TMP_DIR);
  return data_free() >= need;
}


/**
 * Where the decrypted files of one save image are backed up.
 **/
static void
save_dst_dir(char *out, size_t len, int ps5, const char *uid,
             const char *title, const char *save) {
  snprintf(out, len, "%s/saves/%s/%s/%s/%s", BACKUP_ROOT,
           ps5 ? "ps5" : "ps4", uid, title, save);
}


/**
 * Copy src to dst (plain, no temp file). Returns 0 on success, -1 on error
 * and -2 if the copy was stopped because a game appeared.
 **/
static int
copy_plain(const char *src, const char *dst) {
  unsigned reads = 0;
  int rc = -1;
  ssize_t n;
  int in;
  int out;

  if((in = open(src, O_RDONLY)) < 0) {
    return -1;
  }
  backup_activity_begin();
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    close(in);
    return -1;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
    if(++reads) {
      game_watch();
      if(g_game_abort) {
        rc = -2;
        goto done;
      }
    }
  }
  if(n == 0) {
    rc = 0;
  }

done:
  close(in);
  if(close(out)) {
    rc = rc ? rc : -1;
  }
  return rc;
}


/**
 * Decrypt the sealed save key (0x60 bytes) through the pfsmgr device.
 **/
static int
decrypt_key(const uint8_t *sealed, uint8_t key[0x20]) {
  uint8_t *data;
  int fd;
  int rc;

  if(!(data = calloc(1, 0x100))) {
    return -1;
  }
  memcpy(data, sealed, 0x60);

  if((fd = open(PFSMGR_DEV, O_RDWR)) < 0) {
    free(data);
    return -1;
  }
  rc = ioctl(fd, 0xc0845302, data);
  close(fd);

  if(rc >= 0) {
    memcpy(key, data + 0x60, 0x20);
  }
  free(data);
  return rc < 0 ? -1 : 0;
}


/**
 * Unmount the temporary save mount, retrying a few times.
 **/
static int
unmount_save(void) {
  UmountOpt uopt;
  int ret = -1;

  for(int i = 0; i < 5; i++) {
    memset(&uopt, 0, sizeof uopt);
    sceFsInitUmountSaveDataOpt(&uopt);
    if((ret = sceFsUmountSaveData(&uopt, MOUNT_POINT, 0, 0)) >= 0) {
      sync();
      return 0;
    }
    sleep(1);
  }

  logq("umount failed (0x%08x)", ret);
  return -1;
}


/**
 * Remove anything a previous (killed) instance may have left behind.
 **/
static void
cleanup_stale(void) {
  UmountOpt uopt;

  memset(&uopt, 0, sizeof uopt);
  sceFsInitUmountSaveDataOpt(&uopt);
  sceFsUmountSaveData(&uopt, MOUNT_POINT, 0, 0); /* fails if not mounted */

  if(rmdir(MOUNT_POINT) && errno != ENOENT) {
    g_mount_stuck = 1;
    return;
  }
  g_mount_stuck = 0;
  unlink(TMP_IMG);
}


static int
stamp_current(const char *dst_dir, const struct stat *img) {
  char path[PATH_MAX];
  file_signature_t stored, current = file_signature(img);
  snprintf(path, sizeof path, "%s/.image_stamp", dst_dir);
  FILE *f = fopen(path, "rb");
  if(!f) return 0;
  char magic[8];
  int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PS5IMG2\n", 8) &&
           fread(&stored, 1, sizeof stored, f) == sizeof stored &&
           !memcmp(&stored, &current, sizeof stored);
  fclose(f);
  return ok;
}

static void
stamp_write(const char *dst_dir, const struct stat *img) {
  char path[PATH_MAX], part[PATH_MAX];
  file_signature_t sig = file_signature(img);
  mkdir_p(dst_dir);
  snprintf(path, sizeof path, "%s/.image_stamp", dst_dir);
  snprintf(part, sizeof part, "%s.part", path);
  FILE *f = fopen(part, "wb");
  if(!f) return;
  int ok = fwrite("PS5IMG2\n", 1, 8, f) == 8 &&
           fwrite(&sig, 1, sizeof sig, f) == sizeof sig;
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(part, path)) unlink(part);
}


/**
 * Back up one save image. The original is only read: a copy is mounted.
 **/
static sv_result_t
backup_save_image(const char *uid, const char *title, const char *tdir,
                  const char *name, int ps5, backup_stats_t *stats) {
  char image[PATH_MAX];
  char label[PATH_MAX];
  char dst_dir[PATH_MAX];
  char why[160];
  uint8_t sealed[0x60];
  uint8_t key[0x20];
  const char *save = name;
  sv_result_t result = SV_FAILED;
  unsigned failed_before;
  struct stat ist, after;
  MountOpt mopt;
  int mounted = 0;
  int have_tmp = 0;
  int ret;
  int fd;

  g_game_abort = 0;
  snprintf(g_watch_title, sizeof g_watch_title, "%s", title);

  if(!strncmp(save, "sdimg_", 6)) {
    save += 6;
  }

  snprintf(image, sizeof image, "%s/%s", tdir, name);
  save_dst_dir(dst_dir, sizeof dst_dir, ps5, uid, title, save);
  snprintf(label, sizeof label, "%s", dst_dir + strlen(BACKUP_ROOT));

  if(stat(image, &ist) || !S_ISREG(ist.st_mode)) {
    return SV_SKIP;
  }

  /* nothing new since the last backup: no need to mount anything */
  if(stamp_current(dst_dir, &ist)) {
    stats->unchanged++;
    return SV_SKIP;
  }

  /* the size of the whole game was already checked per title id */
  if(ist.st_size < 0x860) {
    logq("Failed to backup %s (not a save image)", label);
    stats->failed++;
    return SV_FAILED;
  }

  /* Use the image size as a conservative bound; do not rescan the USB tree. */
  if(usb_free() < (uint64_t)ist.st_size + SPACE_MARGIN) {
    logq("Failed to backup %s (not enough space on USB)", label);
    stats->failed++;
    return SV_FAILED;
  }

  /* Never stage/mount this title while it is running or suspended. */
  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    return SV_GAME;
  }

  /* sealed key: PS4 keeps it in a companion .bin file, PS5 inside the image */
  if(ps5) {
    if((fd = open(image, O_RDONLY)) < 0) {
      goto fail;
    }
    ret = pread(fd, sealed, sizeof sealed, 0x800);
    close(fd);
  } else {
    char bin[PATH_MAX];

    snprintf(bin, sizeof bin, "%s/%s.bin", tdir, save);
    if((fd = open(bin, O_RDONLY)) < 0) {
      goto fail;
    }
    ret = read(fd, sealed, sizeof sealed);
    close(fd);
  }
  if(ret != (int)sizeof sealed) {
    goto fail;
  }

  if(!enough_space((uint64_t)ist.st_size + SPACE_MARGIN)) {
    logq("Failed to backup %s (not enough free space)", label);
    stats->failed++;
    return SV_FAILED;
  }

  g_watch_game = 1;
  g_last_watch = now_seconds();

  /* work on a private copy; the original image is never mounted */
  mkdir_p(TMP_DIR);
  unlink(TMP_IMG);
  have_tmp = 1;
  g_progress_image = 1;
  progress_begin(ist.st_size);
  progress_stage(ist.st_size, 0, 0.5);
  ret = copy_plain(image, TMP_IMG);
  if(ret == -2) {
    result = SV_GAME;
    goto cleanup;
  }
  if(ret < 0) {
    goto fail_cleanup;
  }

  /* last check right before the mount */
  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    result = SV_GAME;
    goto cleanup;
  }

  if(stat(image, &after) || !source_same(&ist, &after)) {
    logq("Save image changed while staging: %s", label);
    goto fail_cleanup;
  }

  if(decrypt_key(sealed, key)) {
    goto fail_cleanup;
  }

  if(mkdir(PFS_ROOT, 0777) && errno != EEXIST) {
    goto fail_cleanup;
  }
  mkdir(MOUNT_POINT, 0777);

  memset(&mopt, 0, sizeof mopt);
  sceFsInitMountSaveDataOpt(&mopt);
  mopt.budgetid = "system";

  if(title_blocked(title, why, sizeof why, 1)) {
    g_game_abort = 1;
    snprintf(g_abort_why, sizeof g_abort_why, "%s", why);
    result = SV_GAME;
    goto cleanup;
  }
  if(sceFsMountSaveData(&mopt, TMP_IMG, MOUNT_POINT, key) < 0) {
    rmdir(MOUNT_POINT);
    goto fail_cleanup;
  }
  mounted = 1;

  progress_stage(dir_size(MOUNT_POINT), 0.5, 0.5);
  failed_before = stats->failed;
  g_file_cap = 0;     /* the whole game was already judged against its limit */
  backup_tree(MOUNT_POINT, dst_dir, stats);
  g_file_cap = MAX_FILE_SZ;

  game_watch();
  if(title_blocked(title, why, sizeof why, 1)) g_game_abort = 1;
  if(g_game_abort) {
    result = SV_GAME;
  } else if(stat(image, &after) || !source_same(&ist, &after)) {
    logq("Save image changed during backup: %s", label);
  } else if(stats->failed == failed_before) {
    result = SV_OK;
  }
  goto cleanup;

fail_cleanup:
  result = SV_FAILED;

cleanup:
  /* the image must ALWAYS be unmounted again */
  if(mounted) {
    if(unmount_save()) {
      g_mount_stuck = 1;
      logq("Failed to unmount save image; PS4/PS5 saves backup disabled");
      notify("Save backup paused: cleanup failed.");
      g_watch_game = 0;
      progress_finish();
      g_progress_image = 0;
      stats->failed++;
      return SV_FAILED;
    }
    rmdir(MOUNT_POINT);
  }
  if(have_tmp) {
    unlink(TMP_IMG);
  }
  g_watch_game = 0;
  progress_finish();
  g_progress_image = 0;

  if(result == SV_OK) {
    stamp_write(dst_dir, &ist);
  } else if(result == SV_FAILED) {
    logq("Failed to backup %s", label);
    stats->failed++;
  }
  return result;

fail:
  logq("Failed to backup %s", label);
  stats->failed++;
  return SV_FAILED;
}


typedef struct img_info {
  char     name[256];
  uint64_t size;
  int      needed;       /* changed since the last backup */
} img_info_t;


/**
 * Is this directory entry a save image we should back up?
 **/
static int
is_save_image(const char *n, int ps5) {
  size_t len = strlen(n);

  if(n[0] == '.') {
    return 0;
  }
  if(len > 4 && !strcmp(n + len - 4, ".bin")) {
    return 0;                       /* PS4 sealed key */
  }
  if(!strncmp(n, "sdimg_sce_bu_", 13) || !strncmp(n, "sce_bu_", 7)) {
    return 0;                       /* system backup copies */
  }
  if(!ps5 && strncmp(n, "sdimg_", 6)) {
    return 0;                       /* PS4 images are always sdimg_* */
  }
  return 1;
}


/**
 * Scan <HOME>/<uid>/savedata (PS4) or savedata_prospero (PS5).
 *
 * A game (title id folder) is backed up completely or not at all: the size of
 * all its images is checked against the per-game limit, and the USB drive and
 * internal storage are checked for room, before the first image is mounted.
 **/
static void
scan_saves(const char *uid, int ps5, backup_stats_t *stats) {
  char base[PATH_MAX];
  img_info_t *imgs;
  struct dirent *td;
  DIR *bd;

  snprintf(base, sizeof base, "%s/%s/%s", HOME_ROOT, uid,
           ps5 ? "savedata_prospero" : "savedata");
  if(!(bd = opendir(base))) {
    return;
  }
  if(!(imgs = calloc(MAX_IMAGES, sizeof *imgs))) {
    closedir(bd);
    return;
  }

  while((td = readdir(bd)) && !g_mount_stuck) {
    char tdir[PATH_MAX];
    char tlabel[PATH_MAX];
    char dst_dir[PATH_MAX];
    char key[PATH_MAX + 8];
    char a[32];
    char b[32];
    uint64_t total = 0;       /* all images of the game */
    uint64_t need_total = 0;  /* images that must be backed up now */
    uint64_t max_need = 0;
    uint64_t required;
    int n = 0;
    int needed = 0;
    int overflow = 0;
    struct dirent *ie;
    struct stat st;
    DIR *id;

    if(td->d_name[0] == '.') {
      continue;
    }
    if(snprintf(tdir, sizeof tdir, "%s/%s", base, td->d_name) >= (int)sizeof tdir ||
       stat(tdir, &st) || !S_ISDIR(st.st_mode)) {
      continue;
    }
    pending_title_t *queued = pending_find(uid, td->d_name, ps5);
    if(g_pending_only && (!queued || !queued->ready)) continue;
    if(!g_progress_planning && queued) {
      queued->ready = 0;
      queued->retry_at = now_seconds() + g_interval;
    }
    if(!(id = opendir(tdir))) {
      continue;
    }

    /* pass 1: size up the whole game without mounting anything */
    while((ie = readdir(id))) {
      const char *save;
      char path[PATH_MAX];

      if(!is_save_image(ie->d_name, ps5)) {
        continue;
      }
      if(snprintf(path, sizeof path, "%s/%s", tdir, ie->d_name) >= (int)sizeof path ||
         stat(path, &st) || !S_ISREG(st.st_mode)) {
        continue;
      }
      if(n >= MAX_IMAGES) {
        overflow = 1;
        break;
      }

      save = ie->d_name;
      if(!strncmp(save, "sdimg_", 6)) {
        save += 6;
      }
      save_dst_dir(dst_dir, sizeof dst_dir, ps5, uid, td->d_name, save);

      snprintf(imgs[n].name, sizeof imgs[n].name, "%s", ie->d_name);
      imgs[n].size = st.st_size;
      imgs[n].needed = !stamp_current(dst_dir, &st);
      total += st.st_size;
      if(imgs[n].needed) {
        needed++;
        need_total += st.st_size;
        /* Image size bounds decrypted data without walking the USB tree. */
        if((uint64_t)st.st_size > max_need) {
          max_need = st.st_size;
        }
      }
      n++;
    }
    closedir(id);

    if(n == 0) {
      continue;
    }

    if(g_progress_planning) {
      if(!overflow && total <= g_max_game) progress_plan(needed);
      continue;
    }

    snprintf(tlabel, sizeof tlabel, "/saves/%s/%s/%s", ps5 ? "ps5" : "ps4",
             uid, td->d_name);

    /* 1. per-game size limit: the whole game or nothing */
    if(overflow || total > g_max_game) {
      snprintf(key, sizeof key, "size:%s", tlabel);
      if(!seen_before(key)) {
        format_size(total, a, sizeof a);
        format_size(g_max_game, b, sizeof b);
        logq("Abnormal size of %s; skipping backup (%s%s, limit %s)", tlabel,
             overflow ? ">" : "", a, b);
        notify("Game saves skipped: size limit exceeded.");
      }
      stats->skipped++;
      continue;
    }

    if(!needed) {
      pending_remove(uid, td->d_name, ps5);
      stats->unchanged += n;
      continue;
    }

    char why[160];
    if(title_blocked(td->d_name, why, sizeof why, 1)) {
      pending_defer(uid, td->d_name, ps5);
      logq("Deferred %s: %s", td->d_name, why);
      continue;
    }

    /* 2. the largest image is copied to internal storage first */
    if(!enough_space(max_need + SPACE_MARGIN)) {
      snprintf(key, sizeof key, "tmp:%s", tlabel);
      if(!seen_before(key)) {
        format_size(max_need + SPACE_MARGIN, a, sizeof a);
        format_size(data_free(), b, sizeof b);
        logq("Not enough internal storage for %s (need %s, free %s); "
             "skipping backup", tlabel, a, b);
        notify("Game saves skipped: not enough console storage.");
      }
      stats->skipped++;
      continue;
    }

    /* 3. the USB drive must be able to hold everything that is going to be
     * written (image sizes are an upper bound of the decrypted content) */
    required = need_total + SPACE_MARGIN;
    if(usb_free() < required) {
      snprintf(key, sizeof key, "usb:%s", tlabel);
      if(!seen_before(key)) {
        format_size(required, a, sizeof a);
        format_size(usb_free(), b, sizeof b);
        logq("Not enough space on USB for %s (need ~%s, free %s); "
             "skipping backup", tlabel, a, b);
        notify("Game saves skipped: not enough USB space.");
      }
      stats->skipped++;
      continue;
    }

    /* pass 2: back up only this safe title; other titles remain independent. */
    int complete = 1;
    for(int i = 0; i < n && !g_mount_stuck; i++) {
      if(!imgs[i].needed) {
        stats->unchanged++;
        continue;
      }
      sv_result_t result = backup_save_image(uid, td->d_name, tdir, imgs[i].name, ps5, stats);
      if(result == SV_GAME) {
        pending_defer(uid, td->d_name, ps5);
        complete = 0;
        break;
      }
      if(result == SV_FAILED) complete = 0;
    }
    g_game_abort = 0;
    if(complete) pending_remove(uid, td->d_name, ps5);
  }

  free(imgs);
  closedir(bd);
}


/**
 * Back up decrypted PS4/PS5 saves for every user, as enabled in backup.conf.
 **/
static void
backup_saves(backup_stats_t *stats) {
  priv_state_t old;
  struct dirent *ud;
  DIR *hd;

  priv_elevate(&old);

  if(!g_progress_planning && (g_need_cleanup || g_mount_stuck)) {
    cleanup_stale();
    g_need_cleanup = 0;
  }
  if(g_mount_stuck) {
    logq("A previous save mount could not be removed; saves backup disabled");
    stats->failed++;
    priv_restore(&old);
    return;
  }

  if((hd = opendir(HOME_ROOT))) {
    while((ud = readdir(hd)) && !g_mount_stuck) {
      if(!is_hex_name(ud->d_name)) {
        continue;
      }
      if(g_ps4saves) {
        scan_saves(ud->d_name, 0, stats);
      }
      if(g_ps5saves && !g_mount_stuck) {
        scan_saves(ud->d_name, 1, stats);
      }
    }
    closedir(hd);
  }

  g_watch_game = 0;
  priv_restore(&old);
}


/* ------------------------------------------------------------------------
 * backup.conf handling and the main loop
 * ---------------------------------------------------------------------- */

/* Match a setting name, allowing spaces before and after '='. */
static const char *
config_value(const char *line, const char *key, const char *legacy) {
  const char *names[2] = {key, legacy};
  for(int i = 0; i < 2; i++) {
    size_t n = strlen(names[i]);
    if(strncmp(line, names[i], n)) {
      continue;
    }
    const char *p = line + n;
    while(*p == ' ' || *p == '\t') p++;
    if(*p++ != '=') continue;
    while(*p == ' ' || *p == '\t') p++;
    return p;
  }
  return NULL;
}

static int
config_number(const char *value, long min, long max, long *out) {
  char *end;
  errno = 0;
  long v = strtol(value, &end, 10);
  if(errno || end == value || *end) {
    logq("Invalid numeric setting: %s", value);
    return 0;
  }
  if(v < min || v > max) {
    logq("Numeric setting out of range: %s (expected %ld..%ld)",
         value, min, max);
    return 0;
  }
  *out = v;
  return 1;
}

/**
 * Handle a single line from backup.conf.
 **/
static void
backup_entry(char *line, backup_stats_t *stats) {
  char dst[PATH_MAX];
  struct stat st;
  size_t len;

  /* trim whitespace and CR/LF */
  while(*line == ' ' || *line == '\t') {
    line++;
  }
  len = strlen(line);
  while(len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                    line[len-1] == ' '  || line[len-1] == '\t')) {
    line[--len] = 0;
  }

  if(len == 0 || line[0] == '#') {
    return;
  }

  /* Keep legacy key aliases for existing backup.conf files. All MB values
   * now mean decimal megabytes, including the legacy max_game_mb key. */
  const char *value;
  if((value = config_value(line, "backup_interval_seconds", "interval"))) {
    long v;
    if(!config_number(value, MIN_INTERVAL, MAX_INTERVAL, &v)) {
      return;
    }
    g_interval = v;
    return;
  }
  if((value = config_value(line, "max_title_size_mb", "max_game_mb"))) {
    long v;
    if(!config_number(value, MIN_GAME_MB, MAX_GAME_MB, &v)) {
      return;
    }
    g_max_game = (uint64_t)v * BYTES_PER_MB;
    return;
  }
  if((value = config_value(line, "daily_snapshots_to_keep", "dailybackups"))) {
    long v;
    if(!config_number(value, 0, MAX_DAILY, &v)) {
      return;
    }
    g_daily = (int)v;
    return;
  }
  if((value = config_value(line, "backup_ps4_saves", "ps4saves"))) {
    parse_bool(value, &g_ps4saves);
    return;
  }
  if((value = config_value(line, "backup_ps5_saves", "ps5saves"))) {
    parse_bool(value, &g_ps5saves);
    return;
  }

  if(g_pending_only) return;

  /* only absolute paths are valid entries */
  if(line[0] != '/') {
    return;
  }

  /* strip the trailing star wildcard (and the slash before it) */
  if(line[len-1] == '*') {
    line[--len] = 0;
    while(len > 1 && line[len-1] == '/') {
      line[--len] = 0;
    }
  }

  /* never back up the backup itself */
  if(!strncmp(line, USB_ROOT, strlen(USB_ROOT))) {
    return;
  }

  if(snprintf(dst, sizeof dst, "%s%s", BACKUP_ROOT, line) >= (int)sizeof dst) {
    return;
  }

  if(stat(line, &st)) {
    logq("Failed to backup %s", line);
    stats->failed++;
    return;
  }

  if(S_ISDIR(st.st_mode)) {
    backup_tree(line, dst, stats);
  } else if(S_ISREG(st.st_mode)) {
    backup_file(line, dst, &st, stats);
  }
}


/* ------------------------------------------------------------------------
 * Daily snapshots (daily_snapshots_to_keep=N)
 *
 * A snapshot is a copy of the live backup folder, stored as
 * /mnt/usb0/backup_daily/YYYY-MM-DD (console local date). The first run on a
 * given date creates it and it is never touched again that day. Only the N
 * newest date folders are kept.
 * ---------------------------------------------------------------------- */

typedef struct clock_info {
  char date[16];        /* YYYY-MM-DD in console local time */
  int  offset_min;      /* local time minus UTC, in minutes */
  long to_midnight;     /* seconds until the next local midnight */
} clock_info_t;


static void
local_clock(time_t now, clock_info_t *ci) {
  struct tm lt;
  struct tm gt;
  int dd;

  memset(&lt, 0, sizeof lt);
  memset(&gt, 0, sizeof gt);
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);

  snprintf(ci->date, sizeof ci->date, "%04d-%02d-%02d", lt.tm_year + 1900,
           lt.tm_mon + 1, lt.tm_mday);

  /* offset between local time and UTC, taking a date/year change into account */
  dd = lt.tm_yday - gt.tm_yday;
  if(dd > 1) {
    dd = -1;
  } else if(dd < -1) {
    dd = 1;
  }
  ci->offset_min = dd * 1440 + (lt.tm_hour * 60 + lt.tm_min) -
                   (gt.tm_hour * 60 + gt.tm_min);

  ci->to_midnight = 86400 - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
}


static void
format_offset(int offset_min, char *out, size_t len) {
  int a = offset_min < 0 ? -offset_min : offset_min;

  snprintf(out, len, "UTC%c%d:%02d", offset_min < 0 ? '-' : '+', a / 60, a % 60);
}


/**
 * Exactly YYYY-MM-DD. Only folders with such a name are ever deleted.
 **/
static int
is_date_name(const char *n) {
  if(strlen(n) != 10) {
    return 0;
  }
  for(int i = 0; i < 10; i++) {
    if(i == 4 || i == 7) {
      if(n[i] != '-') {
        return 0;
      }
    } else if(!isdigit((unsigned char)n[i])) {
      return 0;
    }
  }
  return 1;
}


static int
is_date_part_name(const char *n) {
  size_t len = strlen(n);
  char date[11];

  if(len != 15 || strcmp(n + 10, ".part")) {
    return 0;
  }
  memcpy(date, n, 10);
  date[10] = 0;
  return is_date_name(date);
}


/**
 * Delete a file or a whole directory tree.
 **/
static int
remove_tree(const char *path) {
  char sub[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int rc = 0;
  DIR *dir;

  if(lstat(path, &st)) {
    return errno == ENOENT ? 0 : -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    return unlink(path);
  }
  if(!(dir = opendir(path))) {
    return -1;
  }
  while((dp = readdir(dir))) {
    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(snprintf(sub, sizeof sub, "%s/%s", path, dp->d_name) >= (int)sizeof sub ||
       remove_tree(sub)) {
      rc = -1;
    }
  }
  closedir(dir);

  if(rmdir(path)) {
    rc = -1;
  }
  return rc;
}


/**
 * Copy one regular file, keeping its modification time.
 **/
static int
copy_file_keep_time(const char *src, const char *dst, const struct stat *st) {
  struct timeval tv[2];
  ssize_t n;
  int rc = -1;
  int in;
  int out;

  if((in = open(src, O_RDONLY)) < 0) {
    return -1;
  }
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
    close(in);
    return -1;
  }

  while((n = read(in, g_buf, COPY_BUF_SZ)) > 0) {
    char *p = g_buf;
    while(n > 0) {
      ssize_t w = write(out, p, n);
      if(w < 0) {
        if(errno == EINTR) {
          continue;
        }
        goto done;
      }
      progress_advance(w);
      p += w;
      n -= w;
    }
  }
  if(n == 0) {
    rc = 0;
  }

done:
  close(in);
  if(close(out)) {
    rc = -1;
  }
  if(!rc) {
    tv[0].tv_sec = st->st_mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    utimes(dst, tv);
  }
  return rc;
}


/**
 * Copy a directory tree from src to dst. Internal bookkeeping files of the
 * live backup (.part, .image_stamp) are not part of a snapshot.
 **/
static int
copy_tree(const char *src, const char *dst, uint64_t *bytes) {
  char spath[PATH_MAX];
  char dpath[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int rc = 0;
  DIR *dir;

  if(mkdir(dst, 0777) && errno != EEXIST) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((dp = readdir(dir)) && !rc) {
    size_t len = strlen(dp->d_name);

    if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }
    if(!strcmp(dp->d_name, ".ps5save-index") ||
       !strcmp(dp->d_name, ".image_stamp") ||
       (len > 5 && !strcmp(dp->d_name + len - 5, ".part"))) {
      continue;
    }
    if(snprintf(spath, sizeof spath, "%s/%s", src, dp->d_name) >= (int)sizeof spath ||
       snprintf(dpath, sizeof dpath, "%s/%s", dst, dp->d_name) >= (int)sizeof dpath ||
       lstat(spath, &st)) {
      rc = -1;
      break;
    }

    if(S_ISDIR(st.st_mode)) {
      rc = copy_tree(spath, dpath, bytes);
    } else if(S_ISREG(st.st_mode)) {
      rc = copy_file_keep_time(spath, dpath, &st);
      if(!rc) {
        *bytes += st.st_size;
      }
    }
  }

  closedir(dir);
  return rc;
}


static int
cmp_desc(const void *a, const void *b) {
  return strcmp((const char*)b, (const char*)a);
}


/**
 * Keep only the newest g_daily date folders. Today's folder is never deleted,
 * and nothing but folders named exactly YYYY-MM-DD (and stale YYYY-MM-DD.part
 * folders of interrupted copies) is ever touched.
 **/
static void
daily_prune(const char *today) {
  char (*names)[11];
  char (*stale)[16];
  char path[PATH_MAX];
  struct dirent *dp;
  struct stat st;
  int n = 0;
  int ns = 0;
  DIR *dir;

  names = malloc((size_t)MAX_DAILY_DIRS * sizeof *names);
  stale = malloc((size_t)MAX_DAILY_DIRS * sizeof *stale);
  if(!names || !stale || !(dir = opendir(DAILY_ROOT))) {
    free(names);
    free(stale);
    return;
  }

  /* collect first, delete afterwards (never modify a directory while reading it) */
  while((dp = readdir(dir))) {
    if(snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, dp->d_name) >= (int)sizeof path) {
      continue;
    }
    if(is_date_part_name(dp->d_name)) {
      /* nothing is being copied while we prune, so this is stale */
      if(ns < MAX_DAILY_DIRS) {
        memcpy(stale[ns++], dp->d_name, 16);
      }
    } else if(is_date_name(dp->d_name) && n < MAX_DAILY_DIRS &&
              !stat(path, &st) && S_ISDIR(st.st_mode)) {
      memcpy(names[n++], dp->d_name, 11);
    }
  }
  closedir(dir);

  for(int i = 0; i < ns; i++) {
    snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, stale[i]);
    remove_tree(path);
  }

  qsort(names, n, sizeof *names, cmp_desc);

  for(int i = g_daily; i < n; i++) {
    if(!strcmp(names[i], today)) {
      continue;
    }
    snprintf(path, sizeof path, "%s/%s", DAILY_ROOT, names[i]);
    if(!remove_tree(path)) {
      logq("Daily backup %s deleted (keeping newest %d)", names[i], g_daily);
      notify("Old daily snapshot removed.");
    } else {
      logq("Failed to delete old daily backup %s", names[i]);
    }
  }

  free(names);
  free(stale);
}


/**
 * Create today's snapshot if there is none yet. Called after every live
 * backup pass, which covers every payload start, every wake-up from rest mode
 * and the midnight wake-up.
 **/
static void
daily_check(void) {
  char final[PATH_MAX];
  char part[PATH_MAX];
  char key[PATH_MAX];
  char tz[24];
  char a[32];
  char b[32];
  clock_info_t ci;
  struct stat st;
  uint64_t bytes = 0;
  uint64_t need;

  if(g_daily <= 0) {
    return;
  }

  local_clock(wall_clock(), &ci);
  format_offset(ci.offset_min, tz, sizeof tz);
  snprintf(final, sizeof final, "%s/%s", DAILY_ROOT, ci.date);
  snprintf(part, sizeof part, "%s.part", final);

  /* one snapshot per date: an existing one is never wiped or refreshed */
  if(!stat(final, &st) && S_ISDIR(st.st_mode)) {
    daily_prune(ci.date);
    return;
  }

  if(stat(BACKUP_ROOT, &st) || !S_ISDIR(st.st_mode)) {
    logq("Daily backup %s skipped: no live backup to copy yet", ci.date);
    return;
  }

  need = dir_size(BACKUP_ROOT) + SPACE_MARGIN;
  if(usb_free() < need) {
    snprintf(key, sizeof key, "daily-space:%s", ci.date);
    if(!seen_before(key)) {
      format_size(need, a, sizeof a);
      format_size(usb_free(), b, sizeof b);
      logq("Not enough space on USB for daily backup %s (need ~%s, free %s); "
           "skipping", ci.date, a, b);
      notify("Daily snapshot skipped: not enough USB space.");
    }
    return;
  }

  progress_begin(need - SPACE_MARGIN);
  backup_activity_begin();
  mkdir_p(DAILY_ROOT);
  remove_tree(part);                     /* leftover of an interrupted copy */

  /* copy into a .part folder and rename at the end, so an interrupted copy is
   * never mistaken for a finished snapshot */
  if(copy_tree(BACKUP_ROOT, part, &bytes) || (sync(), rename(part, final))) {
    progress_finish();
    remove_tree(part);
    snprintf(key, sizeof key, "daily-fail:%s", ci.date);
    if(!seen_before(key)) {
      logq("Failed to create daily backup %s", ci.date);
      notify("Daily snapshot could not be saved.");
    }
    return;
  }

  progress_finish();
  format_size(bytes, a, sizeof a);
  logq("Daily backup %s created (%s, console time %s)", ci.date, a, tz);
  notify("Daily snapshot saved.");
  daily_prune(ci.date);
}


/**
 * Time of the next backup pass: the regular interval, or just after the next
 * local midnight if that comes first (so the new day's snapshot is made
 * promptly).
 **/
static double
next_due(double started) {
  double due = started + g_interval;
  if(g_daily > 0) {
    clock_info_t ci;
    local_clock(wall_clock(), &ci);
    double midnight = now_seconds() + ci.to_midnight + 1;
    if(midnight < due) due = midnight;
  }
  /* Long passes never overlap or generate a catch-up burst. */
  double now = now_seconds();
  return due > now ? due : now + g_interval;
}


/**
 * Run one backup pass. Returns -1 if backup.conf could not be opened.
 **/
static int
run_cycle(int pending_only) {
  backup_stats_t stats;
  char line[PATH_MAX + 16];
  char size[32];
  pid_t pid = getpid();
  double start;
  FILE *f;

  /* escape the jail so that /data and /mnt/usb0 are reachable; done on every
   * cycle in case the state was reset while the console was suspended */
  kernel_set_proc_rootdir(pid, kernel_get_root_vnode());

  if(!(f = fopen(CONF_PATH, "r"))) {
    return -1;
  }

  progress_reset();
  index_load();
  g_pending_only = pending_only;
  g_cycle_deferred = 0;
  g_backup_busy = 0;
  if(!g_start_notified) {
    notify("Backup utility started");
    g_start_notified = 1;
  }
  memset(&stats, 0, sizeof stats);
  start = now_seconds();

  /* Settings are re-read from defaults on every cycle. */
  g_interval = DEF_INTERVAL;
  g_max_game = DEF_GAME_MB * BYTES_PER_MB;
  g_daily = 0;
  g_ps4saves = 0;
  g_ps5saves = 0;
  g_game_abort = 0;
  g_watch_game = 0;

  g_progress_planning = 1;
  while(fgets(line, sizeof line, f)) {
    backup_entry(line, &stats);
  }
  if(g_ps4saves || g_ps5saves) backup_saves(&stats);
  if(!g_pending_only && g_daily > 0) {
    clock_info_t ci;
    char final[PATH_MAX];
    struct stat st;
    local_clock(wall_clock(), &ci);
    snprintf(final, sizeof final, "%s/%s", DAILY_ROOT, ci.date);
    if((stat(final, &st) || !S_ISDIR(st.st_mode)) &&
       (progress_total || (!stat(BACKUP_ROOT, &st) && S_ISDIR(st.st_mode))))
      progress_plan(1);
  }
  g_progress_planning = 0;
  if(fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return -1;
  }
  memset(&stats, 0, sizeof stats);
  g_interval = DEF_INTERVAL;
  g_max_game = DEF_GAME_MB * BYTES_PER_MB;
  g_daily = g_ps4saves = g_ps5saves = 0;
  while(fgets(line, sizeof line, f)) {
    backup_entry(line, &stats);
  }

  fclose(f);

  pending_disable();
  if(g_pending_only) {
    for(pending_title_t *p = pending_titles; p; p = p->next)
      if(p->ready) p->retry_at = now_seconds() + g_interval;
  }
  if(g_ps4saves || g_ps5saves) {
    backup_saves(&stats);
  }

  /* Regular files have already been processed. A deferred save-image pass
   * must not prevent a snapshot of the available live backup. */
  index_save();
  if(!g_pending_only) daily_check();
  progress_stop();


  if(stats.failed) {
    /* announce only on the first failed cycle until recovery */
    if(!g_last_failed) {
      logq("Failed to backup (%u file(s) failed, %u copied)",
           stats.failed, stats.copied);
      notify("Backup incomplete (%u failed).", stats.failed);
    }
    g_last_failed = 1;
  } else if(stats.copied || (g_last_failed && !g_cycle_deferred)) {
    format_size(stats.bytes, size, sizeof size);
    logq("Successful backup (%s) done in %.1f seconds.", size,
         now_seconds() - start);
    notify("Backup complete (%s).", size);
    g_last_failed = 0;
  } else if(g_cycle_deferred) {
    logq("Backup pass finished; protected titles queued (%u files unchanged)", stats.unchanged);
  } else {
    logq("Backup up to date (%u files unchanged)", stats.unchanged);
  }

  return 0;
}


/**
 * Make sure only one instance runs. If another instance is listening on the
 * loopback lock port, ask it to quit (by connecting) and take over.
 * Returns the listening socket or -1 if the lock could not be set up.
 **/
static int
acquire_lock(void) {
  struct sockaddr_in addr;
  int one = 1;

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LOCK_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  for(int attempt = 0; attempt < 10; attempt++) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0) {
      return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    if(!bind(s, (struct sockaddr*)&addr, sizeof addr) && !listen(s, 1)) {
      return s;
    }
    close(s);

    if(errno != EADDRINUSE) {
      return -1;
    }

    /* someone holds the port: tell it to quit, then retry */
    if(attempt == 0) {
      logq("Replacing running backup instance");
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if(s >= 0) {
      connect(s, (struct sockaddr*)&addr, sizeof addr);
      close(s);
    }
    sleep(1);
  }

  return -1;
}


/**
 * Wait on monotonic interval deadlines, with short checks for closed queued
 * titles. A close retry does not reset the next regular backup deadline.
 **/
/* 0 = regular deadline, 1 = replacement requested, 2 = queued title closed. */
static int
wait_until(double due, int lock) {
  for(;;) {
    double remaining = due - now_seconds();
    if(remaining <= 0) return 0;
    int slice = pending_titles ? CLOSE_POLL_SEC : SLICE_SEC;
    if(pending_ready()) return 2;
    int ms = remaining > slice ? slice * 1000 : (int)(remaining * 1000) + 1;
    struct pollfd pfd = {lock, POLLIN, 0};
    if(poll(&pfd, lock >= 0 ? 1 : 0, ms) > 0 && (pfd.revents & POLLIN)) {
      int c = accept(lock, 0, 0);
      if(c >= 0) close(c);
      return 1;
    }
  }
}


int
main(void) {
  int missing = 0;
  int lock;
  double due, started;
#ifdef BACKUP_HOST_TEST
  int cycles = 1;
  const char *max_cycles = getenv("TEST_CYCLES");
#endif

  /* the loader's connection may be closed at any time; never die on SIGPIPE */
  signal(SIGPIPE, SIG_IGN);

  kernel_set_proc_rootdir(getpid(), kernel_get_root_vnode());
  lock = acquire_lock();
  if(lock < 0) {
    notify("Backup could not start: instance lock unavailable.");
    return EXIT_FAILURE;
  }

  if(!(g_buf = malloc(COPY_BUF_SZ))) {
    notify("Backup could not start: not enough memory.");
    return EXIT_FAILURE;
  }

  /* Initial backup immediately; subsequent regular deadlines use monotonic time. */
  started = now_seconds();
  if(run_cycle(0) < 0) {
    notify("backup.conf not found on USB.");
    return EXIT_FAILURE;
  }
  if(g_daily > 0) {
    /* shows the date and UTC offset the payload sees, to check them against
     * the console's clock */
    clock_info_t ci;
    char tz[24];

    local_clock(wall_clock(), &ci);
    format_offset(ci.offset_min, tz, sizeof tz);
    logq("Daily backups on: keeping newest %d (console date %s, %s)",
         g_daily, ci.date, tz);
    notify("Daily snapshots enabled (%d kept).", g_daily);
  }

  due = next_due(started);

  for(;;) {
#ifdef BACKUP_HOST_TEST
    if(max_cycles && cycles >= atoi(max_cycles)) {
      return EXIT_SUCCESS;
    }
#endif
    int wake = wait_until(due, lock);
    if(wake == 1) {
      notify("Previous backup instance stopped.");
      break;
    }

    started = now_seconds();
    if(run_cycle(wake == 2) < 0) {
      /* USB drive may still be mounting after rest mode: retry shortly and
       * only complain if it stays missing */
      if(++missing == MISSING_TRIES) {
        notify("backup.conf not found on USB.");
      }
      pending_retry_after = now_seconds() + SLICE_SEC * 3;
      due = pending_retry_after;
      continue;
    }
#ifdef BACKUP_HOST_TEST
    cycles++;
#endif

    if(missing >= MISSING_TRIES) {
      logq("backup.conf is back");
    }
    missing = 0;
    pending_retry_after = 0;

    /* g_interval may have been changed by the conf file */
    if(wake != 2) due = next_due(started);
  }

  return EXIT_SUCCESS;
}
