################################################################################
#
# procd
#
################################################################################

# the last revision that builds against Buildroot's libubox
PROCD_VERSION = b72282fe33c2b3ac49915c797c237df94a3883b3
PROCD_SITE = https://git.openwrt.org/project/procd.git
PROCD_SITE_METHOD = git
PROCD_LICENSE = LGPL-2.1, GPL-2.0 (udevtrigger, scripts)
PROCD_DEPENDENCIES = libubox ubus json-c udebug

PROCD_CONF_OPTS = -DEARLY_PATH="/usr/sbin:/usr/bin:/sbin:/bin"
PROCD_MAKE_OPTS = init procd udevtrigger setlbf

# the shell side of procd, rc.common and the hotplug rules, from OpenWrt
PROCD_OPENWRT_VERSION = 71e991a8c01af94fb1fd6d6e4e2d259ce0ff36da
PROCD_OPENWRT_FILES = \
	base-files/files/etc/rc.common \
	base-files/files/lib/functions.sh \
	base-files/files/lib/functions/service.sh \
	base-files/files/sbin/hotplug-call \
	system/procd/files/procd.sh \
	system/procd/files/hotplug.json \
	system/procd/files/hotplug-preinit.json
PROCD_EXTRA_DOWNLOADS = $(addprefix \
	https://raw.githubusercontent.com/openwrt/openwrt/$(PROCD_OPENWRT_VERSION)/package/, \
	$(PROCD_OPENWRT_FILES))

define PROCD_INSTALL_TARGET_CMDS
	for f in init procd udevtrigger; do \
		$(INSTALL) -D -m 0755 $(@D)/$$f $(TARGET_DIR)/sbin/$$f || exit 1; \
	done
	$(INSTALL) -D -m 0644 $(@D)/libsetlbf.so $(TARGET_DIR)/lib/libsetlbf.so
	patch -s -o $(TARGET_DIR)/etc/rc.common $(PROCD_DL_DIR)/rc.common \
		$(PROCD_PKGDIR)/rc.common-running.diff
	chmod 0755 $(TARGET_DIR)/etc/rc.common
	$(INSTALL) -D -m 0644 $(PROCD_DL_DIR)/functions.sh $(TARGET_DIR)/lib/functions.sh
	$(INSTALL) -D -m 0644 $(PROCD_DL_DIR)/service.sh $(TARGET_DIR)/lib/functions/service.sh
	$(INSTALL) -D -m 0644 $(PROCD_DL_DIR)/procd.sh $(TARGET_DIR)/lib/functions/procd.sh
	sed 's,%PATH%,/usr/sbin:/usr/bin:/sbin:/bin,' $(PROCD_DL_DIR)/hotplug-call \
		> $(TARGET_DIR)/sbin/hotplug-call
	chmod 0755 $(TARGET_DIR)/sbin/hotplug-call
	$(INSTALL) -m 0644 $(PROCD_DL_DIR)/hotplug.json \
		$(PROCD_DL_DIR)/hotplug-preinit.json $(TARGET_DIR)/etc/
	mkdir -p $(TARGET_DIR)/etc/rc.d $(TARGET_DIR)/etc/hotplug.json.d \
		$(TARGET_DIR)/etc/hotplug-preinit.json.d
	# procd starts ubusd from /sbin
	ln -sf ../usr/sbin/ubusd $(TARGET_DIR)/sbin/ubusd
endef

$(eval $(cmake-package))
