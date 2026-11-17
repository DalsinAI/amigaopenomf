#!/bin/sh
# The release archive, for Cradle's stove_build (target package): OpenOMF and
# its build inputs at their pins (scripts/game_port_prepare.py,
# gameports/catalog.json), built by build-amigaos3.sh with the stove (STOVE=),
# then tools/make_package.py packs openomf-VERSION-68040.lha with its .sha256
# and .readme: the OpenOMF drawer with its GlowIcons (icons/), the program,
# its licence, the ReadMe and resources/ (openomf.bk and the language files
# from OpenOMF's own tree), and beside the drawer the Amiga Installer script
# Install-OpenOMF and its icon (package/installer/, made by amicradle
# scripts/ports/port_installer.py from package/installer.json), which
# fetches the freeware One Must Fall 2097 data into resources/.
# No OMF2097 data is in the package. VERSION is package/VERSION.
# Nothing is published from here.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
OUT=${OUT:-"$ROOT/build-amiga-package"}
VERSION=$(cat "$HERE/VERSION")
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
BUILD="$OUT/build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$ROOT"

python3 scripts/game_port_prepare.py prepare --game all --allow-network > "$OUT/prepare-output.txt" 2>&1 ||
    { tail -n 30 "$OUT/prepare-output.txt"; exit 1; }
sh "$ROOT/build-amigaos3.sh" "" "$BUILD/os3" > "$OUT/build-output.txt" 2>&1 ||
    { tail -n 60 "$OUT/build-output.txt"; exit 1; }
[ "$(od -An -tx1 -N4 "$BUILD/os3/openomf" | tr -d ' \n')" = "000003f3" ] || { echo "openomf is not an Amiga HUNK executable"; exit 1; }
cp "$BUILD/os3/openomf" "$BUILD/openomf"
cp "$BUILD/os3/work/LICENSE" "$BUILD/LICENSE"
# resources/: OpenOMF's own openomf.bk and language files (MIT, its tree)
(cd "$BUILD/os3/work/resources" && tar --format=ustar --owner=0 --group=0 --numeric-owner \
    -cf "$BUILD/resources.tar" openomf.bk ENGLISH2.TXT GERMAN2.TXT DANISH.TXT DANISH2.TXT)

if [ -x "$STOVE/prefix/bin/m68k-amigaos-gcc" ]; then CC="$STOVE/prefix/bin/m68k-amigaos-gcc"; else CC="$STOVE/bin/m68k-amigaos-gcc"; fi
python3 - "$ROOT" "$BUILD" "$CC" <<'PY'
import hashlib, json, subprocess, sys
from pathlib import Path
root, build, cc = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
git = lambda *a: subprocess.run(["git", "-C", str(root), *a], capture_output=True, text=True).stdout.strip()
cat = json.loads((root / "gameports/catalog.json").read_text())
version = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
receipt = {
    "executable_sha256": hashlib.sha256((build / "openomf").read_bytes()).hexdigest(),
    "executable_bytes": (build / "openomf").stat().st_size,
    "upstream_commit": cat["ports"]["openomf"]["source"]["commit"],
    "guest_commit": cat["buildDependencies"]["amicradle-guest"]["commit"],
    "repository_commit": git("rev-parse", "HEAD"),
    "compiler_release": version.split("(")[1].split(")")[0].split()[-1] if "(" in version else version,
    "source_date_epoch": int(git("log", "-1", "--format=%ct") or 0),
}
(build / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt, indent=2))
PY
cp "$BUILD/receipt.json" "$OUT/receipt.json"
python3 "$ROOT/tools/make_package.py" --build "$BUILD" --version "$VERSION" --out "$OUT"
