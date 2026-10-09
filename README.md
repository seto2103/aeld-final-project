# AESD Final Project: Raspberry Pi 4 Network Security Camera

A Buildroot-based network security camera for the Raspberry Pi 4 Model B. A Logitech StreamCam is
captured through the V4L2 API, streamed live to a browser as MJPEG over HTTP, and recorded to disk
when motion is detected. A custom GPIO character driver controls a status LED.

See the [Project Overview](wiki/Project-Overview.md) page for the full proposal, and the
[Schedule](wiki/Schedule.md) page for sprint status.

## Building the image

Buildroot is included as a git submodule. On an Ubuntu host with the
[Buildroot prerequisites](https://buildroot.org/downloads/manual/manual.html#requirement)
installed:

```
git clone --recurse-submodules git@github.com:seto2103/aeld-final-project.git
cd aeld-final-project
./build.sh
```

The first build takes a long time because it compiles the toolchain and the kernel. The SD card
image is written to `buildroot/output/images/sdcard.img`.

* `./save-config.sh` saves configuration changes made with `make -C buildroot menuconfig` to
  `base_external/configs/aesd_rpi4_defconfig`.
* `./clean.sh` removes all build output and the Buildroot configuration.

## Writing the image to an SD card

Insert the SD card, find its device name with `lsblk`, and write the image to it. Replace
`/dev/sdX` with the SD card device; everything on that device is erased.

```
sudo dd if=buildroot/output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Writing the image to a card that was already used in the camera also erases its recordings: the
first boot creates and formats the recordings partition again. Copy the clips off first (see
[Recordings](#recordings)).

## Running

Insert the SD card in the Raspberry Pi 4 Model B, connect Ethernet and power it on. The Pi gets
its address by DHCP and runs an SSH server:

```
ssh root@<pi-address>
```

The root password is `root`.

## Viewing the camera

`camera-server` starts at boot and serves the camera on port 8080:

| URL | Content |
| --- | --- |
| `http://<pi-address>:8080/` | Web page showing the live stream |
| `http://<pi-address>:8080/stream` | MJPEG stream (`multipart/x-mixed-replace`), also playable in VLC |
| `http://<pi-address>:8080/snapshot.jpg` | Newest single frame |

Up to 4 clients can watch at once (change with `-c`); further connections receive `503 Service
Unavailable`. Run `camera-server --help` for the command line options.

## Motion detection

`camera-server` checks about 10 frames per second for motion, comparing a 160x90 grayscale
version of each frame with a slowly updated background. Motion starts when at least 1% of the
pixels have changed for about half a second, and ends 3 seconds after the changes stop. Events
are logged to syslog:

```
grep -E "Motion (started|ended)" /var/log/messages
```

The thresholds can be changed with `--motion-percent`, `--motion-pixel` and `--motion-holdoff`.
The highest score every 10 seconds is logged at debug level, which helps when choosing them.

## Recordings

Each motion event is saved as an MJPEG AVI clip in `/data/recordings`, starting 5 seconds before
the motion and ending 3 seconds after it, named by its start time in UTC (for example
`2026-11-02_14-03-27.avi`). Clips play in VLC. A clip that is still being written ends in
`.avi.part`, and one longer than 1 GB (about 3.5 minutes) continues in a new file.

`/data` is the third partition of the SD card. On the first boot after writing the image, the
`S20recordings` init script creates it from the free space after the root filesystem and formats
it ext4; on later boots it is checked and mounted. When less than 10% of it is free, the oldest
clips are deleted (change with `--record-free`).

To copy the clips to your computer:

```
scp -O 'root@<pi-address>:/data/recordings/*.avi' .
```

`--record-dir` changes the directory, and `--record-dir ""` turns recording off.

## Network time

The Pi has no battery-backed clock, so it starts every boot in 1970. BusyBox `ntpd`, started by
`S45ntpd`, sets the clock from `pool.ntp.org` shortly after the network comes up. A clip that
starts before then is named by the uptime of its first frame instead (for example
`uptime_00h00m42s.avi`); without network access every clip is named that way. Clips are deleted
oldest first by modification time, so clips from before the clock was set are deleted first.

## Status LED

The onboard green ACT LED is the camera's status light:

| LED | Meaning |
| --- | --- |
| Steady on | `camera-server` is capturing |
| Blinking | A motion clip is being recorded |
| Off | `camera-server` is not running, for example while the camera is unplugged |

A device tree overlay
(`driver/dts/status-led-overlay.dts`, enabled with `dtoverlay=status-led` in `config.txt`) takes it
away from the SD card activity trigger, and the `status_led` driver, loaded at boot, controls it
through `/dev/status_led`:

```
echo blink > /dev/status_led    # also on, off
cat /dev/status_led             # prints the current mode
```

`camera-server` sets the LED with the ioctls in `driver/status_led_ioctl.h`, so a mode written by
hand only lasts until its next change. Without the driver, `camera-server` runs without the LED.

## Supervisor and watchdog

The init script (`/etc/init.d/S90camera-server`) starts `camera-supervisor`, which runs
`camera-server` as a child process and starts it again whenever it exits, for example after a
crash or while the camera is unplugged. The delay before each restart doubles from 1 s up to 30 s,
and goes back to 1 s once `camera-server` has run for a minute, so after the camera is plugged
back in streaming resumes within 30 s.

The supervisor also opens the Pi's hardware watchdog (`/dev/watchdog`) with a 15 s timeout and
feeds it every 5 s. If the system hangs, or the supervisor itself dies, the board reboots on its
own. `/etc/init.d/S90camera-server stop` disarms the watchdog before the supervisor exits, so
stopping the service does not reboot the board.

Options after `--` are passed to `camera-server`, and `camera-supervisor -h` lists the
supervisor's own options. Both programs log to syslog (`/var/log/messages`).

## Source layout

| Path | Contents |
| --- | --- |
| `app/` | `camera-server` daemon and `camera-supervisor` (C), their Makefile and init scripts |
| `driver/` | `status_led` kernel module, its device tree overlay and init script |
| `base_external/` | Buildroot external tree: project defconfig, `config.txt`, and the `camera-server` and `status-led` packages |
| `buildroot/` | Buildroot submodule, pinned to 2024.02.13 |
| `wiki/` | Project Overview and Schedule pages |

The Buildroot packages build directly from `app/` and `driver/` in this repository, so changes
don't need to be committed before they can be built.

## Rebuilding after a code change

Rebuild one package and copy the result to the Pi instead of reflashing the SD card. Stop the
service first, because a running program's binary can't be overwritten:

```
make -C buildroot camera-server-rebuild
ssh root@<pi-address> /etc/init.d/S90camera-server stop
scp -O buildroot/output/target/usr/bin/camera-server buildroot/output/target/usr/bin/camera-supervisor root@<pi-address>:/usr/bin/
ssh root@<pi-address> /etc/init.d/S90camera-server start

make -C buildroot status-led-rebuild
scp -O buildroot/output/target/lib/modules/*/extra/status_led.ko.xz root@<pi-address>:/lib/modules/$(ls buildroot/output/target/lib/modules)/extra/
```

`-O` makes `scp` use the classic copy protocol, because the Pi's SSH server (dropbear) has no
SFTP support, which newer `scp` versions use by default.

A changed device tree overlay goes on the boot partition, and takes effect after a reboot:

```
ssh root@<pi-address> 'mkdir -p /mnt/boot && mount /dev/mmcblk0p1 /mnt/boot'
scp -O buildroot/output/build/status-led/status-led.dtbo root@<pi-address>:/mnt/boot/overlays/
ssh root@<pi-address> 'umount /mnt/boot && reboot'
```

The app also builds on the development host for quick compile checks: `make -C app`.

## AI Tool Usage

Claude (Anthropic), through the Claude Code command line tool, was used for assistance with
planning, documentation and code in this project, under the course's Full AI Tool Usage policy.
All AI-assisted work was reviewed, tested and approved by me.
