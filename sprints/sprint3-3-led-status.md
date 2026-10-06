Title: camera-server drives the status LED: on while running, blinking while recording

Sprint: 3
Assignee: seto2103
Blocked by: #7 (status LED driver), #9 (motion-triggered recording)

## Description

Connect `camera-server` to the `status_led` driver from #7 using the ioctls in
`status_led_ioctl.h`, so the board's ACT LED shows the camera's state without opening the
stream.

## Definition of Done

- [ ] `app/src/led.c` opens `/dev/status_led` and sets the mode and blink period with
      `STATUS_LED_IOCSMODE` and `STATUS_LED_IOCSPERIOD`.
- [ ] The LED is on (steady) while the camera is capturing, blinks while a clip is being
      recorded, and is off when `camera-server` stops, whether it exits cleanly or after a
      camera error.
- [ ] If `/dev/status_led` is missing, `camera-server` logs a warning and runs without the LED.
- [ ] Test: start, record a clip by walking in front of the camera, and stop; the LED changes
      on, blinking, on, off. Unplugging the camera turns it off. A short video or description is
      added to this issue as a comment.

## Status

Not started.
