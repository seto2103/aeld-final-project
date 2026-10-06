Title: Motion detection thread compares downscaled frames and reports motion start and end

Sprint: 3
Assignee: seto2103
Blocked by: #6 (multithreaded camera-server)

## Description

Add a motion detection thread to `camera-server` as another reader of the frame store. It
decodes frames at reduced size with libjpeg-turbo (DCT scaling to 1/8, grayscale), compares each
one with a slowly updated background image, and decides when motion starts and when it has
stopped. This issue only detects and reports motion; recording is the next issue.

## Definition of Done

- [ ] `app/src/motion.c` runs in its own thread, reads frames with `frame_store_wait_newer()`,
      and analyzes about 10 frames per second (it skips the rest) so CPU use stays low.
- [ ] Each analyzed frame is decoded to 160x90 grayscale with libjpeg-turbo scaled decoding; the
      fraction of pixels that differ from the background by more than a threshold is the motion
      score.
- [ ] Motion starts after the score is above the trigger level for several consecutive analyzed
      frames, and ends after it stays below for a hold-off time (default 3 s), so single noisy
      frames and short pauses don't start or end an event.
- [ ] The background adapts slowly, so gradual light changes and auto exposure don't count as
      motion.
- [ ] Motion start and end are logged to syslog with the score, and published through a small
      thread-safe API (`motion_active()`, or a callback) for the recorder and the LED.
- [ ] Thresholds have sensible defaults and can be changed with command line options.
- [ ] Shutdown still completes within 2 seconds with the motion thread running.
- [ ] Test: with an empty, still scene for 10 minutes there are no motion events; walking in
      front of the camera starts an event within 1 second, and leaving ends it after the hold-off.
      Capture stays at 25 fps or more. Results and CPU use are added to this issue as a comment.

## Status

Not started.
