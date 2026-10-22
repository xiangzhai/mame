#!/bin/sh
#
# Verify the native LoongArch64 DRC backend against -drc_use_c with a real
# ST-V game.  It runs diehard twice and compares both the emulated screen
# and the contents of the key memories and device registers at fixed frames.
# This catches "runs but reads the wrong value or drops writes" regressions
# that an exit-code-only run would miss.
#

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$root"

jobs=${JOBS:-$(nproc 2>/dev/null || echo 2)}
out=${DRC_VERIFY_OUT:-$root/build/drc_diehard_verify}

for rom in diehard.zip stvbios.zip segabill.zip; do
	if [ ! -f "roms/$rom" ]; then
		echo "SKIP: roms/$rom not found (real-game verification needs the ST-V ROMs)"
		exit 0
	fi
done

if [ ! -x ./mame ]; then
	make -j"$jobs"
fi

rm -rf "$out"
mkdir -p "$out/native" "$out/c"

run_game()
{
	backend=$1
	extra=$2
	echo "Running diehard with $backend backend..."
	DRC_VERIFY_OUT="$out/$backend" ./mame diehard \
		-rompath roms -video none -sound none -nothrottle -seconds_to_run 10 \
		-nvram_directory "$out/$backend/nvram" \
		-cfg_directory "$out/$backend/cfg" \
		-snapshot_directory "$out/$backend/snap" \
		-state_directory "$out/$backend/state" \
		-diff_directory "$out/$backend/diff" \
		$extra \
		-autoboot_script tests/devices/cpu/diehard_drc_verify.lua
}

run_game native ""
run_game c "-drc_use_c"

regions="workram_l workram_h sound_ram backup_ram vdp1_regs vdp2_regs vdp2_cram scu_regs"
frames="120 180 240"
failed=0

for frame in $frames; do
	for region in $regions; do
		native_file="$out/native/mem_$(printf '%04d' "$frame")_$region.bin"
		c_file="$out/c/mem_$(printf '%04d' "$frame")_$region.bin"
		if ! cmp -s "$native_file" "$c_file"; then
			echo "FAIL: frame $frame $region differs between native and C DRC"
			failed=1
		fi
	done

	native_screen=$(cat "$out/native/screen_$(printf '%04d' "$frame").txt")
	c_screen=$(cat "$out/c/screen_$(printf '%04d' "$frame").txt")
	if [ "$native_screen" != "$c_screen" ]; then
		echo "FAIL: frame $frame screen differs"
		echo "  native: $native_screen"
		echo "  C:      $c_screen"
		failed=1
	fi

	nonblack=$(printf '%s\n' "$native_screen" | sed -n 's/.*nonblack=\([0-9][0-9]*\).*/\1/p')
	if [ -z "$nonblack" ] || [ "$nonblack" -lt 100 ]; then
		echo "FAIL: frame $frame screen is (nearly) blank: $native_screen"
		failed=1
	fi
done

if [ "$failed" -ne 0 ]; then
	echo "DRC real-game verification: FAIL"
	exit 1
fi

echo "DRC real-game verification: PASS (screen + memories/registers match at frames $frames)"
