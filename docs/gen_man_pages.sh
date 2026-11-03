#!/bin/sh
# docs/gen_man_pages.sh: render man/man{1,3,7}/* to Jekyll pages under
# docs/reference/man/ with mandoc.  The Pages workflows run it before the
# Jekyll build; the output is generated and gitignored.
set -eu

here=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(unset CDPATH; cd -- "$here/.." && pwd)
OUT="$here/reference/man"

if ! command -v mandoc > /dev/null 2>&1; then
	echo "gen_man_pages: mandoc not found" >&2
	exit 1
fi
mkdir -p "$OUT"

order=1
for f in "$ROOT"/man/man1/*.1 "$ROOT"/man/man3/*.3 "$ROOT"/man/man7/*.7; do
	[ -f "$f" ] || continue
	base=$(basename "$f")
	name=${base%.*}
	sec=${base##*.}
	slug="$name.$sec"
	nd=$(grep -m1 '^\.Nd ' "$f" | sed 's/^\.Nd //')
	{
		echo '---'
		echo "title: $name($sec)"
		echo 'parent: Reference'
		echo "nav_order: $((order + 1))"
		echo "permalink: /reference/man/$slug/"
		echo "lede: \"$nd\""
		echo '---'
		echo
		echo '{% raw %}<div class="manpage" markdown="0">'
		# mandoc links NAME(SEC) to ../NAME.SEC/: right for our own pages;
		# send the libc ones to man.openbsd.org
		mandoc -T html -O fragment,man='../%N.%S/' "$f" |
			sed -E 's#href="\.\./(printf)\.([0-9])/"#href="https://man.openbsd.org/\1.\2"#g'
		echo '</div>{% endraw %}'
		echo
		echo "[mdoc source: \`man/man$sec/$base\`](https://codeberg.org/gregburd/libcmc/src/branch/main/man/man$sec/$base){: .fs-2 }"
	} > "$OUT/$slug.md"
	order=$((order + 1))
done
echo "gen_man_pages: wrote $((order - 1)) pages under $OUT"
