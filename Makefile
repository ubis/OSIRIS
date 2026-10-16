# SPDX-License-Identifier: BSD-2-Clause
#
# OSIRIS top-level build. Every part builds inside its own Docker toolchain
# image; an image is built on first use and rebuilt whenever its Dockerfile
# changes (the tag carries a hash of the Dockerfile). Results go to dist/.
#
#   make dspboot [BOARD=<name>]   first-stage loader -> dist/dspboot/
#   make dspboot-test             its host-side unit tests
#   make dspaudio [AC3=0] [DMA=0] DSP firmware -> build/dspaudio/
#   make toolchain                the cross toolchain (musl) -> build/toolchain/
#   make buildroot                U-Boot, kernel, root fs, bundle -> dist/images/
#   make clean                    remove build output, keep the toolchain, ccache and dl/
#   make dirclean                 also remove the toolchain and ccache
#   make distclean                also remove the downloads in dl/

TOP    := $(CURDIR)
DIST   := $(TOP)/dist
BUILD  := $(TOP)/build
DOCKER ?= docker

# docker_run <image>, <workdir>, <command>: as the calling user, repo mounted at
# the same path; --init so that Ctrl-C reaches make inside the container. The
# user has no passwd entry there, so the kernel gets its name for its version
# string, and the host name is this host's rather than the container's.
docker_run = $(DOCKER) run --rm --init -u $(shell id -u):$(shell id -g) \
	-e KBUILD_BUILD_USER=$(shell id -un) -h $(shell hostname -s) \
	-v $(TOP):$(TOP) -w $(2) $(1) $(3)

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

# dspaudio: the DSP firmware, with dspboot's C6000 toolchain

dspaudio:
	@$(call ensure_image,$(DSPBOOT_IMAGE),dspboot/docker)
	$(call docker_run,$(DSPBOOT_IMAGE),$(TOP)/services/dspaudio,\
		make O=$(BUILD)/dspaudio VERSION=$(shell sh tools/version.sh) \
		$(if $(DMA),DMA=$(DMA)) $(if $(AC3),AC3=$(AC3)))

# Buildroot (the buildroot/ submodule) with the OSIRIS tree in external/:
# U-Boot, the kernel and the root file system in one build, with the
# toolchain below; the sources it downloads are kept in dl/. Buildroot copies
# local sources only once, so this repository's own packages, those the
# defconfig enables, are rebuilt every time.

BUILDROOT_IMAGE := $(call image_tag,buildroot,toolchain/buildroot/Dockerfile)

# The toolchain: a Buildroot SDK built once from its own defconfig, again
# only when that, Buildroot or the build image changes, and used by the main
# build as an external toolchain, so that "make clean" keeps it. A new
# toolchain starts the main build over.

TOOLCHAIN_DEFCONFIG := external/configs/onkyo_nr_toolchain_defconfig
TOOLCHAIN_OUT       := $(BUILD)/toolchain-build
TOOLCHAIN           := $(BUILD)/toolchain
TOOLCHAIN_MAKE      := make O=$(TOOLCHAIN_OUT) BR2_EXTERNAL=$(TOP)/external

toolchain:
	@test -f buildroot/Makefile || git submodule update --init buildroot
	@$(call ensure_image,$(BUILDROOT_IMAGE),toolchain/buildroot)
	@s="$(BUILDROOT_IMAGE) $$(git -C buildroot rev-parse HEAD) $$(sha256sum < $(TOOLCHAIN_DEFCONFIG))"; \
	if [ "$$(cat $(TOOLCHAIN)/.stamp 2> /dev/null)" != "$$s" ]; then \
		rm -rf $(TOOLCHAIN) $(TOOLCHAIN_OUT) $(BUILDROOT_OUT) && mkdir -p $(TOOLCHAIN_OUT) && \
		$(call docker_run,$(BUILDROOT_IMAGE),$(TOP)/buildroot,sh -c \
			"$(TOOLCHAIN_MAKE) onkyo_nr_toolchain_defconfig > /dev/null && \
			 $(TOOLCHAIN_MAKE) sdk BR2_SDK_PREFIX=toolchain && \
			 tar -xzf $(TOOLCHAIN_OUT)/images/toolchain.tar.gz -C $(BUILD) && \
			 $(TOOLCHAIN)/relocate-sdk.sh") && \
		echo "$$s" > $(TOOLCHAIN)/.stamp && \
		rm -rf $(TOOLCHAIN_OUT); \
	fi
	@echo "toolchain -> $(TOOLCHAIN)"

BUILDROOT_OUT   := $(BUILD)/buildroot
BUILDROOT_MAKE  := make O=$(BUILDROOT_OUT) BR2_EXTERNAL=$(TOP)/external
BUILDROOT_LOCAL := mcud dspd dspaudio osirisd pcmd

buildroot: dspaudio toolchain
	mkdir -p $(BUILDROOT_OUT)
	$(call docker_run,$(BUILDROOT_IMAGE),$(TOP)/buildroot,sh -c \
		"$(BUILDROOT_MAKE) onkyo_nr_defconfig > /dev/null && \
		 $(BUILDROOT_MAKE) \$$(cd $(BUILDROOT_OUT) && for p in $(BUILDROOT_LOCAL); do \
			grep -qix BR2_PACKAGE_\$$p=y .config && echo \$$p-rebuild; done) && \
		 $(BUILDROOT_MAKE)")
	mkdir -p $(DIST)
	ln -sfn $(BUILDROOT_OUT)/images $(DIST)/images
	@echo "buildroot -> $(DIST)/images"

clean:
	rm -rf $(DIST) $(BUILDROOT_OUT) $(BUILD)/dspaudio $(BUILD)/dspboot

dirclean: clean
	rm -rf $(BUILD)

distclean: dirclean
	rm -rf $(TOP)/dl

.PHONY: help dspboot dspboot-test dspaudio toolchain buildroot clean dirclean distclean
