Title: Status LED driver controls the onboard ACT LED (on, off, blink) with a device tree overlay

Sprint: 2
Assignee: seto2103
Blocked by: #3 (status-led package)

## Description

Turn the `status_led` placeholder into a working driver for the Raspberry Pi's onboard green
ACT LED (GPIO 42), which the camera uses as its status light. By default the kernel's `leds-gpio`
driver owns this LED as the SD card activity light, so a device tree overlay releases it and
describes it as a `status-led` device for this driver. The char device structure, locking and
`ioctl` handling start from the aesdchar driver; blinking uses a kernel timer.

## Definition of Done

- [ ] `driver/dts/status-led-overlay.dts` disables the ACT LED in `leds-gpio` and adds a node
      with `compatible = "aesd,status-led"` and the LED GPIO. Buildroot compiles it, installs the
      `.dtbo` to the boot partition and adds the `dtoverlay=` line to `config.txt`.
- [ ] `status_led` is a platform driver that binds to that node and gets the LED with the GPIO
      descriptor API (`gpiod`); `/dev/status_led` is created when it binds.
- [ ] Writing `on`, `off` or `blink` to `/dev/status_led` sets the mode; reading it returns the
      current mode.
- [ ] `driver/status_led_ioctl.h` defines ioctls to set the mode and the blink period in
      milliseconds; the header is shared with `camera-server`.
- [ ] Blinking runs from a kernel timer; changing mode, unloading the module or unbinding stops
      the timer safely (`del_timer_sync`) and turns the LED off.
- [ ] Concurrent writers are serialized with a mutex.
- [ ] The module is loaded at boot by the init scripts, and the LED is off after boot.
- [ ] Shell commands showing on, off, blink and read-back, plus `dmesg` load and unload messages,
      are attached to this issue as a comment.

## Status

Not started.
