################################################################################
#
# ubus
#
################################################################################

# the last revision before the packaged libubox (2014-01-29), so the two match
UBUS_VERSION = 4e82a1fabb87b5e3c948a792e16b0fac3702721b
UBUS_SITE = https://git.openwrt.org/project/ubus.git
UBUS_SITE_METHOD = git
UBUS_LICENSE = LGPLv2.1
UBUS_INSTALL_STAGING = YES
UBUS_DEPENDENCIES = libubox json-c
UBUS_CONF_OPT += -DBUILD_LUA:BOOL=OFF -DBUILD_EXAMPLES:BOOL=OFF -DENABLE_SYSTEMD:BOOL=OFF

# this libubox includes <json/json.h>; json-c installs its headers as json-c/
define UBUS_JSON_C_COMPAT
	[ -e $(STAGING_DIR)/usr/include/json ] || \
		ln -s json-c $(STAGING_DIR)/usr/include/json
endef
UBUS_PRE_CONFIGURE_HOOKS += UBUS_JSON_C_COMPAT

$(eval $(cmake-package))
