Title: Pre-event ring buffer, motion-triggered clip recording and rotation on a recordings partition

Sprint: 3
Assignee: seto2103
Blocked by: #8 (motion detection)

## Description

Keep the most recent 5 seconds of frames in a ring buffer in memory, and when motion starts,
save a clip that begins with those buffered frames and continues until motion has ended. 720p
MJPEG at 30 fps is about 4.5 MB/s, too much for the root filesystem, so recordings go to their
own partition that uses the rest of the SD card.

## Definition of Done

- [ ] The capture thread also appends every frame to a ring buffer in `app/src/frame_ring.c`
      that holds at least 5 seconds of frames (about 24 MB at 720p), with the oldest frames
      overwritten. Unlike the frame store, the recorder reads every frame from it, so a short SD
      card stall does not lose frames; frames that are overwritten before they are written are
      counted and logged.
- [ ] A recorder thread in `app/src/recorder.c` starts a clip when motion starts, writes the
      buffered 5 seconds first, then live frames, and closes the clip after motion ends. A clip
      that reaches 1 GB (the AVI 1.0 limit, about 3.5 minutes) is closed and a new file
      continues the event.
- [ ] Clips are MJPEG in an AVI container, so they play in VLC and other players
      with the right frame rate, and are named by start time, for example
      `/data/recordings/2026-11-02_14-03-27.avi`.
- [ ] A third partition for recordings is created at first boot from the free space on the SD
      card, formatted ext4 and mounted at `/data` by the init scripts. If it is missing or
      full, `camera-server` keeps streaming and logs that recording is disabled.
- [ ] Old recordings are deleted, oldest first, whenever free space on `/data` drops below a
      limit (default 10%, at least 200 MB), before a clip starts and while it is written, so
      the newest recordings are always kept and the partition never fills. The clip being
      written is never deleted, and each deletion is logged.
- [ ] A clip that is being written when the daemon stops is closed properly and still plays.
- [ ] Test: walking in front of the camera produces one clip that starts about 5 seconds before
      the motion and ends about 3 seconds after it, plays smoothly in VLC, and has no dropped
      frames in the log. Streaming clients are unaffected while recording. A short description
      and the clip's details (duration, size, frame count) are added to this issue as a comment.

## Status

Not started.
