#!/bin/sh
# Buildroot post-build script for the Onkyo receivers.
# $1: target directory
set -e
TARGET=$1
# The root filesystem is read-only (squashfs); persistent files live on the
# data volume mounted on /data (S01data). Keep the ssh host keys there so
# they survive reboots and updates.
install -d $TARGET/data
rm -rf $TARGET/etc/dropbear
ln -s /data/etc/dropbear $TARGET/etc/dropbear
# authorized_keys too (ssh-copy-id), for logins without the password
rm -rf $TARGET/root/.ssh
ln -s /data/root/.ssh $TARGET/root/.ssh
# and the passwords, so that passwd can change them: S01data starts
# /data/etc/shadow from this default, root password "osiris";
# post-fakeroot.sh links /etc/shadow there in the images
ROOT_HASH='$1$OSIRISv1$PF45S2vBsv1PTcWryHdqr.'
sed "s,^root:[^:]*:,root:$ROOT_HASH:," $TARGET/etc/shadow > $TARGET/usr/share/osiris/shadow
chmod 600 $TARGET/usr/share/osiris/shadow

# S50airplay starts AirPlay after the network
rm -f $TARGET/etc/init.d/S99shairport-sync

# which build this is (osiris-upgrade shows it)
sh $(dirname $0)/../../../../tools/version.sh > $TARGET/etc/osiris-version
# the clock starts from here until NTP syncs (S02clock)
echo ${SOURCE_DATE_EPOCH:-$(date +%s)} > $TARGET/etc/osiris-build-time
