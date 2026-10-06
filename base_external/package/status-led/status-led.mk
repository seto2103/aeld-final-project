##############################################################
#
# STATUS-LED
#
##############################################################

# Built from driver/ in this repository rather than a pinned git commit
STATUS_LED_SITE = $(BR2_EXTERNAL_AESD_FINAL_PROJECT_PATH)/../driver
STATUS_LED_SITE_METHOD = local
# The overlay is installed next to the firmware's overlays, so those must be installed first
STATUS_LED_DEPENDENCIES = rpi-firmware
STATUS_LED_INSTALL_STAGING = YES
STATUS_LED_INSTALL_IMAGES = YES

# Preprocess for the dt-bindings includes, then compile with the kernel's dtc
define STATUS_LED_BUILD_OVERLAY
	$(HOSTCPP) -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $(LINUX_DIR)/include \
		-o $(@D)/dts/status-led-overlay.dts.tmp $(@D)/dts/status-led-overlay.dts
	$(LINUX_DIR)/scripts/dtc/dtc -@ -I dts -O dtb \
		-o $(@D)/status-led.dtbo $(@D)/dts/status-led-overlay.dts.tmp
endef
STATUS_LED_POST_BUILD_HOOKS += STATUS_LED_BUILD_OVERLAY

# ioctl header for camera-server
define STATUS_LED_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/status_led_ioctl.h $(STAGING_DIR)/usr/include/status_led_ioctl.h
endef

define STATUS_LED_INSTALL_IMAGES_CMDS
	$(INSTALL) -D -m 0644 $(@D)/status-led.dtbo $(BINARIES_DIR)/rpi-firmware/overlays/status-led.dtbo
endef

define STATUS_LED_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/init/S15status-led $(TARGET_DIR)/etc/init.d/S15status-led
endef

$(eval $(kernel-module))
$(eval $(generic-package))
