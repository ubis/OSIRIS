################################################################################
#
# pcmd
#
################################################################################

PCMD_VERSION = 1.0
PCMD_SITE = $(BR2_EXTERNAL_OSIRIS_PATH)/../services/pcmd
PCMD_SITE_METHOD = local
PCMD_LICENSE = BSD-2c
PCMD_DEPENDENCIES =

define PCMD_BUILD_CMDS
	$(MAKE) $(TARGET_CONFIGURE_OPTS) DSPD=$(BR2_EXTERNAL_OSIRIS_PATH)/../services/dspd -C $(@D)
endef

define PCMD_INSTALL_TARGET_CMDS
	$(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

$(eval $(generic-package))
