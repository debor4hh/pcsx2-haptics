#!/bin/sh
# Cross-compiles with MinGW-w64. Output goes to dist/.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
CFLAGS="-O2 -Wall -Wextra -Wno-cast-function-type -Wno-missing-field-initializers -D_WIN32_WINNT=0x0A00 -DUNICODE -static-libgcc"
mkdir -p dist build
$WINDRES -I res res/resources.rc -O coff -o build/resources.o
$CC $CFLAGS -shared -o dist/version.dll src/version_proxy.c src/hooks.c src/bridge.c src/ui.c build/resources.o src/version.def \
    -lwinhttp -lcomctl32 -lshell32 -lgdi32 -lgdiplus -luxtheme -lole32 -luuid -s
cp pcsx2-haptics.ini dist/
ls -l dist
