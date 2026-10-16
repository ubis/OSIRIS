################################################################################
#
# osirisd
#
################################################################################

OSIRISD_VERSION = 1.0
OSIRISD_SITE = $(BR2_EXTERNAL_OSIRIS_PATH)/../services/osirisd
OSIRISD_SITE_METHOD = local
OSIRISD_LICENSE = BSD-2c
OSIRISD_DEPENDENCIES = luajit ubus ubus-luajit

define OSIRISD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/osirisd $(TARGET_DIR)/usr/sbin/osirisd
	for f in $(@D)/*.lua; do \
		$(INSTALL) -D -m 0644 $$f $(TARGET_DIR)/usr/share/osirisd/$${f##*/} || exit 1; \
	done
endef

$(eval $(generic-package))
