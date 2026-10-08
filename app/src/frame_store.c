/**
 * @file frame_store.c
 * @brief Holds the newest captured JPEG frame, shared between the capture thread and readers.
 */

#include "frame_store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

/** Grow buf to hold at least len bytes, with headroom since JPEG frame sizes vary. */
static int reserve(unsigned char **buf, size_t *capacity, size_t len)
{
    size_t new_capacity;
    unsigned char *p;

    if (len <= *capacity) {
        return 0;
    }
    new_capacity = len + len / 2;
    p = realloc(*buf, new_capacity);
    if (p == NULL) {
        return -1;
    }
    *buf = p;
    *capacity = new_capacity;
    return 0;
}

int frame_store_init(struct frame_store *store)
{
    pthread_condattr_t attr;
    int rc;

    memset(store, 0, sizeof(*store));

    rc = pthread_mutex_init(&store->lock, NULL);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_mutex_init failed: %s", strerror(rc));
        return -1;
    }

    /* Timed waits use the monotonic clock, so setting the wall clock can't affect them */
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    rc = pthread_cond_init(&store->new_frame, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        syslog(LOG_ERR, "pthread_cond_init failed: %s", strerror(rc));
        pthread_mutex_destroy(&store->lock);
        return -1;
    }
    return 0;
}

int frame_store_put(struct frame_store *store, const void *data, size_t len)
{
    int ret = 0;

    pthread_mutex_lock(&store->lock);
    if (reserve(&store->data, &store->capacity, len) == -1) {
        ret = -1;
    } else {
        memcpy(store->data, data, len);
        store->len = len;
        store->seq++;
        pthread_cond_broadcast(&store->new_frame);
    }
    pthread_mutex_unlock(&store->lock);
    return ret;
}

int frame_store_wait_newer(struct frame_store *store, uint64_t after_seq, struct frame_copy *out,
                           unsigned int timeout_ms)
{
    struct timespec deadline;
    int ret = 0;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&store->lock);
    /* Loop, since condition variables can wake up without a new frame */
    while (!store->shut_down && store->seq <= after_seq) {
        if (pthread_cond_timedwait(&store->new_frame, &store->lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }

    if (store->shut_down) {
        ret = -1;
    } else if (store->seq <= after_seq) {
        ret = 1;
    } else if (frame_copy_set(out, store->data, store->len, store->seq) == -1) {
        ret = -1;
    }
    pthread_mutex_unlock(&store->lock);
    return ret;
}

void frame_store_shutdown(struct frame_store *store)
{
    pthread_mutex_lock(&store->lock);
    store->shut_down = 1;
    pthread_cond_broadcast(&store->new_frame);
    pthread_mutex_unlock(&store->lock);
}

void frame_store_destroy(struct frame_store *store)
{
    pthread_cond_destroy(&store->new_frame);
    pthread_mutex_destroy(&store->lock);
    free(store->data);
    store->data = NULL;
}

int frame_copy_set(struct frame_copy *copy, const void *data, size_t len, uint64_t seq)
{
    if (reserve(&copy->data, &copy->capacity, len) == -1) {
        syslog(LOG_ERR, "Out of memory copying a %zu byte frame", len);
        return -1;
    }
    memcpy(copy->data, data, len);
    copy->len = len;
    copy->seq = seq;
    return 0;
}

void frame_copy_free(struct frame_copy *copy)
{
    free(copy->data);
    memset(copy, 0, sizeof(*copy));
}
