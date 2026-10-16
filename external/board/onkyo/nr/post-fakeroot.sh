#!/bin/sh
# Buildroot post-fakeroot script for the Onkyo receivers: /etc/shadow
# becomes a link to the data volume, in the image only. Buildroot rewrites
# the root entry of target/etc/shadow on every build, so it stays a file
# there.
# $1: the image's copy of the target directory
set -e
rm -f $1/etc/shadow
ln -s /data/etc/shadow $1/etc/shadow
