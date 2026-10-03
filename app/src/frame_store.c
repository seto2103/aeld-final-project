/**
 * @file frame_store.c
 * @brief Holds the newest captured JPEG frame.
 */

#include "frame_store.h"

#include <stdlib.h>
#include <string.h>

void frame_store_init(struct frame_store *store)
{
    memset(store, 0, sizeof(*store));
}

int frame_store_put(struct frame_store *store, const void *data, size_t len)
{
    if (len > store->capacity) {
        /* Grow with headroom, since JPEG frame sizes vary with the scene */
        size_t capacity = len + len / 2;
        unsigned char *buf = realloc(store->data, capacity);

        if (buf == NULL) {
            return -1;
        }
        store->data = buf;
        store->capacity = capacity;
    }
    memcpy(store->data, data, len);
    store->len = len;
    store->seq++;
    return 0;
}

void frame_store_free(struct frame_store *store)
{
    free(store->data);
    memset(store, 0, sizeof(*store));
}
