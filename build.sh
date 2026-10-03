#!/bin/bash
#Script to build buildroot configuration
#Based on the AESD assignment build.sh by Siddhant Jajoo

set -e
cd `dirname $0`
source shared.sh

EXTERNAL_REL_BUILDROOT=../base_external
git submodule init
git submodule sync
git submodule update

if [ ! -e buildroot/.config ]
then
	echo "MISSING BUILDROOT CONFIGURATION FILE"

	if [ -e ${AESD_MODIFIED_DEFCONFIG} ]
	then
		echo "USING ${AESD_MODIFIED_DEFCONFIG}"
		make -C buildroot defconfig BR2_EXTERNAL=${EXTERNAL_REL_BUILDROOT} BR2_DEFCONFIG=${AESD_MODIFIED_DEFCONFIG_REL_BUILDROOT}
	else
		echo "Run ./save-config.sh to save this as the default configuration in ${AESD_MODIFIED_DEFCONFIG}"
		echo "Then add packages as needed to complete the installation, re-running ./save-config.sh as needed"
		make -C buildroot defconfig BR2_EXTERNAL=${EXTERNAL_REL_BUILDROOT} BR2_DEFCONFIG=${AESD_DEFAULT_DEFCONFIG}
	fi
else
	echo "USING EXISTING BUILDROOT CONFIG"
	echo "To force update, delete .config or make changes using make menuconfig and build again."
fi

make -C buildroot BR2_EXTERNAL=${EXTERNAL_REL_BUILDROOT}

echo "SD card image: buildroot/output/images/sdcard.img"
