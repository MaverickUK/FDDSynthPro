#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++).
# Usage: test/run_native_tests.sh
#        SANITIZE=thread test/run_native_tests.sh   # ThreadSanitizer, for cross-core code
set -e
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
mkdir -p .pio/native-tests
SANITIZE="${SANITIZE:-address,undefined}"
"$CXX" -std=c++17 -O2 -g -Wall -Wextra -pthread -fsanitize="$SANITIZE" \
    -Isrc test/native/test_main.cpp test/native/test_short_test.cpp test/native/test_drive.cpp \
    test/native/test_track.cpp test/native/test_loader.cpp test/native/test_activity.cpp \
    test/native/test_write_decode.cpp \
    src/fdd_signals.cpp src/trace_format.cpp src/short_test.cpp \
    src/head.cpp src/rotation.cpp src/drive.cpp src/ibm_track.cpp src/rdata_stream.cpp src/cylinder_loader.cpp \
    src/activity.cpp src/mfm_decode.cpp src/track_write.cpp \
    -o .pio/native-tests/run_tests
# Tools the Python tests use (host/tests/test_track_encoder.py).
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc test/tools/build_track.cpp src/ibm_track.cpp src/rdata_stream.cpp \
    -o .pio/native-tests/build_track
"$CXX" -std=c++17 -O2 -Wall -Wextra -Isrc test/tools/decode_write.cpp src/ibm_track.cpp src/mfm_decode.cpp \
    src/track_write.cpp -o .pio/native-tests/decode_write
.pio/native-tests/run_tests "$@"
