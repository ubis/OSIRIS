################################################################################
#
# procd
#
################################################################################

# the last revision that builds against Buildroot's libubox
PROCD_VERSION = b72282fe33c2b3ac49915c797c237df94a3883b3
PROCD_SITE = https://git.openwrt.org/project/procd.git
PROCD_SITE_METHOD = git
PROCD_LICENSE = LGPL-2.1, GPL-2.0 (hotplug-call), BSD-2-Clause (rc.common)
PROCD_DEPENDENCIES = libubox ubus json-c udebug

PROCD_CONF_OPTS = -DEARLY_PATH="/usr/sbin:/usr/bin:/sbin:/bin"
PROCD_MAKE_OPTS = init procd setlbf

# hotplug-call, without OpenWrt's functions.sh, which no hotplug script here
# uses, and the hotplug rules from OpenWrt; rc.common is OSIRIS's own
PROCD_OPENWRT_VERSION = 71e991a8c01af94fb1fd6d6e4e2d259ce0ff36da
PROCD_OPENWRT_FILES = \
	base-files/files/sbin/hotplug-call \
	system/procd/files/hotplug.json \
	system/procd/files/hotplug-preinit.json
PROCD_EXTRA_DOWNLOADS = $(addprefix \
	https://raw.githubusercontent.com/openwrt/openwrt/$(PROCD_OPENWRT_VERSION)/package/, \
	$(PROCD_OPENWRT_FILES))

define PROCD_INSTALL_TARGET_CMDS
	for f in init procd; do \
		$(INSTALL) -D -m 0755 $(@D)/$$f $(TARGET_DIR)/sbin/$$f || exit 1; \
	done
	$(INSTALL) -D -m 0644 $(@D)/libsetlbf.so $(TARGET_DIR)/lib/libsetlbf.so
	$(INSTALL) -D -m 0755 $(PROCD_PKGDIR)/rc.common $(TARGET_DIR)/etc/rc.common
	sed -e 's,%PATH%,/usr/sbin:/usr/bin:/sbin:/bin,' -e '\,^\. /lib/functions.sh,d' \
		$(PROCD_DL_DIR)/hotplug-call > $(TARGET_DIR)/sbin/hotplug-call
	chmod 0755 $(TARGET_DIR)/sbin/hotplug-call
	$(INSTALL) -m 0644 $(PROCD_DL_DIR)/hotplug.json \
		$(PROCD_DL_DIR)/hotplug-preinit.json $(TARGET_DIR)/etc/
	mkdir -p $(TARGET_DIR)/etc/rc.d $(TARGET_DIR)/etc/hotplug.json.d \
		$(TARGET_DIR)/etc/hotplug-preinit.json.d
endef

$(eval $(cmake-package))
