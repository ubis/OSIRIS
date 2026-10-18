#!/bin/sh
# Buildroot post-image: U-Boot for dspboot, the kernel as a FIT and a
# uImage, the update bundle and the installer's initrd.
# $1: images directory
set -e
IMAGES=$1
TOP=$(dirname $0)/../../../..
BUILD=$BASE_DIR/build
MKIMAGE=$HOST_DIR/bin/mkimage
VERSION=$(sh $TOP/tools/version.sh)

# U-Boot with dspboot's image header
uboot_image() {
	python3 $TOP/dspboot/tools/mkimage.py $IMAGES/u-boot.bin $IMAGES/u-boot.dbi \
		--name "u-boot $VERSION" > /dev/null
}

# uImage with the device tree appended, which an older U-Boot boots on the
# first start after an update
kernel_uimage() {
	cat $IMAGES/zImage $IMAGES/da830-onkyo-nr.dtb > $BUILD/zImage-dtb
	$MKIMAGE -A arm -O linux -T kernel -C none -a 0xc0008000 -e 0xc0008000 \
		-n "Linux OSIRIS $VERSION" -d $BUILD/zImage-dtb $IMAGES/uImage > /dev/null
}

# FIT with a configuration per model, as "board" names it, all with the
# same device tree for now; the zImage runs where it was loaded
# (kernel_noload)
kernel_fit() {
	cat > $BUILD/kernel.its <<EOF
/dts-v1/;
/ {
	description = "Linux OSIRIS $VERSION";
	#address-cells = <1>;
	images {
		kernel {
			description = "Linux";
			data = /incbin/("$IMAGES/zImage");
			type = "kernel_noload";
			arch = "arm";
			os = "linux";
			compression = "none";
			load = <0>;
			entry = <0>;
		};
		fdt-onkyo-nr {
			description = "Onkyo DA830/OMAP-L137 DSP board";
			data = /incbin/("$IMAGES/da830-onkyo-nr.dtb");
			type = "flat_dt";
			arch = "arm";
			compression = "none";
		};
	};
	configurations {
		default = "onkyo-nr";
		onkyo-nr {
			kernel = "kernel";
			fdt = "fdt-onkyo-nr";
		};
		tx-nr515 {
			kernel = "kernel";
			fdt = "fdt-onkyo-nr";
		};
		tx-nr717 {
			kernel = "kernel";
			fdt = "fdt-onkyo-nr";
		};
	};
};
EOF
	$MKIMAGE -f $BUILD/kernel.its $IMAGES/kernel.itb > /dev/null
}

# the slot volume sizes older systems created, which updates from them
# write into
check_sizes() {
	fits $IMAGES/uImage 4208640 kernel_a/b
	fits $IMAGES/rootfs.squashfs 12595200 rootfs_a/b
}

fits() {
	[ $(wc -c < $1) -le $2 ] || { echo "${1##*/} does not fit $3 ($2 bytes)" >&2; exit 1; }
}

# update bundle
bundle() {
	rm -f $IMAGES/osiris-*.tar
	sh $TOP/tools/mkbundle.sh -k $IMAGES/uImage -f $IMAGES/kernel.itb \
		-r $IMAGES/rootfs.squashfs -u $IMAGES/u-boot.dbi \
		-v $VERSION -o $IMAGES/osiris-$VERSION.tar
}

# installer initrd: the root file system and the unpacked bundle, which
# osiris-install writes from RAM; no uImage, the U-Boot it installs boots
# the FIT
installer() {
	work=$BUILD/installer
	rm -rf $work
	mkdir -p $work/root/usr/share/osiris/bundle
	sh $TOP/tools/mkbundle.sh -f $IMAGES/kernel.itb -r $IMAGES/rootfs.squashfs \
		-u $IMAGES/u-boot.dbi -v $VERSION -o $work/bundle.tar
	tar -xf $work/bundle.tar -C $work/root/usr/share/osiris/bundle
	(cd $work/root && find usr/share/osiris |
		cpio -o -H newc -R 0:0 --quiet) > $work/bundle.cpio
	cat $IMAGES/rootfs.cpio $work/bundle.cpio | gzip -1 > $work/installer.cpio.gz
	$MKIMAGE -A arm -O linux -T ramdisk -C gzip -n "OSIRIS $VERSION installer" \
		-d $work/installer.cpio.gz $IMAGES/initrd-installer > /dev/null
}

uboot_image
# U-Boot-only builds stop here
[ -f $IMAGES/zImage ] || exit 0
kernel_uimage
kernel_fit
check_sizes
bundle
installer
