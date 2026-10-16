include $(sort $(wildcard $(BR2_EXTERNAL_OSIRIS_PATH)/package/*/*.mk))

# New U-Boot and Linux files live as files under board/onkyo/nr/u-boot
# and board/onkyo/nr/linux and are copied into the source trees after the
# patches, which only change existing upstream files.
OSIRIS_BOARD_DIR = $(BR2_EXTERNAL_OSIRIS_PATH)/board/onkyo/nr

define UBOOT_OSIRIS_FILES
	cp -a $(OSIRIS_BOARD_DIR)/u-boot/. $(@D)/
endef
UBOOT_POST_PATCH_HOOKS += UBOOT_OSIRIS_FILES

define LINUX_OSIRIS_FILES
	cp -a $(OSIRIS_BOARD_DIR)/linux/. $(@D)/
endef
LINUX_POST_PATCH_HOOKS += LINUX_OSIRIS_FILES

# AirPlay announced through umdns: shairport-sync's external mDNS backend
# runs avahi-publish-service, which is OSIRIS's umdns shim
SHAIRPORT_SYNC_CONF_OPTS += --with-external-mdns
