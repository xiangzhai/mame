#!/bin/sh
#
# Build and run the standalone DRC condition-code test.  The test drives
# SETc and JMPc over every condition and all 32 combinations of the C, V,
# Z, S and U flags, so it does not depend on any guest CPU front-end.
#

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$root"

jobs=${JOBS:-$(nproc 2>/dev/null || echo 2)}

make -j"$jobs" SUBTARGET=drc_condtest SOURCES=src/mame/test/drc_condtest.cpp

echo "Running native DRC condition test..."
./drc_condtest drc_condtest -video none -sound none -nothrottle

echo "Running C DRC condition test..."
./drc_condtest drc_condtest -video none -sound none -nothrottle -drc_use_c
