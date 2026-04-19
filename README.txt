chronos-cli
Command line tools for controlling the Chronos camera. Includes a D-Bus API mock
and the GStreamer-based cam-pipeline daemon (live display + recording).

=====================================================================
Dependencies
=====================================================================
Camera build (cross-compile, ARM armel, Debian jessie target):
    Inside the camera sysroot you must have:
        libglib2.0-dev, libdbus-1-dev, libdbus-glib-1-dev,
        libxml2-dev, libjpeg-dev,
        libgstreamer0.10-dev, libgstreamer-plugins-base0.10-dev,
        libhdf5-dev
    The ARM build links against GStreamer 0.10, which is EOL upstream --
    only the embedded jessie sysroot supplies it.

Host build (for running the Tier-1 HDF5 sink test on a dev box):
    > sudo apt install build-essential autoconf automake libtool pkg-config \
                        libhdf5-dev python3-h5py python3-numpy
    GStreamer is NOT required on the host -- the host build only compiles
    the pure-libhdf5 test target, not cam-pipeline itself.

=====================================================================
Build (camera, cross-compile)
=====================================================================
    > ./bootstrap
    > ./xcompile.sh <path-to-armel-sysroot>
    > make
    Output is in src/. Deploy src/cam-pipeline to the camera's /usr/bin.

=====================================================================
Build + test (host, for iterating on h5.c)
=====================================================================
    > ./bootstrap
    > ./configure
    > make check
    Runs src/test_h5_sink. On success: "ALL TESTS PASSED".
    The produced file is /tmp/test_h5_sink.h5 -- validate with h5py.