Title: V4L2 capture daemon captures MJPEG frames with mmap streaming (single-threaded)

Sprint: 2
Assignee: seto2103
Blocked by: #2 (camera bring-up), #3 (camera-server package)

## Description

Replace the `camera-server` placeholder with a daemon that captures frames from the Logitech
StreamCam through the V4L2 API. This first version is a single loop in one thread: it dequeues
each frame, records it as the newest frame, and requeues the buffer. Threads are added in a
later Sprint 2 issue, once capture and streaming are proven to work.

Capture settings, from issue #2: MJPG, 1280x720, 30 fps.

## Definition of Done

- [ ] `app/src/capture.c` opens `/dev/video0`, checks it supports video capture and streaming,
      sets MJPG 1280x720 (`VIDIOC_S_FMT`) and 30 fps (`VIDIOC_S_PARM`), requests 4 buffers
      (`VIDIOC_REQBUFS`), maps them with `mmap()` and streams with `VIDIOC_QBUF`/`VIDIOC_DQBUF`.
- [ ] The capture code exposes its file descriptor so the main loop can wait on it with
      `poll()` (used by the HTTP issue).
- [ ] The device, resolution and frame rate can be changed with command line options; the
      defaults are the values above.
- [ ] `camera-server -d` runs as a daemon (as in aesdsocket) and is started by
      `S90camera-server`. Errors and status go to syslog.
- [ ] Every 10 seconds the daemon logs the measured capture rate; over one minute it stays at
      25 fps or more.
- [ ] `camera-server --snapshot <file>` captures one frame to a file and exits; the file opens as
      a valid 1280x720 JPEG on a PC.
- [ ] SIGINT and SIGTERM stop the daemon cleanly: streaming is stopped, buffers are unmapped and
      the device is closed. `/etc/init.d/S90camera-server stop` exits within 2 seconds.
- [ ] If the camera is unplugged, the daemon logs an error and exits with a non-zero status
      instead of hanging.
- [ ] Syslog output showing the capture rate and the snapshot test are attached to this issue
      as a comment.

## Status

Not started.
