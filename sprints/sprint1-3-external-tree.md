Title: Buildroot external tree with skeleton app and driver packages

Sprint: 1
Assignee: seto2103
Blocked by: #1 (needs the base Buildroot configuration)

## Description

Set up the repository layout and the Buildroot external tree (`BR2_EXTERNAL`) so that the
capture application and the GPIO kernel driver each have a package which builds into the image.
The packages contain only placeholder code in this sprint; the aim is that Sprint 2 work can
start by editing source files, without further build system changes.

## Definition of Done

- [ ] The repository contains `app/` (userspace daemon) and `driver/` (kernel module)
      directories, each with a Makefile.
- [ ] A `BR2_EXTERNAL` tree in the repository defines one package for the application and one
      kernel module package for the driver, and both are enabled in the project defconfig.
- [ ] The placeholder application is installed in the image and prints a version string when
      run on the Pi.
- [ ] The placeholder kernel module loads with `modprobe` on the Pi and logs a message visible
      in `dmesg`, and unloads cleanly with `rmmod`.
- [ ] `build.sh` builds the complete image, including both packages, from a clean checkout.
- [ ] Terminal output showing the application and the module running on the Pi is attached to
      this issue.

## Status

Not started.
