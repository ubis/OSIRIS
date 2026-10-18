# helpers for the OSIRIS shell scripts

die() {
	echo "${0##*/}: $*" >&2
	exit 1
}

boot_slot() {
	sed -n 's/.*osiris\.slot=\([ab]\).*/\1/p' /proc/cmdline
}

mtd_index() {
	sed -n "s/^mtd\([0-9]*\): .* \"$1\"\$/\1/p" /proc/mtd
}

md5() {
	md5sum | cut -d' ' -f1
}

ubi_info() {
	ubinfo -d 0 ${2:+-N $2} 2> /dev/null | sed -n "s/^$1: *\([0-9]*\).*/\1/p"
}
