/**
 * @file display.h
 * @brief Display thread: shows the newest camera frame and a status bar on a 16-bit framebuffer,
 *        such as the 3.5" SPI display (fb_ili9486, 480x320).
 *
 * The thread is another reader of the frame store. The status bar shows the time (UTC), the
 * number of HTTP clients, and a red REC marker while a clip is recorded. If the framebuffer
 * can't be used, display_start() logs a warning and returns NULL, and camera-server runs
 * without it.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include "frame_store.h"
#include "http_server.h"
#include "recorder.h"

#define DISPLAY_DEFAULT_DEVICE  "/dev/fb0"

struct display_config {
    const char *device;             /* framebuffer device, for example /dev/fb0 */
    unsigned int width;             /* camera frame size */
    unsigned int height;
};

struct display;

/**
 * Open the framebuffer, clear it and start the display thread.
 * @param recorder shown as REC while it records; may be NULL
 * @param srv      its client count is shown; may be NULL
 * @return the display, or NULL if it can't be used (logged)
 */
struct display *display_start(const struct display_config *cfg, struct frame_store *store,
                              const struct recorder *recorder, const struct http_server *srv);

/** Stop and join the thread, clear the screen and free the display. Does nothing if NULL. */
void display_stop(struct display *d);

#endif /* DISPLAY_H */
