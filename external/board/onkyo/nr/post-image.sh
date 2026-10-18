#!/bin/sh
# Buildroot post-image: U-Boot for dspboot, the kernel as a FIT, the update
# bundle and the installer's initrd.
# $1: images directory
set -e
IMAGES=$1
BOARD=$(dirname $0)
TOP=$BOARD/../../../..
BUILD=$BASE_DIR/build
MKIMAGE=$HOST_DIR/bin/mkimage
VERSION=$(sh $TOP/tools/version.sh)

# U-Boot with dspboot's image header
uboot_image() {
	python3 $TOP/dspboot/tools/mkimage.py $IMAGES/u-boot.bin $IMAGES/u-boot.dbi \
		--name "u-boot $VERSION" > /dev/null
}

# the kernel FIT, from kernel.its
kernel_fit() {
	sed "s|@IMAGES@|$IMAGES|g; s|@VERSION@|$VERSION|g" $BOARD/kernel.its > $BUILD/kernel.its
	$MKIMAGE -f $BUILD/kernel.its $IMAGES/kernel.itb > /dev/null
}

# update bundle
bundle() {
	rm -f $IMAGES/osiris-*.tar
	sh $TOP/tools/mkbundle.sh -f $IMAGES/kernel.itb -r $IMAGES/rootfs.squashfs \
		-u $IMAGES/u-boot.dbi -v $VERSION -o $IMAGES/osiris-$VERSION.tar
}

# installer initrd: the root file system and the unpacked bundle, which
# osiris-install writes from RAM
installer() {
	work=$BUILD/installer
	rm -rf $work
	mkdir -p $work/root/usr/share/osiris/bundle
	tar -xf $IMAGES/osiris-$VERSION.tar -C $work/root/usr/share/osiris/bundle
	(cd $work/root && find usr/share/osiris |
		cpio -o -H newc -R 0:0 --quiet) > $work/bundle.cpio
	cat $IMAGES/rootfs.cpio $work/bundle.cpio | gzip -1 > $work/installer.cpio.gz
	$MKIMAGE -A arm -O linux -T ramdisk -C gzip -n "OSIRIS $VERSION installer" \
		-d $work/installer.cpio.gz $IMAGES/initrd-installer > /dev/null
}

uboot_image
# U-Boot-only builds stop here
[ -f $IMAGES/zImage ] || exit 0
kernel_fit
bundle
installer
