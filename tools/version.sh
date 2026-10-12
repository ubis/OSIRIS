#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Print the OSIRIS version of this repository, in the same style as
# dspboot's (dspboot/tools/version.sh):
#   v0.1.0               at the tag osiris-v0.1.0
#   v0.1.0~g1de0787      later commits (hash of the newest one)
#   1de0787              before the first osiris-v* tag
#   ...-dirty            uncommitted changes
cd "$(dirname "$0")/.." || exit 1
git describe --always --dirty --abbrev=7 --match 'osiris-v[0-9]*' 2>/dev/null |
	sed 's/^osiris-//; s/-[0-9]*-g\([0-9a-f]*\)/~g\1/'
