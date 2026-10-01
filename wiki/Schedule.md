# Schedule

Project board: [aeld-final-project Projects](https://github.com/seto2103/aeld-final-project/projects)

All issues are assigned to Sebastian Torres ([@seto2103](https://github.com/seto2103)).

## Top level schedule

| Sprint | Issue | Blocked by | Status |
| --- | --- | --- | --- |
| 1 | [#1 Buildroot image boots on Raspberry Pi 4B with SSH access](https://github.com/seto2103/aeld-final-project/issues/1) | none | Not started |
| 1 | [#2 StreamCam bring-up: capture a frame with v4l2-ctl](https://github.com/seto2103/aeld-final-project/issues/2) | #1 | Not started |
| 1 | [#3 Buildroot external tree with skeleton app and driver packages](https://github.com/seto2103/aeld-final-project/issues/3) | #1 | Not started |
| 2 | V4L2 capture daemon captures MJPEG frames with mmap streaming | #2, #3 | Planned |
| 2 | MJPEG HTTP server streams live video to multiple browser clients | capture daemon | Planned |
| 2 | GPIO char driver controls the status LED (on, off, blink), with device tree overlay | #3 | Planned |
| 3 | 5 second pre-event ring buffer; on motion a clip is saved from the buffer until motion stops | HTTP server | Planned |
| 3 | Status LED blinks while a clip is being recorded | LED driver, motion recording | Planned |
| 3 | Watchdog: supervisor restarts the capture daemon after a crash and feeds the Pi hardware watchdog | HTTP server | Planned |
| 3 | Start on boot, recording rotation, final demo video and wiki update | motion recording | Planned |

## Sprint 1

Goal: a custom image runs on the board, the camera is proven to work from userspace, and the
repository is ready to receive application and driver code.

* [#1 Buildroot image boots on Raspberry Pi 4B with SSH access](https://github.com/seto2103/aeld-final-project/issues/1)
* [#2 StreamCam bring-up: capture a frame with v4l2-ctl](https://github.com/seto2103/aeld-final-project/issues/2)
* [#3 Buildroot external tree with skeleton app and driver packages](https://github.com/seto2103/aeld-final-project/issues/3)

## Sprint 2

Goal: live video is viewable in a browser, and the GPIO driver can drive the LED.

Issues will be created at the start of the sprint.

## Sprint 3

Goal: motion-triggered recording works end to end and the system starts on boot.

Issues will be created at the start of the sprint.
