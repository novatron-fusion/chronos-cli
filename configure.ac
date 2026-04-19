AC_INIT([chronos-cli], [v0.4.0-beta])
AM_INIT_AUTOMAKE([subdir-objects -Wall -Werror foreign])
AM_SILENT_RULES([yes])
AM_PROG_AR
AC_PROG_CC
AC_PROG_CXX
AC_PROG_RANLIB
LT_INIT

## Set a conditional if a GCC sysroot is provided.
AC_SUBST(SYSROOT, ["$with_sysroot"])
AM_CONDITIONAL(SYSROOT, [test "x$with_sysroot" != "xno"])

## Build all tools only when compiling for ARM.
AC_CANONICAL_HOST
AS_CASE([$host],
	[arm*-linux-*], [XCOMPILE="camera"],
	[XCOMPILE="host"])
AM_CONDITIONAL(CAMBUILD, [test "$XCOMPILE" = "camera"])

GST_REQUIRED=0.10.32

## GLib / DBus / libxml are only required for the camera build. Host builds
## only run the libhdf5-only Tier-1 test, so skip those checks there.
if test "$XCOMPILE" = "camera"; then
  PKG_CHECK_MODULES(GLIB, [glib-2.0 gio-2.0])
  PKG_CHECK_MODULES(DBUS, [dbus-glib-1])
  PKG_CHECK_MODULES(XML, [libxml-2.0])
  PKG_CHECK_MODULES(GST, [
    gstreamer-0.10 >= $GST_REQUIRED
    gstreamer-base-0.10 >= $GST_REQUIRED
    gstreamer-controller-0.10 >= $GST_REQUIRED
  ], [
    AC_SUBST(GST_CFLAGS)
    AC_SUBST(GST_LIBS)
  ], [
    AC_MSG_ERROR([
        You need to install or upgrade the GStreamer development
        packages on your system. On debian-based systems these are
        libgstreamer0.10-dev and libgstreamer-plugins-base0.10-dev.
        on RPM-based systems gstreamer0.10-devel, libgstreamer0.10-devel
        or similar. The minimum version required is $GST_REQUIRED.
    ])
  ])
fi

## HDF5 for the h5 recording format. Try pkg-config first (hdf5 or hdf5-serial),
## fall back to a plain -lhdf5 if the .pc file isn't shipped (older jessie).
PKG_CHECK_MODULES(HDF5, [hdf5], [], [
  PKG_CHECK_MODULES(HDF5, [hdf5-serial], [], [
    AC_CHECK_HEADER([hdf5.h], [],
      [AC_MSG_ERROR([hdf5.h not found -- install libhdf5-dev])])
    AC_CHECK_LIB([hdf5], [H5Fcreate], [],
      [AC_MSG_ERROR([libhdf5 not found -- install libhdf5-dev])])
    HDF5_CFLAGS=""
    HDF5_LIBS="-lhdf5"
    AC_SUBST(HDF5_CFLAGS)
    AC_SUBST(HDF5_LIBS)
  ])
])

AC_SYS_LARGEFILE

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
	Makefile
	src/Makefile
])
AC_OUTPUT
