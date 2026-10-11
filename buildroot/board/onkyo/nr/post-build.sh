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
