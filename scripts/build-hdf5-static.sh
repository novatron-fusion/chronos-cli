#!/bin/bash
# Build HDF5 1.10.11 as a static library for the Chronos camera (armel).
#
# Two-step process:
#   1. Run the download step on the HOST (has internet):
#        scripts/build-hdf5-static.sh download
#      This fetches the tarball into the chroot's /tmp.
#
#   2. Run the build step INSIDE the QEMU chroot (no internet needed):
#        sudo chroot ~/sysroot-overlay/merged /usr/bin/qemu-arm-static \
#            /bin/bash /home/chronos-cli/scripts/build-hdf5-static.sh build
#
# Or just run without arguments inside the chroot if the tarball is already there.

set -e

HDF5_VER=1.10.11
HDF5_TAG=hdf5-1_10_11
HDF5_URL="https://github.com/HDFGroup/hdf5/releases/download/${HDF5_TAG}/${HDF5_TAG}.tar.gz"
PREFIX=/opt/hdf5-static
WORKDIR=/tmp/hdf5-build

## "download" subcommand — run on the host to fetch the tarball into the chroot.
if [[ "${1:-}" == "download" ]]; then
    CHROOT_TMP="${2:-$HOME/sysroot-overlay/merged/tmp/hdf5-build}"
    mkdir -p "$CHROOT_TMP"
    TARBALL="$CHROOT_TMP/${HDF5_TAG}.tar.gz"
    if [ -f "$TARBALL" ]; then
        echo "Tarball already exists: $TARBALL"
    else
        echo "Downloading HDF5 $HDF5_VER to $TARBALL ..."
        wget -q "$HDF5_URL" -O "$TARBALL"
        echo "Done."
    fi
    exit 0
fi

## From here on, we're running inside the chroot (or anywhere with the tarball).
if [ -f "$PREFIX/lib/libhdf5.a" ]; then
    echo "HDF5 static library already exists at $PREFIX/lib/libhdf5.a — skipping build."
    exit 0
fi

echo "=== Building HDF5 $HDF5_VER static library ==="

mkdir -p "$WORKDIR"
cd "$WORKDIR"

TARBALL="${HDF5_TAG}.tar.gz"
if [ ! -f "$TARBALL" ]; then
    echo "ERROR: $WORKDIR/$TARBALL not found."
    echo "Run this on the host first:  scripts/build-hdf5-static.sh download"
    exit 1
fi

echo "Extracting ..."
rm -rf hdfsrc
tar xzf "$TARBALL"
cd hdfsrc

echo "Configuring ..."
./configure \
    --prefix="$PREFIX" \
    --enable-static \
    --disable-shared \
    --disable-fortran \
    --disable-cxx \
    --disable-java \
    --disable-hl \
    --disable-tests \
    --disable-tools \
    --silent

echo "Building (this takes a few minutes) ..."
make -j"$(nproc)" --silent

echo "Installing to $PREFIX ..."
make install --silent

echo "=== Done.  Static library: $PREFIX/lib/libhdf5.a ==="
ls -lh "$PREFIX/lib/libhdf5.a"
