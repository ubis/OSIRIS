# SPDX-License-Identifier: BSD-2-Clause
#
# OSIRIS top-level build. Every part builds inside its own Docker toolchain
# image; an image is built on first use and rebuilt whenever its Dockerfile
# changes (the tag carries a hash of the Dockerfile). Results go to dist/.
#
#   make dspboot [BOARD=<name>]   first-stage loader -> dist/dspboot/
#   make dspboot-test             its host-side unit tests
#   make dspaudio [AC3=0] [DMA=0] DSP firmware -> build/dspaudio/
#   make buildroot                U-Boot, kernel, root fs, bundle -> dist/images/
#   make toolchain                only the cross toolchain -> build/toolchains/
#   make clean                    remove build output, keep the toolchains, ccache and dl/
#   make dirclean                 also remove the toolchains and ccache
#   make distclean                also remove the downloads in dl/
#   DEFCONFIG=<name>              buildroot and toolchain for another
#                                 external/configs/<name>_defconfig than onkyo_nr

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

BUILDROOT_IMAGE := $(call image_tag,buildroot,docker/Dockerfile)

# Patches to Buildroot for what BR2_EXTERNAL cannot change. When they or
# the submodule's commit change, buildroot/ is reset and patched again,
# losing edits made there, and the main build starts over.

BUILDROOT_PATCHES := $(sort $(wildcard buildroot-patches/*.patch))

buildroot-src:
	@test -f buildroot/Makefile || git submodule update --init buildroot
	@s="$$(git -C buildroot rev-parse HEAD) $$(cat /dev/null $(BUILDROOT_PATCHES) | sha256sum)"; \
	if [ "$$(cat $(BUILD)/.buildroot-patches 2> /dev/null)" != "$$s" ]; then \
		rm -rf $(BUILD)/buildroot-* && \
		git -C buildroot reset -q --hard && git -C buildroot clean -qfd && \
		for p in $(BUILDROOT_PATCHES); do \
			echo "buildroot: $$p"; git -C buildroot apply $(TOP)/$$p || exit 1; \
		done && \
		mkdir -p $(BUILD) && echo "$$s" > $(BUILD)/.buildroot-patches; \
	fi

# The main build's defconfig, external/configs/<DEFCONFIG>_defconfig, names
# its toolchain in its external toolchain path, build/toolchains/<name>;
# toolchain_<name>_defconfig builds that toolchain as a Buildroot SDK.
# Each one built stays in build/toolchains/<name>-<key>, the key taken
# from Buildroot's commit and the defconfig's settings, and <name> links to
# the one in use: "make clean" keeps them, a new build image needs none,
# and going back to an earlier Buildroot or defconfig builds nothing.

DEFCONFIG           ?= onkyo_nr
TOOLCHAINS          := $(BUILD)/toolchains
TOOLCHAIN_NAME      := $(shell sed -n 's|^BR2_TOOLCHAIN_EXTERNAL_PATH=".*/build/toolchains/\(.*\)"$$|\1|p' \
			 external/configs/$(DEFCONFIG)_defconfig)
TOOLCHAIN_DEFCONFIG := external/configs/toolchain_$(TOOLCHAIN_NAME)_defconfig
TOOLCHAIN           := $(TOOLCHAINS)/$(TOOLCHAIN_NAME)
TOOLCHAIN_OUT       := $(BUILD)/toolchain-build
TOOLCHAIN_MAKE      := make O=$(TOOLCHAIN_OUT) BR2_EXTERNAL=$(TOP)/external
TOOLCHAIN_KEY        = $(shell { git -C buildroot rev-parse HEAD; grep -E '^(# )?BR2_' $(TOOLCHAIN_DEFCONFIG); } | \
			 sha256sum | cut -c1-12)

toolchain: buildroot-src
	@$(call ensure_image,$(BUILDROOT_IMAGE),docker)
	@test -f $(TOOLCHAIN_DEFCONFIG) || { echo "no toolchain for $(DEFCONFIG)_defconfig"; exit 1; }
	@k=$(TOOLCHAIN_NAME)-$(TOOLCHAIN_KEY); t=$(TOOLCHAINS)/$$k; \
	if [ ! -f $$t/.done ]; then \
		rm -rf $$t $(TOOLCHAINS)/new $(TOOLCHAIN_OUT) && mkdir -p $(TOOLCHAIN_OUT) $(TOOLCHAINS)/new && \
		$(call docker_run,$(BUILDROOT_IMAGE),$(TOP)/buildroot,sh -c \
			"$(TOOLCHAIN_MAKE) toolchain_$(TOOLCHAIN_NAME)_defconfig > /dev/null && \
			 $(TOOLCHAIN_MAKE) sdk BR2_SDK_PREFIX=toolchain && \
			 tar -xzf $(TOOLCHAIN_OUT)/images/toolchain.tar.gz -C $(TOOLCHAINS)/new && \
			 mv $(TOOLCHAINS)/new/toolchain $$t && $$t/relocate-sdk.sh") && \
		touch $$t/.done && rm -rf $(TOOLCHAINS)/new $(TOOLCHAIN_OUT) || exit 1; \
	fi; \
	ln -sfn $$k $(TOOLCHAIN)
	@echo "toolchain -> $(TOOLCHAIN) ($$(readlink $(TOOLCHAIN)))"

# One output per defconfig; it starts over when its toolchain changes.

BUILDROOT_OUT   := $(BUILD)/buildroot-$(DEFCONFIG)
BUILDROOT_MAKE  := make O=$(BUILDROOT_OUT) BR2_EXTERNAL=$(TOP)/external
BUILDROOT_LOCAL := mcud dspd dspaudio osirisd pcmd

buildroot: dspaudio toolchain
	@t=$$(readlink $(TOOLCHAIN)); \
	if [ "$$(cat $(BUILDROOT_OUT)/.toolchain 2> /dev/null)" != "$$t" ]; then \
		rm -rf $(BUILDROOT_OUT) && mkdir -p $(BUILDROOT_OUT) && echo "$$t" > $(BUILDROOT_OUT)/.toolchain; \
	fi
	$(call docker_run,$(BUILDROOT_IMAGE),$(TOP)/buildroot,sh -c \
		"$(BUILDROOT_MAKE) $(DEFCONFIG)_defconfig > /dev/null && \
		 $(BUILDROOT_MAKE) \$$(cd $(BUILDROOT_OUT) && for p in $(BUILDROOT_LOCAL); do \
			grep -qix BR2_PACKAGE_\$$p=y .config && echo \$$p-rebuild; done) && \
		 $(BUILDROOT_MAKE)")
	mkdir -p $(DIST)
	ln -sfn $(BUILDROOT_OUT)/images $(DIST)/images
	@echo "buildroot -> $(DIST)/images"

clean:
	rm -rf $(DIST) $(BUILD)/buildroot-* $(BUILD)/dspaudio $(BUILD)/dspboot

dirclean: clean
	rm -rf $(BUILD)

distclean: dirclean
	rm -rf $(TOP)/dl

.PHONY: help dspboot dspboot-test dspaudio buildroot-src toolchain buildroot clean dirclean \
	distclean
