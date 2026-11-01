#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/md3-host-tests"
mkdir -p "$BUILD_DIR"
cp "$ROOT/firmware/main/link.c" "$BUILD_DIR/link.c"
cp "$ROOT/firmware/main/usb_msc.c" "$BUILD_DIR/usb_msc.c"
cp "$ROOT/firmware/main/usb_msc.h" "$BUILD_DIR/usb_msc.h"
cp "$ROOT/firmware/main/storage.c" "$BUILD_DIR/storage.c"
cp "$ROOT/firmware/main/storage.h" "$BUILD_DIR/storage.h"
cp "$ROOT/firmware/main/audio.c" "$BUILD_DIR/audio.c"
cp "$ROOT/firmware/main/audio.h" "$BUILD_DIR/audio.h"

${CC:-cc} \
    -std=c11 -g -fno-omit-frame-pointer \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=undefined \
    -I"$BUILD_DIR" \
    -I"$ROOT/tests/host/include" \
    -I"$ROOT/firmware/main" \
    "$ROOT/tests/host/test_link.c" \
    "$ROOT/tests/host/idf_stubs.c" \
    -o "$BUILD_DIR/test_link"

"$BUILD_DIR/test_link"

${CC:-cc} \
    -std=c11 -g -fno-omit-frame-pointer \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=undefined \
    -I"$BUILD_DIR" \
    -I"$ROOT/tests/host/include" \
    "$ROOT/tests/host/test_usb_msc.c" \
    -o "$BUILD_DIR/test_usb_msc"

"$BUILD_DIR/test_usb_msc"

${CC:-cc} \
    -std=c11 -g -fno-omit-frame-pointer \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=undefined \
    -I"$BUILD_DIR" \
    -I"$ROOT/tests/host/include" \
    "$ROOT/tests/host/test_storage.c" \
    -o "$BUILD_DIR/test_storage"

"$BUILD_DIR/test_storage"

${CC:-cc} \
    -std=c11 -g -fno-omit-frame-pointer \
    -Wall -Wextra -Werror -Wno-unused-parameter \
    -fsanitize=undefined \
    -I"$BUILD_DIR" \
    -I"$ROOT/tests/host/include" \
    "$ROOT/tests/host/test_audio.c" \
    -o "$BUILD_DIR/test_audio"

"$BUILD_DIR/test_audio"
