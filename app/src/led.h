/**
 * @file led.h
 * @brief Sets the board's status LED through the status_led driver (/dev/status_led).
 *
 * If the device is missing, led_open() logs a warning and returns NULL, and the other functions
 * do nothing when given NULL, so camera-server runs without the LED.
 */

#ifndef LED_H
#define LED_H

#include "status_led_ioctl.h"

#define LED_DEVICE              "/dev/status_led"
/* One full on + off cycle while recording */
#define LED_BLINK_PERIOD_MS     500

struct led;

/** @return the LED, or NULL if it can't be used (logged as a warning) */
struct led *led_open(const char *path);

/** Set the mode (STATUS_LED_MODE_OFF, _ON or _BLINK). Safe to call from any thread. */
void led_set(struct led *led, enum status_led_mode mode);

/** Turn the LED off and close the device. Does nothing if led is NULL. */
void led_close(struct led *led);

#endif /* LED_H */
