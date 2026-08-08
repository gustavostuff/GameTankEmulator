#!/usr/bin/env bash
#
# install-rgbpi.sh - run on a Raspberry Pi (e.g. RGB-Pi OS over SSH).
#
# Clones (or updates) the repo, builds WRAPPER fullscreen, and writes:
#
#   GameTank/
#     GameTankEmulator
#     roms/
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/gustavostuff/GameTankEmulator/main/scripts/linux/install-rgbpi.sh | bash
#
# Env overrides: REPO_URL, SRC_DIR, OUT_DIR, JOBS
#

set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/gustavostuff/GameTankEmulator.git}"
SRC_DIR="${SRC_DIR:-$HOME/GameTankEmulator}"
OUT_DIR="${OUT_DIR:-$HOME/GameTank}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

echo ">> Repo:   $REPO_URL"
echo ">> Source: $SRC_DIR"
echo ">> Output: $OUT_DIR"

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
	SUDO=sudo
fi

echo ">> Installing dependencies..."
export DEBIAN_FRONTEND=noninteractive
$SUDO apt-get update -y
$SUDO apt-get install -y --no-install-recommends \
	build-essential git libsdl2-dev ca-certificates

if [ -d "$SRC_DIR/.git" ]; then
	echo ">> Updating $SRC_DIR ..."
	git -C "$SRC_DIR" pull --ff-only
	git -C "$SRC_DIR" submodule update --init --recursive
else
	echo ">> Cloning into $SRC_DIR ..."
	git clone --depth 1 --recurse-submodules "$REPO_URL" "$SRC_DIR"
fi

echo ">> Building..."
make -C "$SRC_DIR" WRAPPERMODE=yes CONSOLE_DISPLAY=fullscreen -j"$JOBS"

BIN="$SRC_DIR/build/GameTankEmulator"
if [ ! -x "$BIN" ]; then
	echo "ERROR: build failed - missing $BIN" >&2
	exit 1
fi

echo ">> Packaging $OUT_DIR ..."
mkdir -p "$OUT_DIR/roms"
cp -f "$BIN" "$OUT_DIR/GameTankEmulator"
# USB/FAT targets may reject chmod; ignore failures.
chmod +x "$OUT_DIR/GameTankEmulator" 2>/dev/null || true

echo
echo ">> Done."
echo "   Binary: $OUT_DIR/GameTankEmulator"
echo "   ROMs:   $OUT_DIR/roms/"
