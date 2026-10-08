Title: Network time, full image check and wiki update

Sprint: 3
Assignee: seto2103
Blocked by: #9 (motion-triggered recording), #10 (status LED integration), #11 (watchdog supervisor)

## Description

Finish the project so it runs unattended from a freshly written SD card: the clock is set from
the network so clip names have the right time, and everything starts at boot. (Deleting old
recordings moved to #9.) Then update the documentation for the finished system.
The demo video is a separate issue.

## Definition of Done

- [ ] BusyBox `ntpd` is enabled and started at boot, so the clock is correct shortly after the
      network comes up (the Pi has no battery-backed clock and otherwise starts in 1970).
      Recording waits for the clock to be set, or names clips by uptime until it is.
- [ ] A freshly built image written to an SD card boots and, with no manual steps, creates the
      recordings partition, sets the clock, starts streaming, lights the LED, records on motion
      and deletes the oldest clips when the partition fills.
- [ ] The README and the wiki pages describe the finished system, the recordings and how to
      copy them off the Pi.

## Status

Not started.
