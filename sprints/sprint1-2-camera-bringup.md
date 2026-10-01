Title: StreamCam bring-up: capture a frame with v4l2-ctl

Sprint: 1
Assignee: seto2103
Blocked by: #1 (needs a booting image to add kernel options and packages to)

## Description

Prove that the Logitech StreamCam works on the Buildroot image before any custom capture code
is written. The camera is a USB Video Class device, so it should be handled by the in-kernel
`uvcvideo` driver. This issue also records which pixel formats and resolutions the camera
offers, which decides the design of the capture daemon in Sprint 2.

## Definition of Done

- [ ] The kernel configuration enables USB Video Class support (`CONFIG_USB_VIDEO_CLASS`) and
      V4L2, and the change is saved in the repository.
- [ ] The `v4l-utils` package is enabled in the project defconfig.
- [ ] With the StreamCam connected through a USB-C to USB-A adapter, `dmesg` shows the camera
      being detected and `/dev/video0` exists.
- [ ] The output of `v4l2-ctl --list-formats-ext` is attached to this issue, and a comment
      states whether MJPG is offered and which resolution and frame rate will be used.
- [ ] One frame is captured to a file on the Pi with `v4l2-ctl --stream-mmap --stream-count=1
      --stream-to=frame.jpg`, copied to a PC with `scp`, and opens as a valid image. The image
      is attached to this issue.

## Status

Not started.
