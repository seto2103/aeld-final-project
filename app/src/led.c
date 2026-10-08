/**
 * @file led.c
 * @brief Sets the board's status LED through the status_led driver (/dev/status_led).
 *
 * The driver serializes ioctls with its own mutex, so threads can share the descriptor.
 */

#include "led.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

struct led {
    int fd;
};

struct led *led_open(const char *path)
{
    uint32_t period = LED_BLINK_PERIOD_MS;
    struct led *led;
    int fd;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        syslog(LOG_WARNING, "Status LED disabled: cannot open %s: %m", path);
        return NULL;
    }
    if (ioctl(fd, STATUS_LED_IOCSPERIOD, &period) == -1) {
        syslog(LOG_WARNING, "Status LED disabled: setting the blink period failed: %m");
        close(fd);
        return NULL;
    }

    led = malloc(sizeof(*led));
    if (led == NULL) {
        syslog(LOG_WARNING, "Status LED disabled: out of memory");
        close(fd);
        return NULL;
    }
    led->fd = fd;
    return led;
}

void led_set(struct led *led, enum status_led_mode mode)
{
    uint32_t value = mode;

    if (led != NULL && ioctl(led->fd, STATUS_LED_IOCSMODE, &value) == -1) {
        syslog(LOG_WARNING, "Setting the status LED failed: %m");
    }
}

void led_close(struct led *led)
{
    if (led == NULL) {
        return;
    }
    led_set(led, STATUS_LED_MODE_OFF);
    close(led->fd);
    free(led);
}
