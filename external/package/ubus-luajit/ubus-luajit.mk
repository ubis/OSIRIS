################################################################################
#
# ubus-luajit
#
################################################################################

# built from the libubox and ubus sources Buildroot already has
UBUS_LUAJIT_SOURCE =
UBUS_LUAJIT_LICENSE = LGPL-2.1, ISC
UBUS_LUAJIT_DEPENDENCIES = libubox ubus luajit

# LuaJIT has only the public name of luaI_openlib, and no Lua 5.0 luaL_reg
UBUS_LUAJIT_CFLAGS = $(TARGET_CFLAGS) -fPIC -I$(STAGING_DIR)/usr/include/luajit-2.1 \
	-DluaI_openlib=luaL_openlib -DluaL_reg=luaL_Reg

define UBUS_LUAJIT_BUILD_CMDS
	$(TARGET_CC) $(UBUS_LUAJIT_CFLAGS) -shared -o $(@D)/uloop.so \
		$(LIBUBOX_DIR)/lua/uloop.c $(TARGET_LDFLAGS) -lubox -lluajit-5.1
	$(TARGET_CC) $(UBUS_LUAJIT_CFLAGS) -include limits.h -shared -o $(@D)/ubus.so \
		$(UBUS_DIR)/lua/ubus.c $(TARGET_LDFLAGS) -lubus -lubox -lblobmsg_json -lluajit-5.1
endef

define UBUS_LUAJIT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/uloop.so $(TARGET_DIR)/usr/lib/lua/5.1/uloop.so
	$(INSTALL) -D -m 0755 $(@D)/ubus.so $(TARGET_DIR)/usr/lib/lua/5.1/ubus.so
endef

$(eval $(generic-package))
