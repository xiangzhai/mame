#!/bin/sh
#
# Build and run the standalone DRC specific-access test.  The test creates
# a 32-bit address space and performs dword reads and writes through the
# native fast path, which must use a full 32-bit mask rather than zero.
#

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$root"

jobs=${JOBS:-$(nproc 2>/dev/null || echo 2)}

make -j"$jobs" SUBTARGET=drc_specifictest SOURCES=src/mame/test/drc_specifictest.cpp

echo "Running native DRC specific-access test..."
./drc_specifictest drc_specifictest -video none -sound none -nothrottle -seconds_to_run 1

echo "Running C DRC specific-access test..."
./drc_specifictest drc_specifictest -video none -sound none -nothrottle -seconds_to_run 1 -drc_use_c
