/**
 * @file motion.c
 * @brief Motion detection thread: compares downscaled grayscale frames with a slowly updated
 *        background and reports when motion starts and ends.
 *
 * Each analyzed frame is decoded with libjpeg-turbo DCT scaling to 1/8 size in grayscale, which
 * skips most of the decoding work (1280x720 becomes 160x90). Downscaling also averages out
 * sensor noise. Motion starts when the score is above the trigger for TRIGGER_FRAMES analyzed
 * frames in a row, and ends when it has stayed below for the hold-off time.
 */

#include "motion.h"
#include "thread_util.h"

#include <errno.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <jpeglib.h>

/* Analyze about 10 frames per second */
#define ANALYZE_INTERVAL_MS         100
/* How long to wait for a frame before checking whether to stop */
#define FRAME_WAIT_MS               500
#define SCALE_DENOM                 8
/* Analyzed frames in a row (about 0.5 s) above the trigger that start motion; filters out
 * brief changes such as a refocus or a flicker */
#define TRIGGER_FRAMES              5
/* Analyzed frames (about 2 s) used only to build the background while exposure settles */
#define WARMUP_FRAMES               20
/* A change this large while idle is a lighting change (auto exposure, lights), not motion */
#define LIGHTING_CHANGE_PERCENT     50.0
/* Share of each analyzed frame blended into the background (time constant about 2 s) */
#define BACKGROUND_WEIGHT           0.05f
#define SCORE_LOG_INTERVAL_SECONDS  10

struct motion {
    struct motion_config cfg;
    struct frame_store *store;
    pthread_t thread;
    atomic_int stop;
    atomic_int active;
    unsigned char *gray;            /* newest decoded frame, width x height */
    float *background;
    unsigned int width;
    unsigned int height;
};

/* libjpeg calls error_exit() on a fatal error and by default exits the process */
struct jpeg_error_jump {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_jump *err = (struct jpeg_error_jump *)cinfo->err;

    longjmp(err->jump, 1);
}

/* Silence libjpeg warnings such as "Corrupt JPEG data"; a bad frame is just skipped */
static void jpeg_emit_message(j_common_ptr cinfo, int msg_level)
{
    (void)cinfo;
    (void)msg_level;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds)
{
    struct timespec ts;

    if (seconds <= 0) {
        return;
    }
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/**
 * Decode a JPEG frame at 1/8 size in grayscale into m->gray. The buffers are allocated on the
 * first frame; later frames must have the same size.
 * @return 0 on success, -1 if the frame could not be decoded
 */
static int decode_gray(struct motion *m, const unsigned char *data, size_t len)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_jump jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    jerr.pub.emit_message = jpeg_emit_message;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);

    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.scale_num = 1;
    cinfo.scale_denom = SCALE_DENOM;
    cinfo.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&cinfo);

    if (m->gray == NULL) {
        size_t pixels = (size_t)cinfo.output_width * cinfo.output_height;

        m->gray = malloc(pixels);
        m->background = malloc(pixels * sizeof(*m->background));
        if (m->gray == NULL || m->background == NULL) {
            syslog(LOG_ERR, "Out of memory for motion detection buffers");
            free(m->gray);
            free(m->background);
            m->gray = NULL;
            m->background = NULL;
            jpeg_destroy_decompress(&cinfo);
            return -1;
        }
        m->width = cinfo.output_width;
        m->height = cinfo.output_height;
        syslog(LOG_INFO, "Motion detection analyzing %ux%u grayscale", m->width, m->height);
    } else if (cinfo.output_width != m->width || cinfo.output_height != m->height) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = m->gray + (size_t)cinfo.output_scanline * m->width;

        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

static void background_reset(struct motion *m)
{
    size_t n = (size_t)m->width * m->height;
    size_t i;

    for (i = 0; i < n; i++) {
        m->background[i] = m->gray[i];
    }
}

static void background_update(struct motion *m)
{
    size_t n = (size_t)m->width * m->height;
    size_t i;

    for (i = 0; i < n; i++) {
        m->background[i] += BACKGROUND_WEIGHT * ((float)m->gray[i] - m->background[i]);
    }
}

/** @return percentage of pixels that differ from the background by more than the threshold */
static double motion_score(const struct motion *m)
{
    size_t n = (size_t)m->width * m->height;
    float threshold = (float)m->cfg.pixel_threshold;
    size_t changed = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        float diff = (float)m->gray[i] - m->background[i];

        if (diff > threshold || diff < -threshold) {
            changed++;
        }
    }
    return 100.0 * (double)changed / (double)n;
}

static void *motion_thread(void *arg)
{
    struct motion *m = arg;
    struct frame_copy frame = { 0 };
    uint64_t last_seq = 0;
    unsigned long analyzed = 0;
    unsigned int frames_above = 0;
    double next_due = monotonic_seconds();
    double event_start = 0;
    double last_above = 0;
    double peak = 0;
    /* For the periodic score log, used to tune the thresholds */
    double log_start = next_due;
    double log_max = 0;
    double log_decode_seconds = 0;
    unsigned long log_frames = 0;
    unsigned long decode_errors = 0;

    while (!atomic_load(&m->stop)) {
        double now;
        double decode_start;
        double score;
        int ret;

        sleep_seconds(next_due - monotonic_seconds());
        ret = frame_store_wait_newer(m->store, last_seq, &frame, FRAME_WAIT_MS);
        if (ret == 1) {
            continue;
        }
        if (ret == -1) {
            break;
        }
        last_seq = frame.seq;

        decode_start = monotonic_seconds();
        next_due = decode_start + ANALYZE_INTERVAL_MS / 1000.0;
        if (decode_gray(m, frame.data, frame.len) == -1) {
            decode_errors++;
            continue;
        }
        now = monotonic_seconds();
        log_decode_seconds += now - decode_start;
        log_frames++;

        /* Build the background while auto exposure settles */
        analyzed++;
        if (analyzed <= WARMUP_FRAMES) {
            background_reset(m);
            if (analyzed == WARMUP_FRAMES) {
                syslog(LOG_INFO, "Motion detection ready (trigger %.2f%% of pixels changed by "
                       "more than %u, hold-off %u ms)", m->cfg.trigger_percent,
                       m->cfg.pixel_threshold, m->cfg.holdoff_ms);
            }
            continue;
        }

        score = motion_score(m);
        if (score > log_max) {
            log_max = score;
        }

        if (!atomic_load(&m->active) && score >= LIGHTING_CHANGE_PERCENT) {
            syslog(LOG_INFO, "Lighting change (%.1f%% of pixels changed), background reset",
                   score);
            background_reset(m);
            frames_above = 0;
            continue;
        }

        if (!atomic_load(&m->active)) {
            frames_above = score >= m->cfg.trigger_percent ? frames_above + 1 : 0;
            if (frames_above >= TRIGGER_FRAMES) {
                atomic_store(&m->active, 1);
                event_start = now;
                last_above = now;
                peak = score;
                syslog(LOG_INFO, "Motion started (score %.2f%%)", score);
            }
        } else {
            if (score >= m->cfg.trigger_percent) {
                last_above = now;
            }
            if (score > peak) {
                peak = score;
            }
            if ((now - last_above) * 1000.0 >= m->cfg.holdoff_ms) {
                atomic_store(&m->active, 0);
                frames_above = 0;
                syslog(LOG_INFO, "Motion ended after %.1f s (peak score %.2f%%)",
                       now - event_start, peak);
            }
        }
        background_update(m);

        if (now - log_start >= SCORE_LOG_INTERVAL_SECONDS) {
            syslog(LOG_DEBUG, "Motion score max %.2f%% over %.0f s, %lu frames analyzed, "
                   "decode %.1f ms average, %lu decode errors", log_max, now - log_start,
                   log_frames, log_frames ? log_decode_seconds * 1000.0 / log_frames : 0.0,
                   decode_errors);
            log_start = now;
            log_max = 0;
            log_decode_seconds = 0;
            log_frames = 0;
            decode_errors = 0;
        }
    }

    if (atomic_load(&m->active)) {
        atomic_store(&m->active, 0);
        syslog(LOG_INFO, "Motion ended: stopping");
    }
    frame_copy_free(&frame);
    return NULL;
}

struct motion *motion_start(const struct motion_config *cfg, struct frame_store *store)
{
    struct motion *m = calloc(1, sizeof(*m));
    int rc;

    if (m == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    m->cfg = *cfg;
    m->store = store;
    atomic_init(&m->stop, 0);
    atomic_init(&m->active, 0);

    rc = thread_create_signals_blocked(&m->thread, motion_thread, m);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_create for motion detection failed: %s", strerror(rc));
        free(m);
        return NULL;
    }
    return m;
}

int motion_active(const struct motion *m)
{
    return atomic_load(&m->active);
}

void motion_stop(struct motion *m)
{
    if (m == NULL) {
        return;
    }
    atomic_store(&m->stop, 1);
    pthread_join(m->thread, NULL);
    free(m->gray);
    free(m->background);
    free(m);
}
