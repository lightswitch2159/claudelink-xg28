#!/usr/bin/env bash
# Full build (compile + link) of the real Simplicity Studio project, driven
# entirely from the shell via its own generated CMake/Ninja workflow -- no
# Studio GUI required. Produces an actual firmware image
# (cmake_gcc/build/base/orangelink_xg28.{out,hex,bin}), not just a syntax
# check (see check_compile.sh for that, faster and does not need src/ wired
# into the project).
#
# Requires the Simplicity Studio project at $STUDIO_PROJECT to already have
# src/{aps,drivers/rail,encoding}/*.c copied into it and added to its own
# orangelink_xg28.slcp source/include lists -- see README.md "Building".
# This script does not do that copying/wiring; it only builds what's already
# there.
#
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

STUDIO_PROJECT="${1:-$HOME/SimplicityStudio/v6_workspace/orangelink_xg28}"
CMAKE_DIR="$STUDIO_PROJECT/cmake_gcc"

if [ ! -f "$CMAKE_DIR/CMakePresets.json" ]; then
	echo "not found: $CMAKE_DIR/CMakePresets.json" >&2
	echo "pass the Studio project directory as \$1, or set it up at the default path" >&2
	exit 1
fi

if ! command -v cmake >/dev/null; then
	echo "cmake not found on PATH" >&2
	exit 1
fi

cd "$CMAKE_DIR"
cmake --workflow --preset project

OUT_DIR="$CMAKE_DIR/build/base"
echo
echo "=== image ==="
ls -la "$OUT_DIR"/orangelink_xg28.{out,hex,bin}

GCC="$(command -v arm-none-eabi-gcc || true)"
if [ -z "$GCC" ]; then
	GCC=$(find "$HOME/.silabs/slt/installs/conan" -maxdepth 5 -name arm-none-eabi-gcc 2>/dev/null | head -1)
fi
SIZE="${GCC%gcc}size"
if [ -x "$SIZE" ]; then
	echo
	"$SIZE" "$OUT_DIR/orangelink_xg28.out"
fi
