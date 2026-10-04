#
# ariston-remotethermo — out-of-tree STCA plugin
#
# Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
#
# Process this file with autoconf to produce configure.

AC_INIT([stca-plugin-ariston-remotethermo], [1.0.0], [startercontroller@gmail.com])
AC_CONFIG_SRCDIR([plugin.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])

AC_PROG_CC
AM_PROG_AR
LT_INIT([dlopen])

PKG_PROG_PKG_CONFIG

AC_ARG_WITH([stca],
  [AS_HELP_STRING([--with-stca=DIR],
    [use uninstalled starter-configurable-adapter from source/build tree DIR])],
  [], [with_stca=])

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.30 gobject-2.0 >= 2.30 gio-2.0 >= 2.30 gmodule-2.0])
PKG_CHECK_MODULES([LIBXML2], [libxml-2.0 >= 2.9.0])
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0])

have_libsoup3=no
PKG_CHECK_MODULES([LIBSOUP], [libsoup-3.0],
  [have_libsoup3=yes],
  [PKG_CHECK_MODULES([LIBSOUP], [libsoup-2.4 >= 2.58])])
AM_CONDITIONAL([HAVE_LIBSOUP_3], [test "$have_libsoup3" = "yes"])
AS_IF([test "$have_libsoup3" = "yes"],
  [AC_DEFINE([HAVE_LIBSOUP_3], [1], [Define if building against libsoup 3.x])])

if test -n "$with_stca"; then
  AS_IF([test ! -d "$with_stca/include/starter/configurable-adapter"],
    [AC_MSG_ERROR([--with-stca=$with_stca does not look like a configurable-adapter tree])])
  AS_IF([test ! -f "$with_stca/lib/libstarter-configurable-adapter.la"],
    [AC_MSG_ERROR([libstarter-configurable-adapter.la not found under $with_stca/lib; build the adapter first])])
  STCA_CFLAGS="-I$with_stca/include $GLIB_CFLAGS $LIBXML2_CFLAGS"
  STCA_LIBS="$with_stca/lib/libstarter-configurable-adapter.la $GLIB_LIBS $LIBXML2_LIBS"
  AC_MSG_NOTICE([using uninstalled STCA from $with_stca])
else
  PKG_CHECK_MODULES([STCA], [starter-configurable-adapter])
fi

PLUGIN_CFLAGS="$STCA_CFLAGS $JSON_GLIB_CFLAGS $LIBSOUP_CFLAGS"
PLUGIN_LIBS="$STCA_LIBS $JSON_GLIB_LIBS $LIBSOUP_LIBS"
AC_SUBST(PLUGIN_CFLAGS)
AC_SUBST(PLUGIN_LIBS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AS_IF([test "$have_libsoup3" = "yes"],
  [soup_msg="libsoup-3.0"],
  [soup_msg="libsoup-2.4"])
AC_MSG_NOTICE([plugin will use $soup_msg])
