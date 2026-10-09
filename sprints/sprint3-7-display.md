Title: 3.5" SPI display shows the live camera image and the recording status

Sprint: 3
Assignee: seto2103
Blocked by: #12 (network time, full image check)

## Description

Show the camera on a 3.5" 480x320 SPI touch display (Hosyond, ILI9486 display controller and
XPT2046 touch controller) plugged onto the Pi's GPIO header, so the camera can be checked
without a browser. The kernel's `fb_ili9486` driver and the stock `piscreen` device tree overlay
already work with this panel: a test pattern written to `/dev/fb0` showed the right colors and
orientation.

A display thread in `camera-server` reads the newest frame from the frame store, like the motion
thread, and draws it with a status bar. The SPI bus limits the screen to about 8 to 10 frames
per second; streaming and recording stay at 30 fps.

## Definition of Done

- [ ] The image enables the display: `dtoverlay=piscreen` in the project `config.txt`, and an
      init script loads `spi_bcm2835` and `fb_ili9486` at boot (the image has no hotplug
      helper to load them).
- [ ] A display thread in `camera-server` decodes the newest frame with libjpeg-turbo, scaled
      to 480x270 straight into RGB565, and writes it to the memory-mapped framebuffer.
- [ ] A status bar under the image shows the time (UTC), a red REC marker while a clip is
      recorded, and the number of stream viewers, using a small built-in bitmap font.
- [ ] `--display PATH` selects the framebuffer (default `/dev/fb0`, `""` disables it), and
      `camera-server` runs normally without a display. The screen is cleared when
      `camera-server` stops.
- [ ] Tests, with output added to this issue as a comment:
  - display frame rate and `camera-server` CPU use with and without the display;
  - REC shown while a clip is recorded, and the stream and recordings still at 30 fps with
    0 frames dropped;
  - camera unplugged and plugged back in, and service stopped: the screen clears, and shows
    the image again once `camera-server` is back.
- [ ] The README and the wiki pages describe the display.

## Status

Not started.
