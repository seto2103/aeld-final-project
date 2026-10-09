/**
 * @file recorder.h
 * @brief Recorder thread: saves a clip for each motion event, starting with the frames from
 *        before the motion that are still in the frame ring.
 */

#ifndef RECORDER_H
#define RECORDER_H

#include "frame_ring.h"
#include "led.h"
#include "motion.h"

struct recorder_config {
    const char *dir;                /* for example /data/recordings; must already exist */
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int pre_event_frames;  /* frames from before the motion at the start of a clip */
    unsigned int free_percent;      /* oldest clips are deleted to keep this much space free */
    struct led *led;                /* blinks while a clip is being written; may be NULL */
};

struct recorder;

/**
 * Start the recorder thread.
 * @return the recorder, or NULL on error (logged to syslog)
 */
struct recorder *recorder_start(const struct recorder_config *cfg, struct frame_ring *ring,
                                const struct motion *motion);

/**
 * Stop and join the thread, closing a clip that is being written, and free the recorder.
 * Does nothing if r is NULL.
 */
void recorder_stop(struct recorder *r);

/** 1 while a clip is being written, 0 otherwise or if r is NULL. Safe to call from any thread. */
int recorder_recording(const struct recorder *r);

/**
 * Count the finished clips in the recordings directory and read its free space. Reads the
 * directory, so call it every few seconds rather than for every frame. Safe to call from any
 * thread.
 * @return 0 on success, -1 if r is NULL or the directory can't be read
 */
int recorder_storage(const struct recorder *r, unsigned int *clips,
                     unsigned long long *free_bytes);

#endif /* RECORDER_H */
