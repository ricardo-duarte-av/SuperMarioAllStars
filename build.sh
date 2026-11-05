#!/usr/bin/env bash
# Builds the Super Mario All-Stars port for Linux (Release) with CMake, then
# packs it into an AppImage, with your ROM inside it when one is found.
#
#   ./build.sh                      build, and the AppImage (ROM auto-detected)
#   ./build.sh --rom path/to.sfc    ... with this ROM in the AppImage
#   ./build.sh --no-rom             ... an AppImage without a ROM (it asks for one)
#   ./build.sh --no-appimage        only the program (build_linux/SuperMarioAllStars)
#   ./build.sh --clean              start from an empty build folder
#
# Needs: cmake (3.20+), a C++17 compiler, SDL2 development files (SDL2-devel,
# sdl2-compat-devel or libsdl2-dev) and OpenGL ES 3 headers (mesa-libGLES-devel
# or libgles-dev). The AppImage step downloads appimagetool the first time.
#
# The ROM (Super Mario All-Stars (USA).sfc, CRC32 925637C7) is looked for, when
# --rom isn't given, in this folder, a "roms" folder here and ~/Downloads.
# An AppImage with the ROM in it is for your own use: don't share it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build_linux"
DIST="$ROOT/dist"
APPDIR="$BUILD/AppDir"
ROM=""
WANT_ROM=1
WANT_APPIMAGE=1
CLEAN=0
KNOWN_CRC="925637c7"

while [ $# -gt 0 ]; do
  case "$1" in
    --rom) ROM="${2:?--rom needs a file}"; shift ;;
    --rom=*) ROM="${1#--rom=}" ;;
    --no-rom) WANT_ROM=0 ;;
    --no-appimage) WANT_APPIMAGE=0 ;;
    --clean) CLEAN=1 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1 (see --help)" >&2; exit 1 ;;
  esac
  shift
done

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

ARCH="$(uname -m)"
JOBS="$(nproc 2>/dev/null || echo 4)"

# ---- requirements ------------------------------------------------------------
command -v cmake >/dev/null || die "cmake not found (install cmake)"
command -v c++ >/dev/null || command -v g++ >/dev/null || command -v clang++ >/dev/null ||
  die "no C++ compiler found (install gcc-c++ / g++)"
if command -v pkg-config >/dev/null; then
  pkg-config --exists sdl2 ||
    die "SDL2 development files not found (Fedora: sdl2-compat-devel, Debian/Ubuntu: libsdl2-dev, Arch: sdl2)"
fi
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)

# ---- the program -------------------------------------------------------------
[ "$CLEAN" = 1 ] && rm -rf "$BUILD"
say "Configuring"
cmake -S "$ROOT" -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release
say "Building"
cmake --build "$BUILD" --parallel "$JOBS"
BIN="$BUILD/SuperMarioAllStars"
[ -x "$BIN" ] || die "the build produced no $BIN"

# The UI fonts beside the program (stb_truetype reads them; without them it
# asks fontconfig each start): the system's sans-serif, semibold, and DejaVu
# Sans for the arrows and symbols, and a font with the circled letters.
say "Copying the UI fonts"
mkdir -p "$BUILD/fonts"
font_of() { command -v fc-match >/dev/null && fc-match -f '%{file}' "$1" 2>/dev/null || true; }
copy_font() {  # pattern, name, fallback paths...
  local pat="$1" name="$2" f
  shift 2
  for f in "$(font_of "$pat")" "$@"; do
    case "$f" in *.ttf|*.otf|*.TTF|*.OTF) ;; *) continue ;; esac  # (stb_truetype: not .ttc / .pcf / woff)
    if [ -f "$f" ]; then
      cp -f "$f" "$BUILD/fonts/$name"
      return 0
    fi
  done
  warn "no font for '$pat' found: the system's is used at run time"
}
copy_font "sans-serif" ui-regular.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
  /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf
copy_font "sans-serif:semibold" ui-bold.ttf "$(font_of 'sans-serif:bold')" \
  /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf /usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf \
  /usr/share/fonts/TTF/DejaVuSans-Bold.ttf
copy_font "DejaVu Sans" ui-fallback.ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
  /usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf
# (the circled button letters, Ⓐ Ⓑ: whichever font has them; Debian/Ubuntu: fonts-noto-core)
copy_font "sans-serif:charset=24b6" ui-symbols.ttf

echo
echo "Built: $BIN"
echo "  (run it from there; settings and saves go to \${XDG_DATA_HOME:-~/.local/share}/SuperMarioAllStars,"
echo "   or beside it if a file named 'portable' is there)"
[ "$WANT_APPIMAGE" = 1 ] || exit 0

# ---- the ROM -----------------------------------------------------------------
crc_of() {
  if command -v python3 >/dev/null; then
    python3 -I -c 'import sys,zlib
d=open(sys.argv[1],"rb").read()
if len(d)&0x3ff==0x200: d=d[0x200:]
print("%08x"%(zlib.crc32(d)&0xffffffff))' "$1"
  elif command -v crc32 >/dev/null; then
    crc32 "$1"
  fi
}
if [ "$WANT_ROM" = 1 ] && [ -z "$ROM" ]; then
  for d in "$ROOT" "$ROOT/roms" "$HOME/Downloads" "$(xdg-user-dir DOWNLOAD 2>/dev/null || true)"; do
    [ -d "$d" ] || continue
    while IFS= read -r -d '' f; do
      if [ "$(crc_of "$f")" = "$KNOWN_CRC" ]; then ROM="$f"; break 2; fi
    done < <(find "$d" -maxdepth 1 -type f \( -iname '*.sfc' -o -iname '*.smc' \) -print0 2>/dev/null)
  done
fi
if [ "$WANT_ROM" = 1 ]; then
  if [ -n "$ROM" ]; then
    [ -f "$ROM" ] || die "ROM not found: $ROM"
    crc="$(crc_of "$ROM")"
    if [ -n "$crc" ] && [ "$crc" != "$KNOWN_CRC" ]; then
      warn "$ROM has CRC32 $crc, not $KNOWN_CRC (the USA version): it may still run, less well tuned"
    fi
  else
    warn "no ROM found (use --rom FILE): the AppImage asks for one when it starts"
  fi
else
  ROM=""
fi

# ---- the AppImage -------------------------------------------------------------
say "Assembling the AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/fonts" "$APPDIR/usr/lib" "$APPDIR/usr/share/applications" \
  "$APPDIR/usr/share/icons/hicolor/192x192/apps"
cp "$BIN" "$APPDIR/usr/bin/"
cp "$ROOT/third_party/fonts/LuckiestGuy-Regular.ttf" "$APPDIR/usr/bin/"
cp "$ROOT/release/README.txt" "$APPDIR/usr/bin/" 2>/dev/null || true
cp "$BUILD"/fonts/*.ttf "$APPDIR/usr/bin/fonts/" 2>/dev/null || true
cp "$ROOT/third_party/fonts/LICENSE.txt" "$APPDIR/usr/bin/fonts/LuckiestGuy-LICENSE.txt"
command -v strip >/dev/null && strip "$APPDIR/usr/bin/SuperMarioAllStars"
if [ -n "$ROM" ]; then
  mkdir -p "$APPDIR/usr/bin/roms"
  cp "$ROM" "$APPDIR/usr/bin/roms/Super Mario All-Stars (USA).sfc"
  echo "  ROM: $ROM"
fi

# SDL2 goes in (and SDL3 when the SDL2 is sdl2-compat, which runs on it); the
# graphics drivers, sound servers and the C library are the system's.
lib_path() { ldd "$1" 2>/dev/null | awk -v n="$2" '$1 ~ n {print $3; exit}'; }
SDL2_SO="$(lib_path "$BIN" '^libSDL2-2\\.0\\.so')"
if [ -n "$SDL2_SO" ] && [ -f "$SDL2_SO" ]; then
  cp -L "$SDL2_SO" "$APPDIR/usr/lib/libSDL2-2.0.so.0"
  # (grep -c, not -q: with pipefail an early exit fails the pipe)
  if [ "$(LC_ALL=C grep -c 'libSDL3\.so\.0' "$SDL2_SO" 2>/dev/null || true)" != 0 ]; then
    SDL3_SO=""
    # (the 64-bit one beside SDL2 first, then ldconfig's for this machine)
    for d in "$(dirname "$SDL2_SO")/libSDL3.so.0" \
      $(ldconfig -p 2>/dev/null | awk '/libSDL3\.so\.0 .*x86-64|libSDL3\.so\.0 .*AArch64/{print $NF}') /usr/lib64/libSDL3.so.0 \
      /usr/lib/libSDL3.so.0 /usr/lib/"$ARCH"-linux-gnu/libSDL3.so.0; do
      [ -f "$d" ] && { SDL3_SO="$d"; break; }
    done
    if [ -n "$SDL3_SO" ]; then
      cp -L "$SDL3_SO" "$APPDIR/usr/lib/libSDL3.so.0"
    else
      warn "SDL2 here is sdl2-compat but libSDL3.so.0 wasn't found: the AppImage needs SDL3 on the system"
    fi
  fi
else
  warn "libSDL2 not found with ldd: the AppImage needs SDL2 on the system"
fi

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/SuperMarioAllStars" "$@"
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/super-mario-all-stars.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Super Mario All-Stars
Comment=Super Mario All-Stars (SNES), native port
Exec=SuperMarioAllStars
Icon=super-mario-all-stars
Categories=Game;
Terminal=false
EOF
cp "$APPDIR/super-mario-all-stars.desktop" "$APPDIR/usr/share/applications/"
ICON="$ROOT/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"
cp "$ICON" "$APPDIR/super-mario-all-stars.png"
cp "$ICON" "$APPDIR/usr/share/icons/hicolor/192x192/apps/super-mario-all-stars.png"
ln -sf super-mario-all-stars.png "$APPDIR/.DirIcon"

TOOLS="$BUILD/tools"
APPIMAGETOOL="$TOOLS/appimagetool-$ARCH.AppImage"
if command -v appimagetool >/dev/null; then
  APPIMAGETOOL="$(command -v appimagetool)"
elif [ ! -x "$APPIMAGETOOL" ]; then
  say "Downloading appimagetool"
  mkdir -p "$TOOLS"
  url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
  if command -v curl >/dev/null; then curl -fL --retry 3 -o "$APPIMAGETOOL.part" "$url"
  elif command -v wget >/dev/null; then wget -O "$APPIMAGETOOL.part" "$url"
  else die "curl or wget is needed to download appimagetool (or install appimagetool)"; fi
  mv "$APPIMAGETOOL.part" "$APPIMAGETOOL"
  chmod +x "$APPIMAGETOOL"
fi

say "Packing the AppImage"
mkdir -p "$DIST"
SUFFIX=""
[ -z "$ROM" ] && SUFFIX="-norom"
OUT="$DIST/SuperMarioAllStars$SUFFIX-$ARCH.AppImage"
rm -f "$OUT"
# (extract-and-run: no FUSE needed to run appimagetool itself)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" "$APPIMAGETOOL" --no-appstream "$APPDIR" "$OUT"
echo
echo "AppImage: $OUT"
echo "  (portable: its settings, saves and screenshots go in the folder it is in)"
[ -n "$ROM" ] && echo "  (it contains your ROM: keep it for yourself)"
exit 0
