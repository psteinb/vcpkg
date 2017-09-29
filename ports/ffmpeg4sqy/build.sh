#!/usr/bin/bash
set -e
set -x
export PATH=/usr/bin:$PATH

env | grep PATH
pacman -Sy --noconfirm --needed diffutils make


PATH_TO_BUILD_DIR="`cygpath "$1"`"
echo "PATH_TO_BUILD_DIR=${PATH_TO_BUILD_DIR}"
PATH_TO_SRC_DIR="`cygpath "$2"`"
PATH_TO_PACKAGE_DIR="`cygpath "$3"`"
# Note: $4 is extra configure options

cd "$PATH_TO_BUILD_DIR"
echo "=== CONFIGURING ==="
"$PATH_TO_SRC_DIR/configure" --toolchain=msvc "--prefix=$PATH_TO_PACKAGE_DIR" $4
echo "=== BUILDING ==="
make -j6
echo "=== INSTALLING ==="
make install
