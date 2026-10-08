/**
 * @file recorder.c
 * @brief Recorder thread: saves a clip for each motion event, starting with the frames from
 *        before the motion that are still in the frame ring.
 *
 * When motion starts, the recorder goes back pre_event_frames in the ring and writes every frame
 * from there, then keeps writing live frames until motion has ended (the motion detector's
 * hold-off is already included). A clip is written as NAME.avi.part and renamed to NAME.avi
 * once it is complete, so a partial file is never mistaken for a finished clip. A clip that
 * reaches the AVI size limit is closed and a new one continues the same event.
 *
 * Rotation: before a clip starts, and once a second while it is written, the oldest clips are
 * deleted until free space is back above the limit, so the newest recordings are always kept.
 * The clip being written is never deleted. If space can't be freed, the clip is closed early so
 * it stays playable, and recording stops until there is room again.
 */

#include "recorder.h"
#include "avi_writer.h"
#include "thread_util.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* How often to check for motion while idle */
#define IDLE_POLL_MS            100
#define FRAME_WAIT_MS           500
/* Free space kept on the recordings filesystem is never less than this, whatever the percentage */
#define MIN_FREE_BYTES          (200ull * 1000 * 1000)
/* Different names for clips that start in the same second, for example after a reboot resets
 * the clock */
#define MAX_NAME_ATTEMPTS       100

struct recorder {
    struct recorder_config cfg;
    struct frame_ring *ring;
    const struct motion *motion;
    pthread_t thread;
    atomic_int stop;
};

/** A clip being written, and the names it is written under */
struct clip {
    struct avi_writer *avi;
    char part_path[PATH_MAX];       /* NAME.avi.part while writing */
    char path[PATH_MAX - sizeof(".part")];  /* NAME.avi once complete; leaves room for .part */
    unsigned long dropped;          /* frames overwritten in the ring before they were written */
};

static void sleep_ms(unsigned int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

/**
 * Read the free space of the recordings filesystem and the amount that must be kept free.
 * @return 0 on success, -1 on error (logged)
 */
static int free_space(const struct recorder *r, unsigned long long *free_bytes,
                      unsigned long long *limit)
{
    struct statvfs vfs;

    if (statvfs(r->cfg.dir, &vfs) == -1) {
        syslog(LOG_WARNING, "statvfs %s failed: %m", r->cfg.dir);
        return -1;
    }
    *free_bytes = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    *limit = (unsigned long long)vfs.f_blocks * vfs.f_frsize / 100 * r->cfg.free_percent;
    if (*limit < MIN_FREE_BYTES) {
        *limit = MIN_FREE_BYTES;
    }
    return 0;
}

/** Finished clips (NAME.avi) and ones left unfinished by a crash (NAME.avi.part) */
static int is_clip_name(const char *name)
{
    size_t len = strlen(name);

    return (len > 4 && strcmp(name + len - 4, ".avi") == 0) ||
           (len > 9 && strcmp(name + len - 9, ".avi.part") == 0);
}

/**
 * Delete the oldest clip in the recordings directory, by modification time.
 * @param keep path of the clip being written, which is never deleted, or NULL
 * @return 0 if a clip was deleted, -1 if there was none to delete
 */
static int delete_oldest(const struct recorder *r, const char *keep)
{
    char oldest[PATH_MAX] = "";
    time_t oldest_mtime = 0;
    off_t oldest_size = 0;
    struct dirent *entry;
    DIR *dir;

    dir = opendir(r->cfg.dir);
    if (dir == NULL) {
        syslog(LOG_WARNING, "Cannot open %s: %m", r->cfg.dir);
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct stat st;

        if (!is_clip_name(entry->d_name)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", r->cfg.dir, entry->d_name);
        if ((keep != NULL && strcmp(path, keep) == 0) || stat(path, &st) == -1 ||
            !S_ISREG(st.st_mode)) {
            continue;
        }
        /* Names sort by start time, so they break ties within the same second */
        if (oldest[0] == '\0' || st.st_mtime < oldest_mtime ||
            (st.st_mtime == oldest_mtime && strcmp(path, oldest) < 0)) {
            memcpy(oldest, path, sizeof(oldest));
            oldest_mtime = st.st_mtime;
            oldest_size = st.st_size;
        }
    }
    closedir(dir);

    if (oldest[0] == '\0') {
        return -1;
    }
    if (unlink(oldest) == -1) {
        syslog(LOG_WARNING, "Deleting %s failed: %m", oldest);
        return -1;
    }
    syslog(LOG_INFO, "Deleted oldest clip %s (%.1f MB) to free space", oldest,
           (double)oldest_size / 1e6);
    return 0;
}

/**
 * Delete the oldest clips until the free space is above the limit.
 * @param keep path of the clip being written, which is never deleted, or NULL
 * @return 0 if there is enough free space, -1 if not (logged)
 */
static int make_space(const struct recorder *r, const char *keep)
{
    for (;;) {
        unsigned long long free_bytes;
        unsigned long long limit;

        if (free_space(r, &free_bytes, &limit) == -1) {
            return -1;
        }
        if (free_bytes >= limit) {
            return 0;
        }
        if (delete_oldest(r, keep) == -1) {
            syslog(LOG_WARNING, "Only %llu MB free in %s (%llu MB needed) and no clip left to "
                   "delete", free_bytes / 1000000, r->cfg.dir, limit / 1000000);
            return -1;
        }
    }
}

/**
 * Check that the recordings directory exists and make room for a clip.
 * @return 0 if recording can start, -1 if not (logged)
 */
static int check_dir(const struct recorder *r)
{
    struct stat st;

    if (stat(r->cfg.dir, &st) == -1 || !S_ISDIR(st.st_mode)) {
        syslog(LOG_WARNING, "Recording disabled: %s does not exist", r->cfg.dir);
        return -1;
    }
    if (make_space(r, NULL) == -1) {
        syslog(LOG_WARNING, "Recording disabled: not enough free space in %s", r->cfg.dir);
        return -1;
    }
    return 0;
}

/**
 * Create a new clip named by the time its first frame was captured.
 * @param start_time wall clock time of the first frame
 * @return 0 on success, -1 on error (logged)
 */
static int clip_open(const struct recorder *r, struct clip *clip, time_t start_time)
{
    char name[64];
    struct tm tm;
    int attempt;

    gmtime_r(&start_time, &tm);
    strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", &tm);

    for (attempt = 0; attempt < MAX_NAME_ATTEMPTS; attempt++) {
        struct stat st;

        if (attempt == 0) {
            snprintf(clip->path, sizeof(clip->path), "%s/%s.avi", r->cfg.dir, name);
        } else {
            snprintf(clip->path, sizeof(clip->path), "%s/%s_%d.avi", r->cfg.dir, name, attempt);
        }
        snprintf(clip->part_path, sizeof(clip->part_path), "%s.part", clip->path);
        if (stat(clip->path, &st) == 0) {
            continue;
        }

        /* avi_open() fails without logging if the .part file exists; try the next name */
        clip->avi = avi_open(clip->part_path, r->cfg.width, r->cfg.height, r->cfg.fps);
        if (clip->avi != NULL) {
            clip->dropped = 0;
            return 0;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    syslog(LOG_ERR, "No free clip name for %s in %s", name, r->cfg.dir);
    return -1;
}

/** Finish the clip and rename it to its final name. */
static void clip_close(const struct recorder *r, struct clip *clip)
{
    unsigned long frames = avi_frames(clip->avi);
    double megabytes = (double)avi_size(clip->avi) / 1e6;

    if (avi_close(clip->avi) == -1) {
        syslog(LOG_ERR, "Clip %s is incomplete", clip->part_path);
    } else if (rename(clip->part_path, clip->path) == -1) {
        syslog(LOG_ERR, "Renaming %s failed: %m", clip->part_path);
    } else {
        syslog(LOG_INFO, "Saved clip %s: %lu frames, %.1f s, %.1f MB, %lu frames dropped",
               clip->path, frames, (double)frames / r->cfg.fps, megabytes, clip->dropped);
    }
    clip->avi = NULL;
}

/**
 * Record one motion event, from the pre-event frames until motion has ended.
 * @param newest newest frame in the ring when motion started; the clip starts pre_event_frames
 *        before it, even if making space took a while
 */
static void record_event(struct recorder *r, uint64_t newest)
{
    struct frame_copy frame = { 0 };
    struct clip clip = { 0 };
    uint64_t seq = newest > r->cfg.pre_event_frames ? newest - r->cfg.pre_event_frames + 1 : 1;
    uint64_t end_seq = 0;           /* last frame to write once motion has ended, 0 until then */
    /* Frames captured since the first frame of the clip, to date the clip by its first frame */
    time_t start_time = time(NULL) - (time_t)((frame_ring_newest(r->ring) - seq + 1) / r->cfg.fps);

    if (clip_open(r, &clip, start_time) == -1) {
        return;
    }
    syslog(LOG_INFO, "Recording %s, starting %llu frames before the motion", clip.path,
           (unsigned long long)(newest - seq + 1));

    while (!atomic_load(&r->stop)) {
        int ret;

        if (motion_active(r->motion)) {
            end_seq = 0;
        } else if (end_seq == 0) {
            end_seq = frame_ring_newest(r->ring);
        }
        if (end_seq != 0 && seq > end_seq) {
            break;
        }

        ret = frame_ring_read(r->ring, seq, &frame, FRAME_WAIT_MS);
        if (ret == 1) {
            continue;
        }
        if (ret == -1) {
            break;
        }
        if (frame.seq > seq) {
            clip.dropped += frame.seq - seq;
        }
        seq = frame.seq + 1;

        if (avi_size(clip.avi) + frame.len + 32 > AVI_MAX_SIZE) {
            syslog(LOG_INFO, "Clip %s reached the size limit, continuing in a new clip",
                   clip.path);
            clip_close(r, &clip);
            if (clip_open(r, &clip, time(NULL)) == -1) {
                break;
            }
        }
        if (avi_write_frame(clip.avi, frame.data, frame.len) == -1) {
            break;
        }
        /* Once a second, delete old clips if the partition is filling up */
        if (avi_frames(clip.avi) % r->cfg.fps == 0 && make_space(r, clip.part_path) == -1) {
            syslog(LOG_WARNING, "Closing %s early: out of space", clip.path);
            break;
        }
    }

    if (clip.avi != NULL) {
        clip_close(r, &clip);
    }
    frame_copy_free(&frame);
}

static void *recorder_thread(void *arg)
{
    struct recorder *r = arg;
    uint64_t newest;

    while (!atomic_load(&r->stop)) {
        if (!motion_active(r->motion)) {
            sleep_ms(IDLE_POLL_MS);
            continue;
        }

        /* Note where the motion started before deleting old clips, which can take a while */
        newest = frame_ring_newest(r->ring);
        if (check_dir(r) == 0) {
            record_event(r, newest);
        }
        /* After an error, or if recording is disabled, don't retry until the next event */
        while (!atomic_load(&r->stop) && motion_active(r->motion)) {
            sleep_ms(IDLE_POLL_MS);
        }
    }
    return NULL;
}

struct recorder *recorder_start(const struct recorder_config *cfg, struct frame_ring *ring,
                                const struct motion *motion)
{
    struct recorder *r = calloc(1, sizeof(*r));
    int rc;

    if (r == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    r->cfg = *cfg;
    r->ring = ring;
    r->motion = motion;
    atomic_init(&r->stop, 0);

    rc = thread_create_signals_blocked(&r->thread, recorder_thread, r);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create for the recorder failed: %s", strerror(rc));
        free(r);
        return NULL;
    }
    syslog(LOG_INFO, "Recording motion to %s with %u frames before each event", cfg->dir,
           cfg->pre_event_frames);
    return r;
}

void recorder_stop(struct recorder *r)
{
    if (r == NULL) {
        return;
    }
    atomic_store(&r->stop, 1);
    pthread_join(r->thread, NULL);
    free(r);
}
