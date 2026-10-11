# SPDX-License-Identifier: BSD-2-Clause
#
# OSIRIS top-level build. Every part builds inside its own Docker toolchain
# image; an image is built on first use and rebuilt whenever its Dockerfile
# changes (the tag carries a hash of the Dockerfile). Results go to dist/.
#
#   make dspboot [BOARD=<name>]   first-stage loader -> dist/dspboot/
#   make dspboot-test             its host-side unit tests
#   make u-boot                   U-Boot 2009.11 -> dist/u-boot/
#   make clean                    remove build output (not the Docker images)

TOP    := $(CURDIR)
DIST   := $(TOP)/dist
BUILD  := $(TOP)/build
JOBS   ?= $(shell nproc)
DOCKER ?= docker

# docker_run <image>, <workdir>, <command>: as the calling user, repo mounted at
# the same path; --init so that Ctrl-C reaches make inside the container
docker_run = $(DOCKER) run --rm --init -u $(shell id -u):$(shell id -g) -v $(TOP):$(TOP) -w $(2) $(1) $(3)

# image_tag <name>, <Dockerfile>
image_tag = osiris-$(1):$(shell sha256sum $(2) | cut -c1-12)

# ensure_image <tag>, <context dir>
ensure_image = $(DOCKER) image inspect $(1) > /dev/null 2>&1 || $(DOCKER) build -t $(1) $(2)

help:
	@sed -n 's/^#   //p' Makefile

# dspboot

DSPBOOT_IMAGE := $(call image_tag,dspboot,dspboot/docker/Dockerfile)
DSPBOOT_VER    = $(shell sh dspboot/tools/version.sh)
DSPBOOT_MAKE   = make VERSION=$(word 1,$(DSPBOOT_VER)) SOURCE_DATE_EPOCH=$(word 2,$(DSPBOOT_VER)) \
		 O=$(BUILD)/dspboot $(if $(BOARD),BOARD=$(BOARD))

dspboot:
	@$(call ensure_image,$(DSPBOOT_IMAGE),dspboot/docker)
	$(call docker_run,$(DSPBOOT_IMAGE),$(TOP)/dspboot,$(DSPBOOT_MAKE) dist DIST=$(DIST)/dspboot)
	@echo "dspboot -> $(DIST)/dspboot"

dspboot-test:
	@$(call ensure_image,$(DSPBOOT_IMAGE),dspboot/docker)
	$(call docker_run,$(DSPBOOT_IMAGE),$(TOP)/dspboot,$(DSPBOOT_MAKE) all test)

# U-Boot, with GCC 4.8 (current compilers do not build the 2009 tree). The
# slot image is what dspboot starts, the .nand files are it as page + spare
# records with the boot ROM's ECC for nandrestore.py (slot a 0x140000,
# slot b 0x1C0000).

LEGACY_IMAGE := $(call image_tag,legacy,toolchain/legacy/Dockerfile)
UBOOT_OUT    := $(BUILD)/u-boot
UBOOT_MAKE   := make -s O=$(UBOOT_OUT) CROSS_COMPILE=arm-none-eabi-
UBOOT_NAME    = u-boot $(shell git describe --always --dirty)

u-boot:
	@$(call ensure_image,$(LEGACY_IMAGE),toolchain/legacy)
	mkdir -p $(UBOOT_OUT)
	$(call docker_run,$(LEGACY_IMAGE),$(TOP)/u-boot,sh -c \
		"$(UBOOT_MAKE) da830evm_config && $(UBOOT_MAKE) -j$(JOBS) all")
	rm -rf $(DIST)/u-boot
	mkdir -p $(DIST)/u-boot
	cp $(UBOOT_OUT)/u-boot.bin $(DIST)/u-boot/
	python3 dspboot/tools/mkimage.py $(UBOOT_OUT)/u-boot.bin $(DIST)/u-boot/u-boot.dbi \
		--name "$(UBOOT_NAME)"
	for p in 512:512b 2048:2k 4096:4k; do \
		python3 dspboot/tools/mknandimg.py $(DIST)/u-boot/u-boot.dbi --page $${p%%:*} \
			--pad-block -o $(DIST)/u-boot/u-boot-$${p##*:}.nand > /dev/null || exit 1; \
	done
	@echo "$(UBOOT_NAME) -> $(DIST)/u-boot"

clean:
	rm -rf $(DIST) $(BUILD)

.PHONY: help dspboot dspboot-test u-boot clean
