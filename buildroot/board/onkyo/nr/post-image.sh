#!/bin/sh
# Buildroot post-image script for the Onkyo receivers: U-Boot as the slot
# image dspboot starts, and that as NAND page + spare records with the boot
# ROM ECC for nandrestore.py, for 512 B, 2 KiB and 4 KiB pages; the
# installer kernel; then the upgrade bundle with the kernel, the root file
# system and U-Boot.
# $1: images directory
set -e
IMAGES=$1
TOP=$(dirname $0)/../../../..
TOOLS=$TOP/dspboot/tools
VERSION=$(git -C $TOP describe --always --dirty)
NAME="u-boot $VERSION"

python3 $TOOLS/mkimage.py $IMAGES/u-boot.bin $IMAGES/u-boot.dbi --name "$NAME" > /dev/null
for p in 512:512b 2048:2k 4096:4k; do
	python3 $TOOLS/mknandimg.py $IMAGES/u-boot.dbi --page ${p%%:*} --pad-block \
		-o $IMAGES/u-boot-${p##*:}.nand > /dev/null
done

# the installer: the same kernel with this root file system built in, to
# boot over TFTP for the first install (osiris-install)
KOUT=$BASE_DIR/build/linux-installer
mkdir -p $KOUT
sed "s|^CONFIG_INITRAMFS_SOURCE=.*|CONFIG_INITRAMFS_SOURCE=\"$IMAGES/rootfs.cpio\"|" \
	$BASE_DIR/build/linux-custom/.config > $KOUT/.config
printf "CONFIG_INITRAMFS_ROOT_UID=0\nCONFIG_INITRAMFS_ROOT_GID=0\n" >> $KOUT/.config
KMAKE="make -C $TOP/linux O=$KOUT ARCH=arm CROSS_COMPILE=$HOST_DIR/usr/bin/arm-buildroot-linux-uclibcgnueabi-"
yes "" | PATH=$HOST_DIR/usr/bin:$PATH $KMAKE oldconfig > /dev/null
PATH=$HOST_DIR/usr/bin:$PATH $KMAKE -j$(nproc) uImage > $KOUT/build.log 2>&1 ||
	{ tail -20 $KOUT/build.log; exit 1; }
cp $KOUT/arch/arm/boot/uImage $IMAGES/uImage-installer

rm -f $IMAGES/osiris-*.tar
sh $TOP/tools/mkbundle.sh -k $IMAGES/uImage -r $IMAGES/rootfs.squashfs -u $IMAGES/u-boot.dbi \
	-v $VERSION -o $IMAGES/osiris-$VERSION.tar
