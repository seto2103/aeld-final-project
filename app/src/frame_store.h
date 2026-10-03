/**
 * @file frame_store.h
 * @brief Holds the newest captured JPEG frame.
 *
 * Single-threaded for now: the capture loop copies each new frame in and readers in the same
 * loop use it directly. Locking for multiple threads is added in the multithreaded version.
 */

#ifndef FRAME_STORE_H
#define FRAME_STORE_H

#include <stddef.h>
#include <stdint.h>

struct frame_store {
    unsigned char *data;
    size_t len;
    size_t capacity;
    uint64_t seq;           /* 0 until the first frame arrives, then 1, 2, ... */
};

void frame_store_init(struct frame_store *store);

/**
 * Copy a new frame into the store and increment the sequence number.
 * @return 0 on success, -1 if memory could not be allocated
 */
int frame_store_put(struct frame_store *store, const void *data, size_t len);

void frame_store_free(struct frame_store *store);

#endif /* FRAME_STORE_H */
