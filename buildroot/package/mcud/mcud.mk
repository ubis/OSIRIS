################################################################################
#
# mcud
#
################################################################################

MCUD_VERSION = 1.0
MCUD_SITE = $(TOPDIR)/../services/mcud
MCUD_SITE_METHOD = local
MCUD_LICENSE = BSD-2c
MCUD_DEPENDENCIES = ubus libubox

define MCUD_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

define MCUD_INSTALL_TARGET_CMDS
	$(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
