Title: Buildroot image boots on Raspberry Pi 4B with SSH access

Sprint: 1
Assignee: seto2103
Blocked by: none

## Description

Create the base Buildroot configuration for the project, starting from
`raspberrypi4_64_defconfig`, and confirm that the resulting image boots on the Raspberry Pi 4
Model B and can be reached over the network. All later work depends on this image.

## Definition of Done

- [ ] Buildroot is added to the repository as a git submodule, with a `build.sh` script that
      builds the image from a clean checkout.
- [ ] A project defconfig based on `raspberrypi4_64_defconfig` is saved in the repository.
- [ ] The image is written to an SD card and the Pi boots to a login prompt on the serial
      console or HDMI.
- [ ] The Pi obtains an IP address over Ethernet and accepts an SSH login (dropbear) as root.
- [ ] The README documents how to build the image and write it to the SD card.
- [ ] The boot log and a successful SSH session are attached to this issue as a comment.

## Status

Not started.
