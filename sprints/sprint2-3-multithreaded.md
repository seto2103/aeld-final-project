Title: Multithreaded camera-server: capture thread, shared frame store, thread per client

Sprint: 2
Assignee: seto2103
Blocked by: #5 (MJPEG HTTP server)

## Description

Restructure the single-threaded `camera-server` so several clients can watch at once and the
Sprint 3 features (motion detection, pre-event buffer, recording) can be added as independent
consumers. A capture thread writes each frame into a shared frame store; every HTTP client gets
its own thread that waits for new frames. The thread list, accept loop and shutdown handling
come from the aesdsocket assignment.

## Definition of Done

- [ ] A capture thread owns the V4L2 device and is the only writer to the frame store.
- [ ] `app/src/frame_store.c` holds the newest frame with a sequence number, protected by a
      mutex, and wakes readers with a condition variable. A reader can wait for "a frame newer
      than N" and receives its own copy.
- [ ] The accept loop starts one thread per client, tracked in a list; finished threads are
      joined, and the 503 limit from the single-threaded version is removed (a configurable
      maximum, default 4, is enforced instead).
- [ ] A slow client cannot slow down capture or other clients: it skips frames instead.
- [ ] Three clients (for example two browsers and VLC) watch the stream at the same time for
      five minutes; each sees smooth video and the capture rate stays at 25 fps or more.
- [ ] SIGINT and SIGTERM stop all threads (capture and clients), join them, close the sockets
      and the device, and exit within 2 seconds.
- [ ] Results of the three-client test are added to this issue as a comment.

## Status

Not started.
