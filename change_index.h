/* Persistent metadata cache. No file contents are read to check for changes.
 * A missing/invalid cache causes a safe recopy, never a content-hash shortcut. */
#ifndef CHANGE_INDEX_H
#define CHANGE_INDEX_H
#define INDEX_PATH BACKUP_ROOT "/.ps5save-index"
#define INDEX_LIMIT 100000
#define INDEX_BUCKETS 4096

typedef struct file_signature {
  int64_t size, mtime, mtime_ns, ctime, ctime_ns;
  uint64_t inode, device;
} file_signature_t;

typedef struct change_record {
  struct change_record *next;
  char *path;
  file_signature_t source, target;
} change_record_t;
static change_record_t *change_index[INDEX_BUCKETS];
static unsigned index_count;
static int index_dirty;

static file_signature_t
file_signature(const struct stat *st) {
  file_signature_t sig;
  memset(&sig, 0, sizeof sig);
  sig.size = st->st_size;
  sig.mtime = st->st_mtimespec.tv_sec;
  sig.mtime_ns = st->st_mtimespec.tv_nsec;
  sig.ctime = st->st_ctimespec.tv_sec;
  sig.ctime_ns = st->st_ctimespec.tv_nsec;
  sig.inode = st->st_ino;
  sig.device = st->st_dev;
  return sig;
}

/* Fields that survive reboots, rest mode and USB remounts.
 * Not compared across passes:
 *  - device: st_dev is handed out at mount time, so it can differ after a
 *    reboot (internal) or after the USB drive is mounted again (rest mode).
 *  - ctime: also moves on metadata-only changes (chmod, chown, link count).
 * Size + mtime (with nanoseconds) + inode still catch every content write
 * and every replace-by-rename on the console's UFS volumes. */
static int
sig_same_content(const file_signature_t *a, const file_signature_t *b) {
  return a->size == b->size && a->mtime == b->mtime &&
         a->mtime_ns == b->mtime_ns && a->inode == b->inode;
}

/* Diagnostics, logged once per pass:
 *  drift  - fields that changed although the contents did not; the old exact
 *           comparison would have recopied these files.
 *  recopy - why a file or save image is being copied again. */
static struct {
  unsigned src_dev, src_ctime, dst_dev, dst_ino, dst_mtime, dst_ctime;
  unsigned ctime_named;
} drift;
static struct {
  unsigned no_record, src_content, src_ino, dst_missing, dst_size;
} recopy;

/* Returns non-zero if any ignored field changed, so the caller can refresh
 * its stored metadata (otherwise the same drift is reported every pass). */
static int
drift_note(const char *what, const file_signature_t *old_src,
           const file_signature_t *src, const file_signature_t *old_dst,
           const file_signature_t *dst) {
  int any = 0;
  if(old_src->device != src->device) {drift.src_dev++; any = 1;}
  if(old_src->ctime != src->ctime || old_src->ctime_ns != src->ctime_ns) {
    drift.src_ctime++; any = 1;
    /* Size, mtime and inode are unchanged, so this is treated as a metadata
     * update. Name a few so a missed save could be spotted in the log. */
    if(drift.ctime_named++ < 5)
      logq("Only ctime changed, treated as unchanged: %s", what);
  }
  if(!old_dst || !dst) return any;
  if(old_dst->device != dst->device) {drift.dst_dev++; any = 1;}
  if(old_dst->inode != dst->inode) {drift.dst_ino++; any = 1;}
  if(old_dst->mtime != dst->mtime || old_dst->mtime_ns != dst->mtime_ns) {drift.dst_mtime++; any = 1;}
  if(old_dst->ctime != dst->ctime || old_dst->ctime_ns != dst->ctime_ns) {drift.dst_ctime++; any = 1;}
  return any;
}

static void
recopy_reason(const file_signature_t *old_src, const file_signature_t *src) {
  if(old_src->size != src->size || old_src->mtime != src->mtime ||
     old_src->mtime_ns != src->mtime_ns) recopy.src_content++;
  else recopy.src_ino++;
}

static void
drift_report(void) {
  if(drift.src_dev | drift.src_ctime | drift.dst_dev | drift.dst_ino |
     drift.dst_mtime | drift.dst_ctime)
    logq("Metadata drift ignored: src dev %u, src ctime %u, usb dev %u, "
         "usb ino %u, usb mtime %u, usb ctime %u", drift.src_dev, drift.src_ctime,
         drift.dst_dev, drift.dst_ino, drift.dst_mtime, drift.dst_ctime);
  if(recopy.no_record | recopy.src_content | recopy.src_ino |
     recopy.dst_missing | recopy.dst_size)
    logq("Recopy reasons: new %u, src size/mtime %u, src inode %u, "
         "usb missing %u, usb size %u", recopy.no_record, recopy.src_content,
         recopy.src_ino, recopy.dst_missing, recopy.dst_size);
  memset(&drift, 0, sizeof drift);
  memset(&recopy, 0, sizeof recopy);
}

/* Exact comparison, only used within one pass ("changed while copying"). */
static int
source_same(const struct stat *a, const struct stat *b) {
  file_signature_t sa = file_signature(a), sb = file_signature(b);
  return !memcmp(&sa, &sb, sizeof sa);
}

static unsigned
index_bucket(const char *path) {
  uint64_t h = 14695981039346656037ULL;
  for(const unsigned char *p = (const unsigned char *)path; *p; p++)
    h = (h ^ *p) * 1099511628211ULL;
  return h % INDEX_BUCKETS;
}

static change_record_t *
index_record(const char *path, int create) {
  unsigned bucket = index_bucket(path);
  for(change_record_t *r = change_index[bucket]; r; r = r->next)
    if(!strcmp(r->path, path)) return r;
  if(!create || index_count == INDEX_LIMIT) return NULL;
  change_record_t *r = calloc(1, sizeof *r);
  if(!r) return NULL;
  r->path = strdup(path);
  if(!r->path) {free(r); return NULL;}
  r->next = change_index[bucket]; change_index[bucket] = r; index_count++;
  return r;
}

static void
index_clear(void) {
  for(unsigned i = 0; i < INDEX_BUCKETS; i++) {
    while(change_index[i]) {
      change_record_t *r = change_index[i];
      change_index[i] = r->next; free(r->path); free(r);
    }
  }
  index_count = 0; index_dirty = 0;
}

static int
file_current(const char *dst, const struct stat *src) {
  change_record_t *r = index_record(dst, 0);
  struct stat target;
  int counting = !g_progress_planning;  /* planning pass: no side effects */
  if(!r) {if(counting) recopy.no_record++; return 0;}
  if(stat(dst, &target) || !S_ISREG(target.st_mode)) {
    if(counting) recopy.dst_missing++;
    return 0;
  }
  file_signature_t s = file_signature(src), t = file_signature(&target);
  /* The console file is the source of truth. The USB copy only has to exist
   * and be complete: exFAT inode, ctime and mtime are not stable across
   * remounts, so they are not compared. */
  if(!sig_same_content(&s, &r->source)) {
    if(counting) recopy_reason(&r->source, &s);
    return 0;
  }
  if(target.st_size != src->st_size) {
    if(counting) recopy.dst_size++;
    return 0;
  }
  if(counting && drift_note(dst, &r->source, &s, &r->target, &t)) {
    r->source = s; r->target = t; index_dirty = 1;
  }
  return 1;
}

static void
index_update(const char *dst, const struct stat *src) {
  struct stat target;
  if(stat(dst, &target)) return;
  change_record_t *r = index_record(dst, 1);
  if(!r) {logq("Change cache full/unavailable; file will be checked again: %s", dst); return;}
  r->source = file_signature(src); r->target = file_signature(&target);
  index_dirty = 1;
}

static void
index_load(void) {
  index_clear();
  FILE *f = fopen(INDEX_PATH, "rb");
  if(!f) return;
  char magic[8];
  int valid = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PS5IDX2\n", 8);
  while(valid) {
    uint32_t len;
    size_t n = fread(&len, 1, sizeof len, f);
    if(!n && !ferror(f)) break;
    if(n != sizeof len || !len || len >= PATH_MAX || index_count == INDEX_LIMIT) {valid = 0; break;}
    char path[PATH_MAX];
    file_signature_t source, target;
    if(fread(path, 1, len, f) != len ||
       fread(&source, 1, sizeof source, f) != sizeof source ||
       fread(&target, 1, sizeof target, f) != sizeof target) {valid = 0; break;}
    path[len] = 0;
    if(strlen(path) != len || strncmp(path, BACKUP_ROOT "/", strlen(BACKUP_ROOT) + 1)) {valid = 0; break;}
    change_record_t *r = index_record(path, 1);
    if(!r) {valid = 0; break;}
    r->source = source; r->target = target;
  }
  fclose(f);
  if(!valid) {index_clear(); logq("Change cache invalid; using safe recopy");}
}

static void
index_save(void) {
  drift_report();
  if(!index_dirty) return;
  FILE *f = fopen(INDEX_PATH ".part", "wb");
  if(!f) {logq("Cannot save change cache; next pass may recopy files"); return;}
  int ok = fwrite("PS5IDX2\n", 1, 8, f) == 8;
  for(unsigned i = 0; i < INDEX_BUCKETS && ok; i++) {
    for(change_record_t *r = change_index[i]; r && ok; r = r->next) {
      uint32_t len = (uint32_t)strlen(r->path);
      ok = fwrite(&len, 1, sizeof len, f) == sizeof len &&
           fwrite(r->path, 1, len, f) == len &&
           fwrite(&r->source, 1, sizeof r->source, f) == sizeof r->source &&
           fwrite(&r->target, 1, sizeof r->target, f) == sizeof r->target;
    }
  }
  if(fflush(f) || fsync(fileno(f))) ok = 0;
  if(fclose(f)) ok = 0;
  if(!ok || rename(INDEX_PATH ".part", INDEX_PATH)) {
    unlink(INDEX_PATH ".part"); logq("Cannot commit change cache");
  } else index_dirty = 0;
}
#endif
