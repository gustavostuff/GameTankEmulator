#!/usr/bin/env bash
#
# install-rgbpi.sh — run on a Raspberry Pi (e.g. RGB-Pi OS over SSH).
#
# Clones/updates GameTankEmulator, builds the WRAPPER fullscreen binary,
# and writes a GameTank/ folder:
#
#   GameTank/
#     GameTankEmulator
#     roms/          (empty — drop .gtr files here)
#
# Font and tab icons are embedded in the binary.
# If /media/usb1/roms/ports exists, installs GameTank/ there.
#
# Usage (from README, paste into SSH):
#   curl -fsSL https://raw.githubusercontent.com/gustavostuff/GameTankEmulator/main/scripts/linux/install-rgbpi.sh | bash
#
# Or from a local checkout:
#   bash scripts/linux/install-rgbpi.sh
#
# Env overrides:
#   REPO_URL   git remote (default: https://github.com/gustavostuff/GameTankEmulator.git)
#   SRC_DIR    clone/build directory (default: $HOME/GameTankEmulator)
#   OUT_DIR    output GameTank folder (default: USB ports path if present, else $HOME/GameTank)
#   JOBS       parallel make jobs (default: nproc)
#

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/gustavostuff/GameTankEmulator.git}"
SRC_DIR="${SRC_DIR:-$HOME/GameTankEmulator}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

if [ -z "${OUT_DIR:-}" ]; then
	if [ -d /media/usb1/roms/ports ]; then
		OUT_DIR="/media/usb1/roms/ports/GameTank"
	else
		OUT_DIR="$HOME/GameTank"
	fi
fi

echo ">> Repo:    $REPO_URL"
echo ">> Source:  $SRC_DIR"
echo ">> Output:  $OUT_DIR"
echo ">> Jobs:    $JOBS"

need_sudo=0
if [ "$(id -u)" -ne 0 ]; then
	need_sudo=1
fi
run_root() {
	if [ "$need_sudo" -eq 1 ]; then
		sudo "$@"
	else
		"$@"
	fi
}

echo ">> Installing build dependencies (git, g++, SDL2)..."
export DEBIAN_FRONTEND=noninteractive
run_root apt-get update -y
run_root apt-get install -y --no-install-recommends \
	build-essential \
	git \
	libsdl2-dev \
	ca-certificates

if [ -d "$SRC_DIR/.git" ]; then
	echo ">> Updating existing checkout..."
	git -C "$SRC_DIR" remote set-url origin "$REPO_URL" 2>/dev/null || true
	git -C "$SRC_DIR" fetch --depth 1 origin
	# Prefer main, fall back to master / current branch tip
	if git -C "$SRC_DIR" rev-parse --verify origin/main >/dev/null 2>&1; then
		git -C "$SRC_DIR" checkout -B main origin/main
	elif git -C "$SRC_DIR" rev-parse --verify origin/master >/dev/null 2>&1; then
		git -C "$SRC_DIR" checkout -B master origin/master
	fi
else
	echo ">> Cloning repository..."
	mkdir -p "$(dirname "$SRC_DIR")"
	git clone --depth 1 --recurse-submodules "$REPO_URL" "$SRC_DIR"
fi

echo ">> Ensuring submodules..."
git -C "$SRC_DIR" submodule update --init --recursive

echo ">> Building WRAPPER (fullscreen)..."
make -C "$SRC_DIR" clean || true
make -C "$SRC_DIR" WRAPPERMODE=yes CONSOLE_DISPLAY=fullscreen -j"$JOBS"

BIN="$SRC_DIR/build/GameTankEmulator"
if [ ! -x "$BIN" ]; then
	echo "ERROR: build failed — missing $BIN" >&2
	exit 1
fi

echo ">> Packaging $OUT_DIR ..."
mkdir -p "$OUT_DIR/roms"
cp -f "$BIN" "$OUT_DIR/GameTankEmulator"
chmod +x "$OUT_DIR/GameTankEmulator"

echo
echo ">> Done."
ls -lah "$OUT_DIR" "$OUT_DIR/roms"
echo
echo "   Binary:  $OUT_DIR/GameTankEmulator"
echo "   ROMs:    $OUT_DIR/roms/   (place .gtr files here)"
echo
echo "   Rebuild later:"
echo "     bash $SRC_DIR/scripts/linux/install-rgbpi.sh"
