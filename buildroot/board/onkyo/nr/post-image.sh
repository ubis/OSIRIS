#!/bin/sh
# Buildroot post-image script for the Onkyo receivers: U-Boot as the slot
# image dspboot starts, and that as NAND page + spare records with the boot
# ROM ECC for nandrestore.py, for 512 B, 2 KiB and 4 KiB pages; then the
# upgrade bundle with the kernel, the root file system and U-Boot.
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

rm -f $IMAGES/osiris-*.tar
sh $TOP/tools/mkbundle.sh -k $IMAGES/uImage -r $IMAGES/rootfs.squashfs -u $IMAGES/u-boot.dbi \
	-v $VERSION -o $IMAGES/osiris-$VERSION.tar
