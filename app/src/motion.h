/**
 * @file motion.h
 * @brief Motion detection thread: compares downscaled grayscale frames with a slowly updated
 *        background and reports when motion starts and ends.
 *
 * The thread is another reader of the frame store. About 10 frames per second are decoded at
 * 1/8 size with libjpeg-turbo; the score is the percentage of pixels that differ from the
 * background by more than a threshold.
 */

#ifndef MOTION_H
#define MOTION_H

#include "frame_store.h"

#define MOTION_DEFAULT_PIXEL_THRESHOLD  25
#define MOTION_DEFAULT_TRIGGER_PERCENT  1.0
#define MOTION_DEFAULT_HOLDOFF_MS       3000

struct motion_config {
    unsigned int pixel_threshold;   /* brightness difference (0-255) that counts as changed */
    double trigger_percent;         /* changed pixels, as a percentage, that count as motion */
    unsigned int holdoff_ms;        /* motion ends after this long below the trigger */
};

struct motion;

/**
 * Start the motion detection thread.
 * @return the detector, or NULL on error (logged to syslog)
 */
struct motion *motion_start(const struct motion_config *cfg, struct frame_store *store);

/** 1 while motion is in progress, 0 otherwise. Safe to call from any thread. */
int motion_active(const struct motion *m);

/** Stop and join the thread and free the detector. Does nothing if m is NULL. */
void motion_stop(struct motion *m);

#endif /* MOTION_H */
