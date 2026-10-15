################################################################################
#
# dspaudio
#
################################################################################

# built by "make dspaudio" at the top of the repository (C6000 toolchain)
DSPAUDIO_VERSION = 1.0
DSPAUDIO_SITE = $(TOPDIR)/../build/dspaudio
DSPAUDIO_SITE_METHOD = local
DSPAUDIO_LICENSE = BSD-2-Clause, GPL-2.0-or-later (with liba52)

define DSPAUDIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/dspaudio.bin $(TARGET_DIR)/usr/lib/osiris/dspaudio.bin
	rm -f $(TARGET_DIR)/usr/lib/osiris/dspaudio.ddr
	if [ -f $(@D)/dspaudio.ddr ]; then \
		$(INSTALL) -D -m 0644 $(@D)/dspaudio.ddr $(TARGET_DIR)/usr/lib/osiris/dspaudio.ddr; \
	fi
endef

$(eval $(generic-package))
