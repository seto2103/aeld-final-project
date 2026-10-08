/**
 * @file frame_store.h
 * @brief Holds the newest captured JPEG frame, shared between the capture thread and readers.
 *
 * The capture thread is the only writer. Readers wait for a frame newer than the last one they
 * saw and receive their own copy, so the lock is only held while copying, never while a reader
 * sends the frame. A reader that falls behind simply gets the newest frame on its next wait,
 * skipping the ones in between, so it never slows down capture or other readers.
 */

#ifndef FRAME_STORE_H
#define FRAME_STORE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

struct frame_store {
    pthread_mutex_t lock;
    pthread_cond_t new_frame;   /* broadcast on every new frame and on shutdown */
    unsigned char *data;
    size_t len;
    size_t capacity;
    uint64_t seq;               /* 0 until the first frame arrives, then 1, 2, ... */
    int shut_down;
};

/** A reader's own copy of a frame. Zero-initialize before first use; the buffer is reused. */
struct frame_copy {
    unsigned char *data;
    size_t len;
    size_t capacity;
    uint64_t seq;
};

/** @return 0 on success, -1 on error (logged to syslog) */
int frame_store_init(struct frame_store *store);

/**
 * Copy a new frame into the store, increment the sequence number and wake all readers.
 * @return 0 on success, -1 if memory could not be allocated
 */
int frame_store_put(struct frame_store *store, const void *data, size_t len);

/**
 * Wait until the store holds a frame with a sequence number greater than after_seq, then copy
 * it into out. Passing after_seq 0 returns the newest frame without waiting, if there is one.
 * @return 0 when a frame was copied, 1 on timeout, -1 if the store was shut down or memory
 *         could not be allocated
 */
int frame_store_wait_newer(struct frame_store *store, uint64_t after_seq, struct frame_copy *out,
                           unsigned int timeout_ms);

/** Wake all waiting readers and make every later wait return -1. */
void frame_store_shutdown(struct frame_store *store);

/** Free the store. No thread may be using it. */
void frame_store_destroy(struct frame_store *store);

/**
 * Copy a frame into copy, growing its buffer if needed.
 * @return 0 on success, -1 if memory could not be allocated (logged)
 */
int frame_copy_set(struct frame_copy *copy, const void *data, size_t len, uint64_t seq);

void frame_copy_free(struct frame_copy *copy);

#endif /* FRAME_STORE_H */
