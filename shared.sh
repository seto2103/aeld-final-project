#!/bin/sh
# Shared definitions for buildroot scripts

# The defconfig from the buildroot directory we use for Raspberry Pi 4 builds
RPI4_DEFCONFIG=configs/raspberrypi4_64_defconfig
# The place we store customizations to the Raspberry Pi 4 configuration
MODIFIED_RPI4_DEFCONFIG=base_external/configs/aesd_rpi4_defconfig
# The defconfig from the buildroot directory we use for the project
AESD_DEFAULT_DEFCONFIG=${RPI4_DEFCONFIG}
AESD_MODIFIED_DEFCONFIG=${MODIFIED_RPI4_DEFCONFIG}
AESD_MODIFIED_DEFCONFIG_REL_BUILDROOT=../${AESD_MODIFIED_DEFCONFIG}
