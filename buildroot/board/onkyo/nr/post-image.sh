#!/bin/sh
# Buildroot post-image script for the Onkyo receivers: U-Boot as the slot
# image dspboot starts, and that as NAND page + spare records with the boot
# ROM ECC for nandrestore.py, for 512 B, 2 KiB and 4 KiB pages.
# $1: images directory
set -e
IMAGES=$1
TOOLS=$(dirname $0)/../../../../dspboot/tools
NAME="u-boot $(git -C $(dirname $0) describe --always --dirty)"

python3 $TOOLS/mkimage.py $IMAGES/u-boot.bin $IMAGES/u-boot.dbi --name "$NAME" > /dev/null
for p in 512:512b 2048:2k 4096:4k; do
	python3 $TOOLS/mknandimg.py $IMAGES/u-boot.dbi --page ${p%%:*} --pad-block \
		-o $IMAGES/u-boot-${p##*:}.nand > /dev/null
done
