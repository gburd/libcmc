#!/bin/sh
# codeberg-release.sh VERSION: publish the Codeberg release for vVERSION.
#
# Codeberg Actions are disabled for this repository, so the forgejo
# release workflow (.forgejo/workflows/release.yml) does not run and the
# Codeberg release is not created automatically.  This script does it
# through the Codeberg API instead: it creates the release from the
# CHANGELOG section for VERSION, and attaches the same source tarball and
# man-page tarball that the GitHub release workflow built (the canonical
# artifacts), fetched from the GitHub release.
#
# Run it after the GitHub release for the tag has published (the tag push
# triggers .github/workflows/release.yml).  It is idempotent: if the
# Codeberg release already exists it only fills in any missing assets.
#
# The token is the Codeberg API token in ~/.config/tea/config.yml, the
# same one `tea` uses; override with $CODEBERG_TOKEN.
#
#   scripts/codeberg-release.sh 1.0.0
set -eu
VER=${1:?usage: codeberg-release.sh VERSION}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OWNER=gregburd
REPO=libcmc
GH=gburd/libcmc
API=https://codeberg.org/api/v1/repos/$OWNER/$REPO
TOK=${CODEBERG_TOKEN:-$(grep -m1 'token:' "$HOME/.config/tea/config.yml" | awk '{print $2}')}
[ -n "$TOK" ] || { echo "codeberg-release: no token (set CODEBERG_TOKEN or ~/.config/tea/config.yml)" >&2; exit 1; }
AUTH="Authorization: token $TOK"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/cbrel.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

notes="$WORK/notes.md"
sh "$HERE/scripts/release-notes.sh" "$VER" > "$notes"

# create the release if it does not exist; capture its id either way
existing=$(curl -fsS -H "$AUTH" "$API/releases/tags/v$VER" 2>/dev/null || true)
relid=$(printf '%s' "$existing" | grep -oE '"id":[0-9]+' | head -1 | grep -oE '[0-9]+' || true)
if [ -z "$relid" ]; then
	# build the JSON payload with the notes body safely quoted
	python3 - "$notes" "$VER" > "$WORK/payload.json" <<'PY'
import json, sys
body = open(sys.argv[1]).read()
print(json.dumps({"tag_name": "v"+sys.argv[2], "name": "libcmc "+sys.argv[2],
                  "body": body, "draft": False, "prerelease": False}))
PY
	resp=$(curl -fsS -H "$AUTH" -H "Content-Type: application/json" \
		--data @"$WORK/payload.json" "$API/releases")
	relid=$(printf '%s' "$resp" | grep -oE '"id":[0-9]+' | head -1 | grep -oE '[0-9]+')
	echo "codeberg-release: created v$VER (id $relid)"
else
	echo "codeberg-release: v$VER already exists (id $relid); filling assets"
fi
[ -n "$relid" ] || { echo "codeberg-release: could not determine release id" >&2; exit 1; }

# which assets are already attached, so we do not duplicate
attached=$(curl -fsS -H "$AUTH" "$API/releases/$relid/assets" 2>/dev/null || echo '[]')

for a in "libcmc-$VER.tar.gz" "libcmc-man-$VER.tar.gz"; do
	if printf '%s' "$attached" | grep -q "\"name\":\"$a\""; then
		echo "codeberg-release: $a already attached"
		continue
	fi
	url="https://github.com/$GH/releases/download/v$VER/$a"
	if ! curl -fsSL -o "$WORK/$a" "$url"; then
		echo "codeberg-release: could not fetch $url (is the GitHub release published?)" >&2
		exit 1
	fi
	code=$(curl -fsS -o /dev/null -w '%{http_code}' -H "$AUTH" \
		-F "attachment=@$WORK/$a" "$API/releases/$relid/assets?name=$a")
	echo "codeberg-release: attached $a (HTTP $code)"
done
echo "codeberg-release: v$VER published at https://codeberg.org/$OWNER/$REPO/releases/tag/v$VER"
