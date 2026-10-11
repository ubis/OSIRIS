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

# which build this is (osiris-upgrade shows it)
git -C $(dirname $0) describe --always --dirty > $TARGET/etc/osiris-version
# the clock starts from here when nothing newer is known (S02clock)
echo ${SOURCE_DATE_EPOCH:-$(date +%s)} > $TARGET/etc/osiris-build-time
