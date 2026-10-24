#!/bin/bash
# Builds the 3DMM exporter plug-in against an MM3D source tree + its build directory.
#   usage: build.sh <mm3d-src-dir> <mm3d-build-dir> [outdir]
# The plug-in only needs the MM3D headers (it uses the symbols of the running mm3d executable).
set -e
SRC=${1:?mm3d source dir}; BLD=${2:?mm3d build dir (contains config.h)}; OUT=${3:-$(dirname "$0")/out}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
INC="-I$BLD -I$SRC/src/libmm3d -I$SRC/src/mm3dcore -I$SRC/src"
Z=""; if [ -f /usr/include/zlib.h ]; then Z="-DDMM_USE_ZLIB"; ZL="-lz"; fi
g++ -std=c++11 -O2 -fPIC -shared -fvisibility=hidden -DHAVE_CONFIG_H -DMM3D_EDIT -DPLUGIN $Z $INC \
  $HERE/core/chunky.cpp $HERE/core/actor.cpp $HERE/core/zipwriter.cpp $HERE/core/imaging.cpp $HERE/core/chunkread.cpp $HERE/core/stock.cpp \
  $HERE/mm3d/dmmexport.cc $HERE/mm3d/dmmfilter.cc $ZL -o "$OUT/3dmm.so"
echo "built $OUT/3dmm.so"
