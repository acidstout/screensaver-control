#!/usr/bin/env bash
# build.sh - same build as build.bat, for a bash/MSYS shell. See build.bat.
set -e
cd "$(dirname "$0")"
mkdir -p build

CFLAGS="-m32 -O2 -nostdlib -nostartfiles -nodefaultlibs -fno-ident -fno-builtin
        -fno-stack-protector -fno-asynchronous-unwind-tables
        -ffunction-sections -fdata-sections
        -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type"

LDFLAGS="-m32 -nostdlib -nostartfiles -nodefaultlibs -Llib32
         -Wl,--gc-sections -Wl,--entry,_WinMainCRTStartup
         -Wl,--subsystem,windows:4.0
         -Wl,--major-os-version,4 -Wl,--minor-os-version,0
         -Wl,--file-alignment,512 -Wl,-s"

echo "[1/3] compiling scrctl.c"
gcc $CFLAGS -c scrctl.c -o build/scrctl.o

echo "[2/3] compiling resources"
windres --codepage=1252 -F pe-i386 -O coff -I. -i scrctl.rc -o build/scrctl.res.o

echo "[3/3] linking"
gcc $LDFLAGS -o build/scrctl.exe build/scrctl.o build/scrctl.res.o \
    -lkernel32 -luser32 -lgdi32 -lshell32 -ladvapi32

echo
echo "Built build/scrctl.exe ($(stat -c%s build/scrctl.exe) bytes)"
