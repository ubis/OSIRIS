################################################################################
#
# osirisd
#
################################################################################

OSIRISD_VERSION = 1.0
OSIRISD_SITE = $(TOPDIR)/../services/osirisd
OSIRISD_SITE_METHOD = local
OSIRISD_LICENSE = BSD-2c
OSIRISD_DEPENDENCIES = luajit ubus

define OSIRISD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/osirisd $(TARGET_DIR)/usr/sbin/osirisd
endef

$(eval $(generic-package))
