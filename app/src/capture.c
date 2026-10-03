/**
 * @file capture.c
 * @brief V4L2 MJPEG capture from a USB webcam using memory-mapped streaming buffers.
 */

#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#define CAPTURE_NUM_BUFFERS 4

struct capture_buffer {
    void *start;
    size_t length;
};

struct capture {
    int fd;
    struct capture_buffer buffers[CAPTURE_NUM_BUFFERS];
    unsigned int num_buffers;
    int streaming;
    int dequeued_index;     /* buffer held by the caller, or -1 */
    unsigned int width;
    unsigned int height;
    unsigned int fps;
};

/* Retry an ioctl interrupted by a signal */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static int queue_buffer(struct capture *cap, unsigned int index)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (xioctl(cap->fd, VIDIOC_QBUF, &buf) == -1) {
        syslog(LOG_ERR, "VIDIOC_QBUF failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int check_capabilities(struct capture *cap, const char *device)
{
    struct v4l2_capability caps;

    memset(&caps, 0, sizeof(caps));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &caps) == -1) {
        syslog(LOG_ERR, "%s is not a V4L2 device: %s", device, strerror(errno));
        return -1;
    }
    if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        syslog(LOG_ERR, "%s does not support video capture", device);
        return -1;
    }
    if (!(caps.capabilities & V4L2_CAP_STREAMING)) {
        syslog(LOG_ERR, "%s does not support streaming I/O", device);
        return -1;
    }
    syslog(LOG_INFO, "Opened %s: %s (driver %s)", device, caps.card, caps.driver);
    return 0;
}

static int set_format(struct capture *cap, const struct capture_config *cfg)
{
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg->width;
    fmt.fmt.pix.height = cfg->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) == -1) {
        syslog(LOG_ERR, "VIDIOC_S_FMT failed: %s", strerror(errno));
        return -1;
    }
    /* The driver adjusts the request to the nearest supported mode */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        syslog(LOG_ERR, "Camera does not support MJPEG");
        return -1;
    }
    cap->width = fmt.fmt.pix.width;
    cap->height = fmt.fmt.pix.height;

    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cfg->fps;
    if (xioctl(cap->fd, VIDIOC_S_PARM, &parm) == -1) {
        syslog(LOG_ERR, "VIDIOC_S_PARM failed: %s", strerror(errno));
        return -1;
    }
    if (parm.parm.capture.timeperframe.numerator != 0) {
        cap->fps = parm.parm.capture.timeperframe.denominator /
                   parm.parm.capture.timeperframe.numerator;
    }

    syslog(LOG_INFO, "Capture format MJPEG %ux%u at %u fps", cap->width, cap->height, cap->fps);
    return 0;
}

static int map_buffers(struct capture *cap)
{
    struct v4l2_requestbuffers req;
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.count = CAPTURE_NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) == -1) {
        syslog(LOG_ERR, "VIDIOC_REQBUFS failed: %s", strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        syslog(LOG_ERR, "Driver granted only %u buffer(s)", req.count);
        return -1;
    }
    if (req.count > CAPTURE_NUM_BUFFERS) {
        req.count = CAPTURE_NUM_BUFFERS;
    }

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            syslog(LOG_ERR, "VIDIOC_QUERYBUF failed: %s", strerror(errno));
            return -1;
        }

        cap->buffers[i].length = buf.length;
        cap->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                     cap->fd, buf.m.offset);
        if (cap->buffers[i].start == MAP_FAILED) {
            cap->buffers[i].start = NULL;
            syslog(LOG_ERR, "mmap failed: %s", strerror(errno));
            return -1;
        }
        cap->num_buffers++;
    }
    return 0;
}

static int start_streaming(struct capture *cap)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned int i;

    for (i = 0; i < cap->num_buffers; i++) {
        if (queue_buffer(cap, i) == -1) {
            return -1;
        }
    }
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) == -1) {
        syslog(LOG_ERR, "VIDIOC_STREAMON failed: %s", strerror(errno));
        return -1;
    }
    cap->streaming = 1;
    return 0;
}

struct capture *capture_open(const struct capture_config *cfg)
{
    struct capture *cap;

    cap = calloc(1, sizeof(*cap));
    if (cap == NULL) {
        syslog(LOG_ERR, "Out of memory");
        return NULL;
    }
    cap->dequeued_index = -1;
    cap->fps = cfg->fps;

    cap->fd = open(cfg->device, O_RDWR | O_NONBLOCK);
    if (cap->fd == -1) {
        syslog(LOG_ERR, "Cannot open %s: %s", cfg->device, strerror(errno));
        free(cap);
        return NULL;
    }

    if (check_capabilities(cap, cfg->device) == -1 || set_format(cap, cfg) == -1 ||
        map_buffers(cap) == -1 || start_streaming(cap) == -1) {
        capture_close(cap);
        return NULL;
    }
    return cap;
}

int capture_fd(const struct capture *cap)
{
    return cap->fd;
}

void capture_get_format(const struct capture *cap, unsigned int *width, unsigned int *height,
                        unsigned int *fps)
{
    *width = cap->width;
    *height = cap->height;
    *fps = cap->fps;
}

int capture_dequeue(struct capture *cap, const void **data, size_t *len)
{
    struct v4l2_buffer buf;

    for (;;) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) {
                return 1;
            }
            /* ENODEV when the camera is unplugged */
            syslog(LOG_ERR, "VIDIOC_DQBUF failed: %s", strerror(errno));
            return -1;
        }
        if (buf.index >= cap->num_buffers) {
            syslog(LOG_ERR, "Driver returned invalid buffer index %u", buf.index);
            return -1;
        }

        /* Skip corrupted or empty frames and keep waiting for a good one */
        if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused == 0) {
            if (queue_buffer(cap, buf.index) == -1) {
                return -1;
            }
            continue;
        }

        cap->dequeued_index = (int)buf.index;
        *data = cap->buffers[buf.index].start;
        *len = buf.bytesused;
        return 0;
    }
}

int capture_release(struct capture *cap)
{
    int index = cap->dequeued_index;

    if (index < 0) {
        return 0;
    }
    cap->dequeued_index = -1;
    return queue_buffer(cap, (unsigned int)index);
}

void capture_close(struct capture *cap)
{
    unsigned int i;

    if (cap == NULL) {
        return;
    }
    if (cap->streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        /* Fails harmlessly if the camera has already been unplugged */
        xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    }
    for (i = 0; i < cap->num_buffers; i++) {
        if (cap->buffers[i].start != NULL) {
            munmap(cap->buffers[i].start, cap->buffers[i].length);
        }
    }
    if (cap->fd != -1) {
        close(cap->fd);
    }
    free(cap);
}
