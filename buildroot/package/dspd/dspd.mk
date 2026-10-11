################################################################################
#
# dspd
#
################################################################################

DSPD_VERSION = 1.0
DSPD_SITE = $(TOPDIR)/../services/dspd
DSPD_SITE_METHOD = local
DSPD_LICENSE = BSD-2c
DSPD_DEPENDENCIES = ubus libubox

define DSPD_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define DSPD_INSTALL_TARGET_CMDS
	$(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
