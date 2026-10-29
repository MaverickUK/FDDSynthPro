#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++), and
# build the tools the Python tests use. test/native.mk does the building, in
# parallel and only what changed.
# Usage: test/run_native_tests.sh
#        SANITIZE=thread test/run_native_tests.sh   # ThreadSanitizer, for cross-core code
set -e
cd "$(dirname "$0")/.."
python3 scripts/embed_welcome.py  # src/welcome_text.h, the welcome disk's README
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
SANITIZE="${SANITIZE:-address,undefined}"
make -s -f test/native.mk -j"$JOBS" CXX="${CXX:-c++}" SANITIZE="$SANITIZE"
# Each sanitizer setting has its own binary; run_tests points at this one
# (scripts/build_panel_page.py runs it too).
ln -sf "obj-$(echo "$SANITIZE" | tr , -)/run_tests" .pio/native-tests/run_tests
.pio/native-tests/run_tests "$@"
