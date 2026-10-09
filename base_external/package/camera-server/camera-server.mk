##############################################################
#
# CAMERA-SERVER
#
##############################################################

# Built from app/ in this repository rather than a pinned git commit
CAMERA_SERVER_SITE = $(BR2_EXTERNAL_AESD_FINAL_PROJECT_PATH)/../app
CAMERA_SERVER_SITE_METHOD = local
CAMERA_SERVER_DEPENDENCIES = jpeg status-led

# clean first: the local source is copied with any host build binaries left in app/
define CAMERA_SERVER_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) clean all
endef

define CAMERA_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/camera-server $(TARGET_DIR)/usr/bin/camera-server
	$(INSTALL) -D -m 0755 $(@D)/camera-supervisor $(TARGET_DIR)/usr/bin/camera-supervisor
endef

define CAMERA_SERVER_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/init/S20recordings $(TARGET_DIR)/etc/init.d/S20recordings
	$(INSTALL) -D -m 0755 $(@D)/init/S90camera-server $(TARGET_DIR)/etc/init.d/S90camera-server
endef

$(eval $(generic-package))
