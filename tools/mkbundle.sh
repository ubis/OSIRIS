#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# mkbundle.sh: pack an OSIRIS update bundle for osiris-upgrade.
#
#   tools/mkbundle.sh -k uImage -r rootfs.squashfs [-u u-boot.dbi] [-v version] -o bundle.tar
#
# The version defaults to `git describe` of this repository.
#
set -e

usage() {
	echo "usage: $0 -k uImage -r rootfs.squashfs [-u u-boot.dbi] [-v version] -o bundle.tar" >&2
	exit 2
}

kernel= rootfs= uboot= version= out=
while getopts k:r:u:v:o: opt; do
	case $opt in
	k) kernel=$OPTARG ;;
	r) rootfs=$OPTARG ;;
	u) uboot=$OPTARG ;;
	v) version=$OPTARG ;;
	o) out=$OPTARG ;;
	*) usage ;;
	esac
done
[ -n "$kernel" ] && [ -n "$rootfs" ] && [ -n "$out" ] || usage
[ -n "$version" ] || version=$(git -C "$(dirname "$0")" describe --always --dirty)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$kernel" "$tmp/uImage"
cp "$rootfs" "$tmp/rootfs.squashfs"
files="VERSION uImage rootfs.squashfs"
if [ -n "$uboot" ]; then
	cp "$uboot" "$tmp/u-boot.dbi"
	files="$files u-boot.dbi"
fi
echo "$version" > "$tmp/VERSION"
(cd "$tmp" && md5sum $files > md5sums)
tar --format=ustar -C "$tmp" -cf "$out" $files md5sums
echo "$out: OSIRIS $version ($files)"
