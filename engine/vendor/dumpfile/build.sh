#!/bin/sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBFSNTFS_DIR="$SCRIPT_DIR/../../../third_party/lgpl-dynamic/libfsntfs"

cc -std=c11 \
  -I "$LIBFSNTFS_DIR/include" \
  -I "$LIBFSNTFS_DIR/common" \
  "$SCRIPT_DIR/main.c" \
  -L "$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -lfsntfs \
  -Wl,-rpath,"$LIBFSNTFS_DIR/libfsntfs/.libs" \
  -o "$SCRIPT_DIR/dumpfile"

echo "编译成功: $SCRIPT_DIR/dumpfile"
