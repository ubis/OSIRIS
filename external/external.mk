include $(sort $(wildcard $(BR2_EXTERNAL_OSIRIS_PATH)/package/*/*.mk))

# New U-Boot files live as files under board/onkyo/nr/u-boot and are
# copied into its source tree after the patches, which only change
# existing upstream files.
OSIRIS_BOARD_DIR = $(BR2_EXTERNAL_OSIRIS_PATH)/board/onkyo/nr

define UBOOT_OSIRIS_FILES
	cp -a $(OSIRIS_BOARD_DIR)/u-boot/. $(@D)/
endef
UBOOT_POST_PATCH_HOOKS += UBOOT_OSIRIS_FILES
