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

The current version serves one client at a time; other connections receive `503 Service
Unavailable` until it disconnects. Run `camera-server --help` for the command line options.

## Status LED

The onboard green ACT LED is the camera's status light. A device tree overlay
(`driver/dts/status-led-overlay.dts`, enabled with `dtoverlay=status-led` in `config.txt`) takes it
away from the SD card activity trigger, and the `status_led` driver, loaded at boot, controls it
through `/dev/status_led`:

```
echo blink > /dev/status_led    # also on, off
cat /dev/status_led             # prints the current mode
```

Programs can also set the mode and blink period with the ioctls in `driver/status_led_ioctl.h`.

## Source layout

| Path | Contents |
| --- | --- |
| `app/` | `camera-server` userspace daemon (C), its Makefile and init script |
| `driver/` | `status_led` kernel module, its device tree overlay and init script |
| `base_external/` | Buildroot external tree: project defconfig, `config.txt`, and the `camera-server` and `status-led` packages |
| `buildroot/` | Buildroot submodule, pinned to 2024.02.13 |
| `wiki/` | Project Overview and Schedule pages |

The Buildroot packages build directly from `app/` and `driver/` in this repository, so changes
don't need to be committed before they can be built.

## Rebuilding after a code change

Rebuild one package and copy the result to the Pi instead of reflashing the SD card:

```
make -C buildroot camera-server-rebuild
scp -O buildroot/output/target/usr/bin/camera-server root@<pi-address>:/usr/bin/

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
