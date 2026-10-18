#!/bin/sh
# Buildroot post-fakeroot: /etc/shadow becomes a link to /data in the image
# only, as Buildroot rewrites target/etc/shadow on every build
# $1: the image's copy of the target directory
set -e
rm -f $1/etc/shadow
ln -s /data/etc/shadow $1/etc/shadow
