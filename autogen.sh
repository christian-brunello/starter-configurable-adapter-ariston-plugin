#!/bin/bash
set -e

echo "Initializing Autotools for ariston-remotethermo..."

if [ -d "m4" ]; then
    aclocal -I m4
else
    aclocal
fi

autoconf
autoheader
libtoolize
automake --add-missing --copy

echo "Done. Now run: ./configure --with-stca=../../configurable-adapter"
