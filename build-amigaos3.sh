#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$HERE
SRC=${1:-"$ROOT/build/game-ports/sources/openomf"}
OUT=${2:-"$ROOT/build/os3/openomf"}
WORK="$OUT/work"
BUILD="$OUT/build"
GUEST=${GUEST_PORT_LAYER:-"$ROOT/build/game-ports/dependencies/amicradle-guest"}
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16"}
P="$STOVE/prefix"
SYS="$P/m68k-amigaos"
CDOGS_SRC=${CDOGS_SRC:-"$ROOT/build/game-ports/sources/cdogs-sdl"}

CC="$P/bin/m68k-amigaos-gcc"
CXX="$P/bin/m68k-amigaos-g++"

[ -x "$CC" ] || { echo "missing GCC16 compiler: $CC" >&2; exit 2; }
[ -x "$CXX" ] || { echo "missing GCC16 C++ compiler: $CXX" >&2; exit 2; }
[ -d "$SRC" ] || { echo "missing pinned OpenOMF source: $SRC" >&2; exit 2; }
[ -d "$GUEST/gameports/openomf" ] || { echo "missing pinned guest OpenOMF profile: $GUEST" >&2; exit 2; }
[ -d "$GUEST/gameports/acgame" ] || { echo "missing pinned guest ACGame: $GUEST" >&2; exit 2; }
[ -d "$CDOGS_SRC/src/cdogs/enet" ] || { echo "missing pinned C-Dogs ENet source" >&2; exit 2; }

rm -rf "$OUT"
mkdir -p "$OUT"
cp -a "$SRC" "$WORK"

mkdir -p "$WORK/src/vendored"
cp -a "$CDOGS_SRC/src/cdogs/enet" "$WORK/src/vendored/enet"
cp -a "$GUEST/gameports/acgame" "$WORK/src/vendored/acgame"

python3 "$GUEST/gameports/openomf/apply_aga_profile.py" "$WORK" \
  --overlay "$GUEST/gameports/openomf/overlay"
python3 "$HERE/patch_enet_amiga.py" "$WORK/src/vendored/enet"
python3 "$HERE/patch_openomf_amigaos3.py" "$WORK" \
  --stub "$HERE/modmanager_amiga_stub.c"
cp "$HERE/psm_source_amiga_stub.c" "$WORK/src/audio/music_sources/psm_source_amiga_stub.c"
python3 "$HERE/patch_openomf_audio_ahi.py" "$WORK"

# libxmp (MIT) at its pin, static, for the PSM music
XMP_SRC=${XMP_SRC:-"$ROOT/build/game-ports/dependencies/libxmp"}
[ -f "$XMP_SRC/CMakeLists.txt" ] || { echo "missing pinned libxmp: $XMP_SRC" >&2; exit 2; }
cmake -S "$XMP_SRC" -B "$OUT/xmp-build" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain/amigaos3-opengpu.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAMIGA_STOVE="$STOVE" -DAMIGA_SDK="$SYS" \
  -DCMAKE_INSTALL_PREFIX="$OUT/xmp" -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
  -DLIBXMP_DISABLE_DEPACKERS=ON -DLIBXMP_DISABLE_PROWIZARD=ON -DLIBXMP_DOCS=OFF
cmake --build "$OUT/xmp-build" -j${JOBS:-2}
cmake --install "$OUT/xmp-build"

cmake -S "$WORK" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/toolchain/amigaos3-opengpu.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAMIGA_STOVE="$STOVE" \
  -DAMIGAAGA=ON \
  -DAMIGA_SDK="$SYS" \
  -DUSE_TESTS=OFF \
  -DUSE_TOOLS=OFF \
  -DBUILD_LANGUAGES=OFF \
  -DUSE_LIBPNG=OFF \
  -DUSE_OPUSFILE=OFF \
  -DUSE_MINIUPNPC=OFF \
  -DUSE_NATPMP=OFF \
  -DUSE_EXTENDED_PALETTE=OFF \
  -DOPENOMF_XMP_ROOT="$OUT/xmp"

cmake --build "$BUILD" --target openomf -j${JOBS:-2}

BIN=$(find "$BUILD" -type f -name openomf -perm -111 | head -1)
[ -n "$BIN" ] || { echo "OpenOMF binary not found after build" >&2; exit 4; }
cp "$BIN" "$OUT/openomf"

file "$OUT/openomf"
sha256sum "$OUT/openomf"
cat > "$OUT/OPENOMF_RUN.txt" <<EOF
OpenOMF / OMFAGA first-light build
Target: AmigaOS 3.x / AC090 / 68040 + FPU
Compiler: $("$CC" --version | head -1)
Renderer: native ACGame indexed AGA
Audio: NULL first-light backend
ACGame source: DalsinAI/amicradle-guest at pinned campaign commit
Original One Must Fall 2097 data is NOT bundled.
Place user-supplied OMF2097 data under PROGDIR:resources before launch.
EOF
