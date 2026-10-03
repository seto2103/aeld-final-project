/**
 * @file capture.h
 * @brief V4L2 MJPEG capture from a USB webcam using memory-mapped streaming buffers.
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <stddef.h>

struct capture_config {
    const char *device;     /* for example /dev/video0 */
    unsigned int width;
    unsigned int height;
    unsigned int fps;
};

struct capture;

/**
 * Open the device, set MJPEG format and frame rate, map the buffers and start streaming.
 * The device is opened non-blocking, so capture_fd() can be used with poll().
 * @return the capture handle, or NULL on error (logged to syslog)
 */
struct capture *capture_open(const struct capture_config *cfg);

/** File descriptor of the device, readable (POLLIN) when a frame is ready. */
int capture_fd(const struct capture *cap);

/** Resolution and frame rate the driver actually accepted. */
void capture_get_format(const struct capture *cap, unsigned int *width, unsigned int *height,
                        unsigned int *fps);

/**
 * Take the next filled buffer from the driver.
 * On success *data and *len describe one JPEG frame, valid until capture_release().
 * @return 0 on success, 1 if no frame is ready yet, -1 on error (for example camera unplugged)
 */
int capture_dequeue(struct capture *cap, const void **data, size_t *len);

/**
 * Give the buffer from the last successful capture_dequeue() back to the driver.
 * @return 0 on success, -1 on error
 */
int capture_release(struct capture *cap);

/** Stop streaming, unmap the buffers, close the device and free the handle. */
void capture_close(struct capture *cap);

#endif /* CAPTURE_H */
