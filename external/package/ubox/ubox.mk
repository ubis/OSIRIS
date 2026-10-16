################################################################################
#
# ubox
#
################################################################################

UBOX_VERSION = 6f78fa496bf36c55864a41e353df7d13f04b1077
UBOX_SITE = https://git.openwrt.org/project/ubox.git
UBOX_SITE_METHOD = git
UBOX_LICENSE = GPL-2.0
UBOX_DEPENDENCIES = libubox ubus json-c udebug

# only logd, logread and kmodloader, which procd's init runs and which
# is also insmod, rmmod, lsmod, modinfo and modprobe; the other tools want
# uci. CMake looks for uci.h on every target, so point it at a directory
# that exists.
UBOX_CONF_OPTS = -Duci_include_dir=$(STAGING_DIR)/usr/include
UBOX_MAKE_OPTS = logd logread kmodloader

define UBOX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(UBOX_BUILDDIR)/logd $(TARGET_DIR)/sbin/logd
	$(INSTALL) -D -m 0755 $(UBOX_BUILDDIR)/logread $(TARGET_DIR)/sbin/logread
	$(INSTALL) -D -m 0755 $(UBOX_BUILDDIR)/kmodloader $(TARGET_DIR)/sbin/kmodloader
	for t in insmod rmmod lsmod modinfo modprobe; do \
		ln -sf kmodloader $(TARGET_DIR)/sbin/$$t; \
	done
endef

$(eval $(cmake-package))
