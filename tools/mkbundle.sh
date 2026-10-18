#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# mkbundle.sh: pack an OSIRIS update bundle for osiris-upgrade.
#
#   tools/mkbundle.sh -f kernel.itb -r rootfs.squashfs [-u u-boot.dbi]
#                     [-v version] -o bundle.tar
#
# The version defaults to tools/version.sh.
#
set -e

usage() {
	echo "usage: $0 -f kernel.itb -r rootfs.squashfs [-u u-boot.dbi] [-v version] -o bundle.tar" >&2
	exit 2
}

fit= rootfs= uboot= version= out=
while getopts f:r:u:v:o: opt; do
	case $opt in
	f) fit=$OPTARG ;;
	r) rootfs=$OPTARG ;;
	u) uboot=$OPTARG ;;
	v) version=$OPTARG ;;
	o) out=$OPTARG ;;
	*) usage ;;
	esac
done
[ -n "$fit" ] && [ -n "$rootfs" ] && [ -n "$out" ] || usage
[ -n "$version" ] || version=$(sh "$(dirname "$0")/version.sh")

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$fit" "$tmp/kernel.itb"
cp "$rootfs" "$tmp/rootfs.squashfs"
files="VERSION kernel.itb rootfs.squashfs"
if [ -n "$uboot" ]; then
	cp "$uboot" "$tmp/u-boot.dbi"
	files="$files u-boot.dbi"
fi
echo "$version" > "$tmp/VERSION"
(cd "$tmp" && md5sum $files > md5sums)
tar --format=ustar -C "$tmp" -cf "$out" $files md5sums
echo "$out: OSIRIS $version ($files)"
