#!/bin/sh
# Buildroot post-build: what the root file system needs besides the
# packages and the overlay.
# $1: target directory
set -e
TARGET=$1
TOP=$(dirname $0)/../../../..
# root password "osiris"
ROOT_HASH='$1$OSIRISv1$PF45S2vBsv1PTcWryHdqr.'

# ssh keys and passwords live on /data, as the root file system is
# read-only; 20_data starts /data/etc/shadow from the copy made here and
# post-fakeroot.sh links /etc/shadow to it
data_links() {
	install -d $TARGET/data
	rm -rf $TARGET/etc/dropbear $TARGET/root/.ssh
	ln -s /data/etc/dropbear $TARGET/etc/dropbear
	ln -s /data/root/.ssh $TARGET/root/.ssh
	sed "s,^root:[^:]*:,root:$ROOT_HASH:," $TARGET/etc/shadow \
		> $TARGET/usr/share/osiris/shadow
	chmod 600 $TARGET/usr/share/osiris/shadow
}

# /var is /tmp, /run is /tmp/run (10_run)
tmp_links() {
	for d in var run; do
		if [ -n "$(find $TARGET/$d -mindepth 1 ! -type d ! -type l 2> /dev/null)" ]; then
			echo "post-build: files in /$d would be lost" >&2
			exit 1
		fi
	done
	rm -rf $TARGET/var $TARGET/run
	ln -s tmp $TARGET/var
	ln -s tmp/run $TARGET/run
	rmdir $TARGET/mnt $TARGET/opt 2> /dev/null || true
}

# start and stop links, as "/etc/init.d/<name> enable" makes them
service_links() {
	rm -f $TARGET/etc/rc.d/*
	for s in $TARGET/etc/init.d/*; do
		n=${s##*/}
		start=$(sed -n 's/^START=//p' $s)
		stop=$(sed -n 's/^STOP=//p' $s)
		[ -z "$start" ] || ln -s ../init.d/$n $TARGET/etc/rc.d/S$start$n
		[ -z "$stop" ] || ln -s ../init.d/$n $TARGET/etc/rc.d/K$stop$n
	done
}

# os-release, whose time 40_clock starts the clock from
os_release() {
	v=$(sh $TOP/tools/version.sh)
	cat > $TARGET/usr/lib/os-release <<EOF
NAME=OSIRIS
ID=osiris
VERSION="$v"
VERSION_ID=$(echo $v | sed 's/^v//; s/[^0-9a-z._-].*//')
PRETTY_NAME="OSIRIS $v"
HOME_URL="https://github.com/ubis/osiris"
EOF
}

data_links
tmp_links
service_links
os_release
