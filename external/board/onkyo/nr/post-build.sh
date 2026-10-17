#!/bin/sh
# Buildroot post-build script for the Onkyo receivers.
# $1: target directory
set -e
TARGET=$1
# The root filesystem is read-only (squashfs); persistent files live on the
# data volume mounted on /data (/lib/preinit/20_data). Keep the ssh host keys there so
# they survive reboots and updates.
install -d $TARGET/data
rm -rf $TARGET/etc/dropbear
ln -s /data/etc/dropbear $TARGET/etc/dropbear
# authorized_keys too (ssh-copy-id), for logins without the password
rm -rf $TARGET/root/.ssh
ln -s /data/root/.ssh $TARGET/root/.ssh
# and the passwords, so that passwd can change them: 20_data starts
# /data/etc/shadow from this default, root password "osiris";
# post-fakeroot.sh links /etc/shadow there in the images
ROOT_HASH='$1$OSIRISv1$PF45S2vBsv1PTcWryHdqr.'
sed "s,^root:[^:]*:,root:$ROOT_HASH:," $TARGET/etc/shadow > $TARGET/usr/share/osiris/shadow
chmod 600 $TARGET/usr/share/osiris/shadow

# the start and stop links of the services, as "/etc/init.d/<name> enable"
# makes them
rm -f $TARGET/etc/rc.d/*
for s in $TARGET/etc/init.d/*; do
	n=${s##*/}
	start=$(sed -n 's/^START=//p' $s)
	stop=$(sed -n 's/^STOP=//p' $s)
	[ -z "$start" ] || ln -s ../init.d/$n $TARGET/etc/rc.d/S$start$n
	[ -z "$stop" ] || ln -s ../init.d/$n $TARGET/etc/rc.d/K$stop$n
done

# os-release names this build; its time is where /lib/preinit/40_clock
# starts the clock
v=$(sh $(dirname $0)/../../../../tools/version.sh)
cat > $TARGET/usr/lib/os-release <<EOF
NAME=OSIRIS
ID=osiris
VERSION="$v"
VERSION_ID=$(echo $v | sed 's/^v//; s/[^0-9a-z._-].*//')
PRETTY_NAME="OSIRIS $v"
HOME_URL="https://github.com/ubis/osiris"
EOF
