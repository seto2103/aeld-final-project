##############################################################
#
# STATUS-LED
#
##############################################################

# Built from driver/ in this repository rather than a pinned git commit
STATUS_LED_SITE = $(BR2_EXTERNAL_AESD_FINAL_PROJECT_PATH)/../driver
STATUS_LED_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
