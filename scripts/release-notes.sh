#!/bin/sh
# release-notes.sh VERSION: print CHANGELOG.md's section for VERSION (the
# lines after "## VERSION ..." up to the next "## "), for a release body.
# Fails if there is no such section, so a release cannot ship without one.
set -eu
v=${1:?usage: release-notes.sh VERSION}
here=$(cd "$(dirname "$0")/.." && pwd)
awk -v v="$v" '
	/^## / { if (p) exit; p = ($2 == v) ; next }
	p { print }
' "$here/CHANGELOG.md" | sed -e '/./,$!d' > "${TMPDIR:-/tmp}/notes.$$"
if [ ! -s "${TMPDIR:-/tmp}/notes.$$" ]; then
	rm -f "${TMPDIR:-/tmp}/notes.$$"
	echo "release-notes: CHANGELOG.md has no '## $v' section" >&2
	exit 1
fi
cat "${TMPDIR:-/tmp}/notes.$$"
rm -f "${TMPDIR:-/tmp}/notes.$$"
