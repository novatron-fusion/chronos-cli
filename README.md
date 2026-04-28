# chronos-cli

Command line tools for controlling the Chronos camera. Includes a D-Bus API mock
and the GStreamer-based cam-pipeline daemon (live display + recording).

## Dependencies

### Camera build (ARM armel, Debian Jessie target)

The camera sysroot must contain:

- `libglib2.0-dev`, `libdbus-1-dev`, `libdbus-glib-1-dev`
- `libxml2-dev`, `libjpeg-dev`
- `libgstreamer0.10-dev`, `libgstreamer-plugins-base0.10-dev`
- `libhdf5-dev`

The ARM build links against GStreamer 0.10, which is EOL upstream — only the
Jessie sysroot supplies it.

### Host build (for the HDF5 sink test)

```bash
sudo apt install build-essential autoconf automake libtool pkg-config \
                 libhdf5-dev python3-h5py python3-numpy
```

GStreamer is **not** required on the host — the host build only compiles the
pure-libhdf5 test target, not `cam-pipeline` itself.

## Build + test (host)

For iterating on `h5.c` without a camera:

```bash
./bootstrap
./configure
make check
```

Runs `src/test_h5_sink`. On success: `ALL TESTS PASSED`.
The produced file is `/tmp/test_h5_sink.h5` — validate with `h5py`.

## Cross-compile for camera

There are two methods: using a proper cross-toolchain with `xcompile.sh`, or
using QEMU user-mode chroot with the camera's native GCC. The QEMU method is
recommended because modern host cross-compilers (e.g. Ubuntu 24.04's
`gcc-arm-linux-gnueabi` with GCC 13 / glibc 2.39) produce binaries with
time64 ABI symbols that are incompatible with the camera's glibc 2.19.

### Method 1: xcompile.sh (with a compatible cross-toolchain)

If you have an armel cross-compiler that targets glibc 2.19 (e.g. Linaro
GCC 4.9), point `xcompile.sh` at the camera sysroot:

```bash
./bootstrap
./xcompile.sh <path-to-armel-sysroot>
make
```

Output: `src/cam-pipeline`. Deploy to the camera's `/usr/bin/`.

### Method 2: QEMU chroot (recommended)

Build inside the camera's own root filesystem using QEMU user-mode emulation.
This uses the camera's native GCC 4.9.2, avoiding any ABI mismatches.

#### Prerequisites

```bash
sudo apt install qemu-user-static binfmt-support
```

You also need a Chronos camera SD card image (e.g.
`chronos-voyager-20220124.img.xz`).

#### 1. Decompress and mount the camera image

```bash
xz -dk chronos-voyager-20220124.img.xz        # produces .img (~3.5 GB)
mkdir -p ~/chronos-sysroot
sudo mount -o loop,offset=$((80325 * 512)),ro,noload ~/chronos-voyager.img ~/chronos-sysroot
```

> **Note:** The root partition starts at sector 80325. The `noload` option is
> required because the ext3 journal may be dirty; `ro` keeps the image
> unmodified.

#### 2. Create a writable overlay

The image is mounted read-only, so use overlayfs to get a writable sysroot:

```bash
mkdir -p ~/sysroot-overlay/{upper,work,merged}
sudo mount -t overlay overlay \
    -o lowerdir=$HOME/chronos-sysroot,upperdir=$HOME/sysroot-overlay/upper,workdir=$HOME/sysroot-overlay/work \
    $HOME/sysroot-overlay/merged
```

#### 3. Install HDF5 into the sysroot (first time only)

The overlay upper dir (`~/sysroot-overlay/upper`) persists across remounts,
so this step is only needed once. Download the Jessie armel HDF5 packages
from the Debian archive and extract them into the overlay:

```bash
cd /tmp
wget http://archive.debian.org/debian/pool/main/h/hdf5/libhdf5-8_1.8.13+docs-15+deb8u1_armel.deb
wget http://archive.debian.org/debian/pool/main/h/hdf5/libhdf5-dev_1.8.13+docs-15+deb8u1_armel.deb
sudo dpkg-deb -x libhdf5-8_1.8.13+docs-15+deb8u1_armel.deb ~/sysroot-overlay/merged
sudo dpkg-deb -x libhdf5-dev_1.8.13+docs-15+deb8u1_armel.deb ~/sysroot-overlay/merged
```

#### 4. Copy the source tree and QEMU into the chroot

```bash
sudo cp /usr/bin/qemu-arm-static ~/sysroot-overlay/merged/usr/bin/
sudo rm -rf ~/sysroot-overlay/merged/home/chronos-cli
sudo cp -a ~/chronos-cli ~/sysroot-overlay/merged/home/chronos-cli
```

The autotools auxiliary files (`install-sh`, `compile`, `missing`, etc.) are
symlinks that point to the host's `/usr/share/automake-*`, which won't exist
inside the chroot. Replace them with real copies:

```bash
cd ~/sysroot-overlay/merged/home/chronos-cli
for f in install-sh compile missing ar-lib config.guess config.sub depcomp test-driver ltmain.sh; do
    if [[ -L "$f" ]]; then
        sudo cp --remove-destination "$(readlink -f "$f")" "$f"
    fi
done
```

#### 5. Build inside the chroot

```bash
sudo chroot ~/sysroot-overlay/merged /usr/bin/qemu-arm-static /bin/bash -c \
    "cd /home/chronos-cli && ./configure && make"
```

The output binary is at:

```
~/sysroot-overlay/merged/home/chronos-cli/src/cam-pipeline
```

Verify it is an ARM binary:

```bash
file ~/sysroot-overlay/merged/home/chronos-cli/src/cam-pipeline
# ELF 32-bit LSB executable, ARM, EABI5 ...
```

#### 6. Deploy to camera

The binary dynamically links `libhdf5_serial.so.8`, so the runtime library
must be installed on the camera (first time only):

```bash
scp /tmp/libhdf5-8_1.8.13+docs-15+deb8u1_armel.deb root@<camera-ip>:/tmp/
ssh root@<camera-ip> dpkg -i /tmp/libhdf5-8_1.8.13+docs-15+deb8u1_armel.deb
```

Copy the binary and restart the video service:

```bash
scp ~/sysroot-overlay/merged/home/chronos-cli/src/cam-pipeline root@<camera-ip>:/usr/bin/cam-pipeline
ssh root@<camera-ip> systemctl restart chronos-video
```

No full reboot is needed — `cam-pipeline` runs as the `chronos-video` systemd
service, so restarting it is sufficient.

#### Cleanup

```bash
sudo umount ~/sysroot-overlay/merged
sudo umount ~/chronos-sysroot
rm -rf ~/sysroot-overlay
```

## VS Code tasks

The project includes VS Code tasks in `.vscode/tasks.json`:

- **Build (ARM chroot)** (`Ctrl+Shift+B`) — copies source into the chroot,
  fixes autotools symlinks, and runs `configure && make` via QEMU.
- **Build + test (host)** — runs `configure && make check` natively.
- **Mount chroot** — sets up the image + overlay mounts (run once per session).
- **Unmount chroot** — tears down the mounts.

Run "Mount chroot" first, then use `Ctrl+Shift+B` to build repeatedly.