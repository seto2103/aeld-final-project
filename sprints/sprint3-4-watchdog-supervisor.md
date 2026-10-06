Title: camera-supervisor restarts camera-server after a failure and feeds the hardware watchdog

Sprint: 3
Assignee: seto2103
Blocked by: #6 (multithreaded camera-server)

## Description

Add a second program, `camera-supervisor`, that the init script starts instead of
`camera-server`. It runs `camera-server` as a child process and starts it again whenever it
exits, for example after a crash or a camera unplug. It also feeds the Pi's hardware watchdog
(`/dev/watchdog`, the `bcm2835_wdt` driver), so if the whole system hangs the board reboots on
its own.

## Definition of Done

- [ ] `app/src/supervisor.c` builds a second binary, `camera-supervisor`, installed by the
      `camera-server` package.
- [ ] The supervisor starts `camera-server` in the foreground with `fork()` and `exec()`,
      waits for it, and restarts it after it exits, with a growing delay (1 s, 2 s, 4 s, up to
      30 s) that resets once the daemon has run for a while. Each exit and restart is logged
      with the exit status or signal.
- [ ] The supervisor opens `/dev/watchdog`, sets a timeout (default 15 s) and feeds it every
      few seconds. On a clean stop it disables the watchdog with the magic close (`V`), so
      stopping the service does not reboot the board.
- [ ] SIGTERM to the supervisor stops `camera-server`, waits for it, and exits.
- [ ] `S90camera-server` starts and stops `camera-supervisor`; `camera-server` itself no longer
      needs `-d`.
- [ ] Tests, with output added to this issue as a comment:
  - `kill -9` of `camera-server`: it is restarted within a few seconds.
  - Camera unplugged and plugged back in: streaming resumes without user action.
  - `kill -9` of the supervisor (no magic close): the board reboots after the watchdog timeout
    and comes back up streaming.
  - `/etc/init.d/S90camera-server stop`: everything stops and the board does not reboot.

## Status

Not started.
