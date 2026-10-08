/**
 * @file frame_ring.h
 * @brief Ring buffer of the most recent frames, read in order by the recorder.
 *
 * The capture thread pushes every frame; the oldest frame is overwritten once the ring is full.
 * Unlike the frame store, which only keeps the newest frame, a reader can ask for any frame by
 * sequence number, so it can start a clip with frames from before the motion and does not miss
 * frames during a short stall, such as a slow SD card write.
 */

#ifndef FRAME_RING_H
#define FRAME_RING_H

#include <pthread.h>
#include <stdint.h>

#include "frame_store.h"

struct ring_slot {
    unsigned char *data;
    size_t len;
    size_t capacity;
};

struct frame_ring {
    pthread_mutex_t lock;
    pthread_cond_t new_frame;   /* broadcast on every new frame and on shutdown */
    struct ring_slot *slots;    /* frame with sequence number seq is in slots[seq % count] */
    unsigned int count;
    uint64_t newest;            /* sequence number of the newest frame, 0 until the first */
    int shut_down;
};

/**
 * @param count number of frames the ring holds
 * @return 0 on success, -1 on error (logged to syslog)
 */
int frame_ring_init(struct frame_ring *ring, unsigned int count);

/**
 * Copy a frame into the ring, overwriting the oldest one when full, and wake readers.
 * @return 0 on success, -1 if memory could not be allocated
 */
int frame_ring_push(struct frame_ring *ring, const void *data, size_t len);

/** @return sequence number of the newest frame, 0 if there is none yet */
uint64_t frame_ring_newest(struct frame_ring *ring);

/**
 * Copy the frame with sequence number seq into out, waiting for it if it has not been captured
 * yet. If it has already been overwritten, the oldest frame still in the ring is copied instead;
 * out->seq tells the caller which frame it got.
 * @return 0 when a frame was copied, 1 on timeout, -1 if the ring was shut down or memory could
 *         not be allocated
 */
int frame_ring_read(struct frame_ring *ring, uint64_t seq, struct frame_copy *out,
                    unsigned int timeout_ms);

/** Wake all waiting readers and make every later read return -1. */
void frame_ring_shutdown(struct frame_ring *ring);

/** Free the ring. No thread may be using it. */
void frame_ring_destroy(struct frame_ring *ring);

#endif /* FRAME_RING_H */
