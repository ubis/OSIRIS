#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Print "<version> <epoch>" for the dspboot sources in this directory:
#   v1.0                 at the tag dspboot-v1.0
#   v1.0~g4f6ccf8        later dspboot commits (hash of the newest one)
#   ...-dirty            uncommitted changes to tracked dspboot files
# Only commits that touch dspboot count, so OS work elsewhere in the
# repository does not change the version. The epoch (commit time of the
# newest dspboot commit) becomes SOURCE_DATE_EPOCH, which GCC uses for
# __DATE__/__TIME__: the same sources always give the same image.
BASE=1.0
cd "$(dirname "$0")/.." || exit 1
rev=$(git log -1 --format=%h --abbrev=7 -- . 2>/dev/null)
[ -n "$rev" ] || { echo "v$BASE~unknown 0"; exit 0; }
epoch=$(git log -1 --format=%ct -- .)
tag=$(git describe --tags --match 'dspboot-v*' --abbrev=0 "$rev" 2>/dev/null)
if [ -n "$tag" ]; then
	BASE=${tag#dspboot-v}
	n=$(git rev-list --count "$tag..$rev" -- .)
else
	n=1
fi
v="v$BASE"
[ "$n" -gt 0 ] && v="$v~g$rev"
git diff --quiet HEAD -- . 2>/dev/null || v="$v-dirty"
echo "$v $epoch"
