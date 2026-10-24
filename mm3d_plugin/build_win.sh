#!/bin/bash
# Builds 3dmm.dll for Windows. Run it in an "MSYS2 MinGW 64-bit" shell, in the same environment you build mm3d in
# (see INSTALL.WIN32 of Maverick Model 3D).
#
#   usage: build_win.sh <mm3d-src-dir> <mm3d-build-dir> <libmm3d_exe.a> [outdir]
#
# A Windows DLL cannot use functions of the program that loads it unless the program exports them, and the stock
# mm3d.exe does not. So mm3d has to be linked once with the export list in win/mm3d_exports.def (40 symbols) and an
# import library:
#
#   cd <mm3d-build-dir> && ./configure ... && make clean
#   make LDFLAGS="$PWD/../dmm3d_plugin/win/mm3d_exports.def -Wl,--out-implib,$PWD/libmm3d_exe.a"
#
# (use the real path of win/mm3d_exports.def). That gives a mm3d.exe that works exactly like before and the import
# library this script links against. Use that mm3d.exe, copy 3dmm.dll to <mm3d>\plugins\1.3\ next to it.
set -e
SRC=${1:?mm3d source dir}; BLD=${2:?mm3d build dir (contains config.h)}; IMP=${3:?libmm3d_exe.a}; OUT=${4:-$(dirname "$0")/out}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
INC="-I$BLD -I$SRC/src/libmm3d -I$SRC/src/mm3dcore -I$SRC/src"
Z=""; ZL=""; if [ -f /mingw64/include/zlib.h ]; then Z="-DDMM_USE_ZLIB"; ZL="-lz"; fi
g++ -std=c++11 -O2 -shared -DWIN32 -DHAVE_CONFIG_H -DMM3D_EDIT -DPLUGIN $Z $INC \
  $HERE/core/chunky.cpp $HERE/core/actor.cpp $HERE/core/zipwriter.cpp $HERE/core/imaging.cpp $HERE/core/chunkread.cpp $HERE/core/stock.cpp \
  $HERE/mm3d/dmmexport.cc $HERE/mm3d/dmmfilter.cc "$IMP" $ZL -o "$OUT/3dmm.dll"
echo "built $OUT/3dmm.dll"
