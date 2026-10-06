Title: Network time, recording rotation, full image check, demo video and wiki update

Sprint: 3
Assignee: seto2103
Blocked by: #9 (motion-triggered recording), #10 (status LED integration), #11 (watchdog supervisor)

## Description

Finish the project so it runs unattended from a freshly written SD card: the clock is set from
the network so clip names have the right time, old recordings are deleted before the partition
fills, and everything starts at boot. Then record the demo and update the documentation.

## Definition of Done

- [ ] BusyBox `ntpd` is enabled and started at boot, so the clock is correct shortly after the
      network comes up (the Pi has no battery-backed clock and otherwise starts in 1970).
      Recording waits for the clock to be set, or names clips by uptime until it is.
- [ ] Old recordings are deleted, oldest first, whenever free space on `/data` drops below a
      limit (default 10%), so the partition never fills. Each deletion is logged.
- [ ] A freshly built image written to an SD card boots and, with no manual steps, creates the
      recordings partition, sets the clock, starts streaming, lights the LED and records on
      motion.
- [ ] A demo video shows the live stream in a browser, a motion-triggered recording with the 5
      seconds before the motion, the LED blinking while recording, and recovery after the
      camera is unplugged.
- [ ] The README and the wiki pages describe the finished system, the recordings and how to
      copy them off the Pi, and the Schedule marks all issues done.

## Status

Not started.
