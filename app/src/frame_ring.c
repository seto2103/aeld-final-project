/**
 * @file frame_ring.c
 * @brief Ring buffer of the most recent frames, read in order by the recorder.
 */

#include "frame_ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

int frame_ring_init(struct frame_ring *ring, unsigned int count)
{
    pthread_condattr_t attr;
    int rc;

    memset(ring, 0, sizeof(*ring));
    ring->slots = calloc(count, sizeof(*ring->slots));
    if (ring->slots == NULL) {
        syslog(LOG_ERR, "Out of memory for a %u frame ring", count);
        return -1;
    }
    ring->count = count;

    rc = pthread_mutex_init(&ring->lock, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_mutex_init failed: %s", strerror(rc));
        free(ring->slots);
        return -1;
    }

    /* Timed waits use the monotonic clock, so setting the wall clock can't affect them */
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    rc = pthread_cond_init(&ring->new_frame, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_cond_init failed: %s", strerror(rc));
        pthread_mutex_destroy(&ring->lock);
        free(ring->slots);
        return -1;
    }
    return 0;
}

int frame_ring_push(struct frame_ring *ring, const void *data, size_t len)
{
    struct ring_slot *slot;
    int ret = 0;

    pthread_mutex_lock(&ring->lock);
    slot = &ring->slots[(ring->newest + 1) % ring->count];
    if (len > slot->capacity) {
        /* Grow with headroom, since JPEG frame sizes vary with the scene */
        size_t capacity = len + len / 2;
        unsigned char *buf = realloc(slot->data, capacity);

        if (buf == NULL) {
            ret = -1;
        } else {
            slot->data = buf;
            slot->capacity = capacity;
        }
    }
    if (ret == 0) {
        memcpy(slot->data, data, len);
        slot->len = len;
        ring->newest++;
        pthread_cond_broadcast(&ring->new_frame);
    }
    pthread_mutex_unlock(&ring->lock);
    return ret;
}

uint64_t frame_ring_newest(struct frame_ring *ring)
{
    uint64_t newest;

    pthread_mutex_lock(&ring->lock);
    newest = ring->newest;
    pthread_mutex_unlock(&ring->lock);
    return newest;
}

int frame_ring_read(struct frame_ring *ring, uint64_t seq, struct frame_copy *out,
                    unsigned int timeout_ms)
{
    struct timespec deadline;
    uint64_t oldest;
    int ret = 0;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ring->lock);
    /* Loop, since condition variables can wake up without a new frame */
    while (!ring->shut_down && ring->newest < seq) {
        if (pthread_cond_timedwait(&ring->new_frame, &ring->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }

    if (ring->shut_down) {
        ret = -1;
    } else if (ring->newest < seq) {
        ret = 1;
    } else {
        /* Frames older than this have been overwritten */
        oldest = ring->newest >= ring->count ? ring->newest - ring->count + 1 : 1;
        if (seq < oldest) {
            seq = oldest;
        }
        if (frame_copy_set(out, ring->slots[seq % ring->count].data,
                           ring->slots[seq % ring->count].len, seq) == -1) {
            ret = -1;
        }
    }
    pthread_mutex_unlock(&ring->lock);
    return ret;
}

void frame_ring_shutdown(struct frame_ring *ring)
{
    pthread_mutex_lock(&ring->lock);
    ring->shut_down = 1;
    pthread_cond_broadcast(&ring->new_frame);
    pthread_mutex_unlock(&ring->lock);
}

void frame_ring_destroy(struct frame_ring *ring)
{
    unsigned int i;

    pthread_cond_destroy(&ring->new_frame);
    pthread_mutex_destroy(&ring->lock);
    for (i = 0; i < ring->count; i++) {
        free(ring->slots[i].data);
    }
    free(ring->slots);
    ring->slots = NULL;
}
