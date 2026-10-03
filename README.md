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
