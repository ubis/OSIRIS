#!/bin/sh
# Buildroot post-image script for the Onkyo receivers: U-Boot as the slot
# image dspboot starts; the upgrade bundle with the kernel, the root file
# system and U-Boot; then the installer's initrd, which carries that
# bundle.
# $1: images directory
set -e
IMAGES=$1
TOP=$(dirname $0)/../../../..
TOOLS=$TOP/dspboot/tools
VERSION=$(sh $TOP/tools/version.sh)
NAME="u-boot $VERSION"

python3 $TOOLS/mkimage.py $IMAGES/u-boot.bin $IMAGES/u-boot.dbi --name "$NAME" > /dev/null

[ -f $IMAGES/zImage ] || exit 0

# the kernel with its device tree appended, as a uImage: an older U-Boot
# boots it too, with ATAGs, on the first start after an update
cat $IMAGES/zImage $IMAGES/da830-onkyo-nr.dtb > $BASE_DIR/build/zImage-dtb
$HOST_DIR/bin/mkimage -A arm -O linux -T kernel -C none -a 0xc0008000 -e 0xc0008000 \
	-n "Linux OSIRIS $VERSION" -d $BASE_DIR/build/zImage-dtb $IMAGES/uImage > /dev/null

# older systems created the slot volumes with these sizes, and updates
# from them write into them
fits() {
	[ $(wc -c < $1) -le $2 ] || { echo "${1##*/} does not fit $3 ($2 bytes)" >&2; exit 1; }
}
fits $IMAGES/uImage 4208640 kernel_a/b
fits $IMAGES/rootfs.squashfs 12595200 rootfs_a/b

rm -f $IMAGES/osiris-*.tar
sh $TOP/tools/mkbundle.sh -k $IMAGES/uImage -r $IMAGES/rootfs.squashfs -u $IMAGES/u-boot.dbi \
	-v $VERSION -o $IMAGES/osiris-$VERSION.tar

# the installer's initrd: this root file system with the bundle
# (/usr/share/osiris/osiris.tar for osiris-install), to boot over TFTP with
# the same kernel for the first install: bootm <uImage> <initrd-installer>.
WORK=$BASE_DIR/build/installer
rm -rf $WORK
mkdir -p $WORK/root/usr/share/osiris
cp $IMAGES/osiris-$VERSION.tar $WORK/root/usr/share/osiris/osiris.tar
(cd $WORK/root && echo usr/share/osiris/osiris.tar |
	cpio -o -H newc -R 0:0 --quiet) > $WORK/bundle.cpio
cat $IMAGES/rootfs.cpio $WORK/bundle.cpio | gzip -1 > $WORK/installer.cpio.gz
$HOST_DIR/bin/mkimage -A arm -O linux -T ramdisk -C gzip -n "OSIRIS $VERSION installer" \
	-d $WORK/installer.cpio.gz $IMAGES/initrd-installer > /dev/null
