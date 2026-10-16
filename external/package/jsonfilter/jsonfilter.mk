################################################################################
#
# jsonfilter
#
################################################################################

JSONFILTER_VERSION = b9034210bd331749673416c6bf389cccd4e23610
JSONFILTER_SITE = https://git.openwrt.org/project/jsonpath.git
JSONFILTER_SITE_METHOD = git
JSONFILTER_LICENSE = ISC
JSONFILTER_DEPENDENCIES = libubox json-c

define JSONFILTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(JSONFILTER_BUILDDIR)/jsonpath \
		$(TARGET_DIR)/usr/bin/jsonfilter
endef

$(eval $(cmake-package))
