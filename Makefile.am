#
# ariston-remotethermo — out-of-tree STCA plugin
#
# Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = . doc

plugindir = $(libdir)/starter/configurable-adapter/plugins
plugin_LTLIBRARIES = ariston-remotethermo.la

ariston_remotethermo_la_SOURCES = \
	plugin.c \
	util.c \
	http.c \
	store.c \
	api.c

ariston_remotethermo_la_CPPFLAGS = \
	$(AM_CPPFLAGS) $(PLUGIN_CFLAGS) \
	-I$(srcdir)

if HAVE_LIBSOUP_3
ariston_remotethermo_la_CPPFLAGS += -DHAVE_LIBSOUP_3
endif

ariston_remotethermo_la_LIBADD = $(PLUGIN_LIBS)
ariston_remotethermo_la_LDFLAGS = -module -avoid-version -no-undefined

exampledir = $(datadir)/starter/configurable-adapter/examples/ariston-remotethermo
dist_example_DATA = examples/ariston-remotethermo.stca.xml

EXTRA_DIST = README COPYING

.PHONY: doc
doc:
	$(MAKE) $(AM_MAKEFLAGS) -C doc pdf
