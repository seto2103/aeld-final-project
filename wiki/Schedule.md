# Schedule

Project board: https://github.com/users/seto2103/projects/1

All issues are assigned to Sebastian Torres ([@seto2103](https://github.com/seto2103)).

## Top level schedule

| Sprint | Issue | Blocked by | Status |
| --- | --- | --- | --- |
| 1 | [#1 Buildroot image boots on Raspberry Pi 4B with SSH access](https://github.com/seto2103/aeld-final-project/issues/1) | none | Done |
| 1 | [#2 StreamCam bring-up: capture a frame with v4l2-ctl](https://github.com/seto2103/aeld-final-project/issues/2) | #1 | Done |
| 1 | [#3 Buildroot external tree with skeleton app and driver packages](https://github.com/seto2103/aeld-final-project/issues/3) | #1 | Done |
| 2 | [#4 V4L2 capture daemon captures MJPEG frames with mmap streaming (single-threaded)](https://github.com/seto2103/aeld-final-project/issues/4) | #2, #3 | Done |
| 2 | [#5 MJPEG HTTP server streams live video to a browser (single-threaded)](https://github.com/seto2103/aeld-final-project/issues/5) | #4 | Done |
| 2 | [#6 Multithreaded camera-server: capture thread, shared frame store, thread per client](https://github.com/seto2103/aeld-final-project/issues/6) | #5 | Done |
| 2 | [#7 Status LED driver controls the onboard ACT LED (on, off, blink) with a device tree overlay](https://github.com/seto2103/aeld-final-project/issues/7) | #3 | Done |
| 3 | 5 second pre-event ring buffer; on motion a clip is saved from the buffer until motion stops | #6 | Planned |
| 3 | Status LED blinks while a clip is being recorded | #7, motion recording | Planned |
| 3 | Watchdog: supervisor restarts the capture daemon after a crash and feeds the Pi hardware watchdog | #6 | Planned |
| 3 | Start on boot, recording rotation, final demo video and wiki update | motion recording | Planned |

## Sprint 1

Goal: a custom image runs on the board, the camera is proven to work from userspace, and the
repository is ready to receive application and driver code.

* [#1 Buildroot image boots on Raspberry Pi 4B with SSH access](https://github.com/seto2103/aeld-final-project/issues/1)
* [#2 StreamCam bring-up: capture a frame with v4l2-ctl](https://github.com/seto2103/aeld-final-project/issues/2)
* [#3 Buildroot external tree with skeleton app and driver packages](https://github.com/seto2103/aeld-final-project/issues/3)

## Sprint 2

Goal: live video is viewable in a browser, and the GPIO driver can drive the LED.

The application is built single-threaded first (#4, #5) to prove capture and streaming, then
restructured into threads (#6) so several clients can watch and Sprint 3 features can be added.

* [#4 V4L2 capture daemon captures MJPEG frames with mmap streaming (single-threaded)](https://github.com/seto2103/aeld-final-project/issues/4)
* [#5 MJPEG HTTP server streams live video to a browser (single-threaded)](https://github.com/seto2103/aeld-final-project/issues/5)
* [#6 Multithreaded camera-server: capture thread, shared frame store, thread per client](https://github.com/seto2103/aeld-final-project/issues/6)
* [#7 Status LED driver controls the onboard ACT LED (on, off, blink) with a device tree overlay](https://github.com/seto2103/aeld-final-project/issues/7)

## Sprint 3

Goal: motion-triggered recording works end to end and the system starts on boot.

Issues will be created at the start of the sprint.
