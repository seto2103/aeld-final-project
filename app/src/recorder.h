/**
 * @file recorder.h
 * @brief Recorder thread: saves a clip for each motion event, starting with the frames from
 *        before the motion that are still in the frame ring.
 */

#ifndef RECORDER_H
#define RECORDER_H

#include "frame_ring.h"
#include "motion.h"

struct recorder_config {
    const char *dir;                /* for example /data/recordings; must already exist */
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int pre_event_frames;  /* frames from before the motion at the start of a clip */
    unsigned int free_percent;      /* oldest clips are deleted to keep this much space free */
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

#endif /* RECORDER_H */
