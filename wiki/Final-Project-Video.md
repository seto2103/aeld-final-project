# Final Project Video

## Overview

This page provides an overview of Sebastian Torres's contribution to the AESD Final Project, a
network security camera on a Raspberry Pi 4 Model B running a custom Buildroot Linux image. It
was a solo project, so all of the work below is mine:

* The Buildroot external tree, defconfig and packages that build a bootable SD card image.
* `camera-server`, a multithreaded C daemon: V4L2 capture, MJPEG streaming over HTTP to several
  clients, motion detection, motion-triggered clip recording with a 5 second pre-event buffer
  and automatic deletion of old clips, and a status display on a 3.5" SPI screen.
* `camera-supervisor`, which restarts `camera-server` after a failure and feeds the hardware
  watchdog.
* `status_led`, a kernel driver with a device tree overlay that controls the Pi's onboard ACT LED
  (on, off, blink) through a character device and `ioctl`.
* First-boot setup of the recordings partition, network time, and the init scripts that start
  everything at boot.

See the [Project Overview](Project-Overview.md) for the design and the [Schedule](Schedule.md)
for the issues it was built in.

## Video Outline

See [Final Project Video](https://drive.google.com/file/d/1Flw5Y8asWI2MkZ5BtPYQRghEUUnbfrjg/view?usp=drive_link)

The video demonstrates:

* Project hardware
* Couple implementation details
* Watching the live MJPEG stream in a browser
* Showing rpi with lcd and explaining things on screen
* Explaining the recording led and "REC" on screen
* Unplugging the camera and plugging it back in

## Challenges

My most difficult challenges when implementing this project were:

* **Sharing frames between threads without one slow reader holding up the others.** Capture,
  every HTTP client, motion detection, recording and the display all read the same frames. The
  newest frame is kept in a store guarded by a mutex and condition variable, and readers copy it
  out, so the lock is never held while a client sends over the network. The recorder needs every
  frame, not just the newest, so it reads from a separate ring buffer that also holds the 5
  seconds before motion starts.
* **Writing AVI files that stay playable.** The AVI header needs the frame count and sizes,
  which are only known when the clip ends, so the header is patched and the index is written on
  close. Clips are written under a temporary name and renamed when complete, so a crash or power
  cut never leaves a broken file that looks finished.
* **First boot on a reflashed SD card.** Writing a new image left the old recordings filesystem
  on the card, and `mkfs.ext4` stopped at a "Proceed anyway?" prompt that nobody can answer
  during boot, so the old recordings were silently kept. Found during the full image test and
  fixed by forcing the format of the partition the script had just created.
* **No real-time clock.** The Pi starts in 1970 on every boot, so clips had wrong names and
  deleting the oldest clips by date went wrong after a reboot. BusyBox `ntpd` now sets the clock
  at boot, and clips that start before that are named by uptime instead.
* **The SPI display.** The first version left the last frame frozen on the screen after
  `camera-server` stopped. Reading the kernel's framebuffer code showed that closing the device
  cancels a pending screen update, so the clear never reached the panel; `fsync()` before closing
  fixed it. Colors also showed banding at 16 bits per pixel until dithering was turned on.

## Lessons Learned

The most important topics I learned from this project were:

* **The V4L2 capture API**: negotiating the format with `ioctl`, memory-mapped buffers, and
  streaming I/O, using the camera's own MJPEG compression so the Pi never re-encodes video.
* **Designing threads around data ownership**: one writer per piece of shared data, readers that
  copy and never block the writer, and an orderly shutdown that wakes every waiting thread before
  joining it.
* **Kernel drivers with device tree**: a platform driver bound through a device tree overlay,
  the `gpiod` consumer interface, a kernel timer for blinking, and an `ioctl` interface shared
  with userspace through one header.
* **Building for unattended operation**: init scripts, first-boot setup, process supervision
  with restart back-off, and the hardware watchdog with a magic close so a clean stop doesn't
  reboot the board.
* **Testing on the real image**: several bugs only showed up when the full image was written to
  a card and booted with no manual steps, so a full image check is worth doing before calling a
  feature finished.
