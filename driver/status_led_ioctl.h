/**
 * @file status_led_ioctl.h
 * @brief ioctl definitions for /dev/status_led, shared by the driver and camera-server.
 *
 * Based on aesd_ioctl.h from the aesdchar driver.
 */

#ifndef STATUS_LED_IOCTL_H
#define STATUS_LED_IOCTL_H

#ifdef __KERNEL__
#include <asm-generic/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
#endif

/** LED modes, passed as a uint32_t */
enum status_led_mode {
    STATUS_LED_MODE_OFF = 0,
    STATUS_LED_MODE_ON = 1,
    STATUS_LED_MODE_BLINK = 2,
};

/** Blink period limits in milliseconds (one full on + off cycle) */
#define STATUS_LED_PERIOD_MIN_MS        50
#define STATUS_LED_PERIOD_MAX_MS        10000
#define STATUS_LED_PERIOD_DEFAULT_MS    500

// Pick an arbitrary unused value from https://github.com/torvalds/linux/blob/master/Documentation/userspace-api/ioctl/ioctl-number.rst
#define STATUS_LED_IOC_MAGIC 0x17

#define STATUS_LED_IOCSMODE     _IOW(STATUS_LED_IOC_MAGIC, 1, uint32_t)
#define STATUS_LED_IOCGMODE     _IOR(STATUS_LED_IOC_MAGIC, 2, uint32_t)
#define STATUS_LED_IOCSPERIOD   _IOW(STATUS_LED_IOC_MAGIC, 3, uint32_t)
#define STATUS_LED_IOCGPERIOD   _IOR(STATUS_LED_IOC_MAGIC, 4, uint32_t)
/**
 * The maximum number of commands supported, used for bounds checking
 */
#define STATUS_LED_IOC_MAXNR 4

#endif /* STATUS_LED_IOCTL_H */
