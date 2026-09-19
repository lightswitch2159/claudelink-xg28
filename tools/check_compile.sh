#!/usr/bin/env bash
# Syntax-check the driver and encoding layer against the real project's exact
# build configuration -- no linking, no hardware, but it exercises every
# header include and every RAIL_*() call site for real.
#
# Requires a Simplicity Studio project generated for this exact board
# (xG28-EK2705A / BRD2705A) sitting somewhere on disk -- point STUDIO_PROJECT
# at it. The include/define list is extracted directly from that project's
# own generated cmake_gcc/<project>.cmake, not hand-maintained here, so it
# stays correct as Studio regenerates the project.
#
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

STUDIO_PROJECT="${1:-$HOME/SimplicityStudio/v6_workspace/rail_soc_railtest}"
PROJECT_NAME="$(basename "$STUDIO_PROJECT")"
CMAKE_FILE="$STUDIO_PROJECT/cmake_gcc/${PROJECT_NAME}.cmake"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -f "$CMAKE_FILE" ]; then
	echo "not found: $CMAKE_FILE" >&2
	echo "pass the Studio project directory as \$1, or set it up at the default path" >&2
	exit 1
fi

GCC="$(command -v arm-none-eabi-gcc || true)"
if [ -z "$GCC" ]; then
	GCC=$(find "$HOME/.silabs/slt/installs/conan" -maxdepth 5 -name arm-none-eabi-gcc 2>/dev/null | head -1)
fi
if [ -z "$GCC" ]; then
	echo "arm-none-eabi-gcc not found on PATH or under ~/.silabs/slt/installs/conan" >&2
	exit 1
fi
echo "toolchain: $GCC"
"$GCC" --version | head -1

RSP="$(mktemp)"
trap 'rm -f "$RSP"' EXIT

python3 - "$CMAKE_FILE" "$STUDIO_PROJECT" "$RSP" <<'PYEOF'
import re, sys

cmake_file, project_dir, rsp_path = sys.argv[1:4]
raw = open(cmake_file).read()

def extract_block(text, name):
	m = re.search(rf'{name}\(slc PUBLIC\s*\n(.*?)\n\)', text, re.S)
	out = []
	for line in m.group(1).splitlines():
		line = line.strip()
		if not line:
			continue
		first = line.index('"')
		last = line.rindex('"')
		out.append(line[first + 1:last].replace('\\"', '"'))
	return out

incs = extract_block(raw, "target_include_directories")
defs = extract_block(raw, "target_compile_definitions")

sdk_match = re.search(r'set\(COPIED_SDK_PATH "([^"]+)"\)', raw)
sdk = sdk_match.group(1) if sdk_match else "simplicity_sdk"

lines = []
for i in incs:
	p = i.replace("${COPIED_SDK_PATH}", sdk)
	p = p[3:] if p.startswith("../") else p
	lines.append(f'-I"{project_dir}/{p}"')
for d in defs:
	lines.append(f"-D'{d}'" if '"' in d else f"-D{d}")

open(rsp_path, "w").write("\n".join(lines))
print(f"{len(incs)} includes, {len(defs)} defines extracted from {cmake_file}")
PYEOF

CFLAGS=(-fsyntax-only -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse
        -Wall -Wextra -Og --specs=nano.specs)

echo
echo "=== driver ==="
"$GCC" "${CFLAGS[@]}" -I"$REPO/src/drivers/rail" "@$RSP" \
	"$REPO/src/drivers/rail/sl_subg_radio.c"
echo "OK: sl_subg_radio.c"

echo "=== driver header (standalone) ==="
HDR_CHECK="$(mktemp --suffix=.c)"
echo '#include "sl_subg_radio.h"' > "$HDR_CHECK"
"$GCC" "${CFLAGS[@]}" -I"$REPO/src/drivers/rail" "@$RSP" "$HDR_CHECK"
rm -f "$HDR_CHECK"
echo "OK: sl_subg_radio.h"

echo "=== encoding layer (no SDK deps) ==="
for f in 4b6b manchester; do
	"$GCC" -fsyntax-only -mcpu=cortex-m33 -mthumb -std=c11 -Wall -Wextra \
		-I"$REPO/src/encoding" "$REPO/src/encoding/$f.c"
	echo "OK: $f.c"
done

echo
echo "All clean."
