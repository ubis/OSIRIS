################################################################################
#
# udebug
#
################################################################################

UDEBUG_VERSION = 6726e98ad69402e79b6d0b305012ace857ca0786
UDEBUG_SITE = https://git.openwrt.org/project/udebug.git
UDEBUG_SITE_METHOD = git
UDEBUG_LICENSE = ISC
UDEBUG_DEPENDENCIES = libubox ubus
UDEBUG_INSTALL_STAGING = YES

# the library only; the daemon and its ucode module are not needed.
# CMake looks for the ucode headers on every target, so point it at a
# directory that exists.
UDEBUG_CONF_OPTS = -Ducode_include_dir=$(STAGING_DIR)/usr/include
UDEBUG_MAKE_OPTS = udebug

define UDEBUG_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0755 $(UDEBUG_BUILDDIR)/libudebug.so $(STAGING_DIR)/usr/lib/libudebug.so
	$(INSTALL) -D -m 0644 $(@D)/udebug.h $(STAGING_DIR)/usr/include/udebug.h
	$(INSTALL) -D -m 0644 $(@D)/udebug-pcap.h $(STAGING_DIR)/usr/include/udebug-pcap.h
endef

define UDEBUG_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(UDEBUG_BUILDDIR)/libudebug.so $(TARGET_DIR)/usr/lib/libudebug.so
endef

$(eval $(cmake-package))
