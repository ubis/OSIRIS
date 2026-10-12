#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Print the OSIRIS version of this repository:
#   v0.1                 at the tag osiris-v0.1
#   v0.1-3-g1de0787      later commits
#   1de0787              before the first osiris-v* tag
#   ...-dirty            uncommitted changes
# The receiver shows "OSIRIS <version>" in 14 characters, so the tag
# prefix is left out. dspboot has its own version (dspboot/tools/version.sh).
cd "$(dirname "$0")/.." || exit 1
v=$(git describe --always --dirty --match 'osiris-v[0-9]*' 2>/dev/null)
echo "${v#osiris-}"
