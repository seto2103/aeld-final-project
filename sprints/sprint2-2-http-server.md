Title: MJPEG HTTP server streams live video to a browser (single-threaded)

Sprint: 2
Assignee: seto2103
Blocked by: #4 (V4L2 capture daemon)

## Description

Add an HTTP server to `camera-server` so the live camera view can be watched in a browser on the
local network. This version stays single-threaded: the main loop uses `poll()` to wait on the
camera, the listening socket and one connected client at the same time, and writes each new
frame straight to that client. Video is sent as MJPEG over HTTP (`multipart/x-mixed-replace`):
each part is one JPEG frame, so the Pi does no video encoding. Multiple clients are added in the
multithreaded issue.

## Definition of Done

- [ ] `app/src/http_server.c` listens on TCP port 8080 (changeable with a command line option).
      The socket setup starts from the aesdsocket assignment.
- [ ] The main loop waits on the camera, the listening socket and the client with `poll()`;
      nothing blocks on one of them while another is ready.
- [ ] `GET /` returns a small HTML page showing the stream.
- [ ] `GET /stream` returns `multipart/x-mixed-replace` with one JPEG part per new frame, each
      with `Content-Type: image/jpeg` and `Content-Length` headers.
- [ ] `GET /snapshot.jpg` returns the newest single frame.
- [ ] Any other path returns `404 Not Found`; malformed requests return `400 Bad Request`.
- [ ] While one client is streaming, a second connection receives `503 Service Unavailable`
      and is closed.
- [ ] A client that disconnects mid-stream is cleaned up and the server keeps running (no
      SIGPIPE termination); a new client can then connect.
- [ ] A browser shows smooth live video for five minutes while the capture rate stays at
      25 fps or more.
- [ ] The README documents the stream URLs.
- [ ] A description or screenshot of the browser view, and the 503 and reconnect tests, are
      added to this issue as a comment.

## Status

Not started.
