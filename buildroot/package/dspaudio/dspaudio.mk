################################################################################
#
# dspaudio
#
################################################################################

# built by "make dspaudio" at the top of the repository (C6000 toolchain)
DSPAUDIO_VERSION = 1.0
DSPAUDIO_SITE = $(TOPDIR)/../build/dspaudio
DSPAUDIO_SITE_METHOD = local
DSPAUDIO_LICENSE = BSD-2c

define DSPAUDIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/dspaudio.bin $(TARGET_DIR)/usr/lib/osiris/dspaudio.bin
endef

$(eval $(generic-package))
