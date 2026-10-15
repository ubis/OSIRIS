# SPDX-License-Identifier: BSD-2-Clause
#
# OSIRIS top-level build. Every part builds inside its own Docker toolchain
# image; an image is built on first use and rebuilt whenever its Dockerfile
# changes (the tag carries a hash of the Dockerfile). Results go to dist/.
#
#   make dspboot [BOARD=<name>]   first-stage loader -> dist/dspboot/
#   make dspboot-test             its host-side unit tests
#   make dspaudio [AC3=0] [DMA=0] DSP firmware -> build/dspaudio/
#   make buildroot                toolchain, U-Boot, kernel, root fs, bundle -> dist/images/
#   make clean                    remove build output (not the Docker images)

TOP    := $(CURDIR)
DIST   := $(TOP)/dist
BUILD  := $(TOP)/build
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

# dspaudio: the DSP firmware, with dspboot's C6000 toolchain

dspaudio:
	@$(call ensure_image,$(DSPBOOT_IMAGE),dspboot/docker)
	$(call docker_run,$(DSPBOOT_IMAGE),$(TOP)/services/dspaudio,\
		make O=$(BUILD)/dspaudio VERSION=$(shell sh tools/version.sh) \
		$(if $(DMA),DMA=$(DMA)) $(if $(AC3),AC3=$(AC3)))

# Buildroot: its own target toolchain, U-Boot, the kernel and the root file
# system in one build; the sources it downloads are kept in buildroot/dl.
# Buildroot copies local sources only once, so this repository's own
# packages are rebuilt every time.

BUILDROOT_IMAGE := $(call image_tag,buildroot,toolchain/buildroot/Dockerfile)
BUILDROOT_OUT   := $(BUILD)/buildroot
BUILDROOT_MAKE  := make O=$(BUILDROOT_OUT)
BUILDROOT_LOCAL := mcud dspd dspaudio osirisd pcmd

buildroot: dspaudio
	@$(call ensure_image,$(BUILDROOT_IMAGE),toolchain/buildroot)
	mkdir -p $(BUILDROOT_OUT)
	$(call docker_run,$(BUILDROOT_IMAGE),$(TOP)/buildroot,sh -c \
		"$(BUILDROOT_MAKE) onkyo_nr_defconfig > /dev/null && \
		 $(BUILDROOT_MAKE) $(addsuffix -rebuild,$(BUILDROOT_LOCAL)) && \
		 $(BUILDROOT_MAKE)")
	mkdir -p $(DIST)
	ln -sfn $(BUILDROOT_OUT)/images $(DIST)/images
	@echo "buildroot -> $(DIST)/images"

clean:
	rm -rf $(DIST) $(BUILD)

.PHONY: help dspboot dspboot-test dspaudio buildroot clean
