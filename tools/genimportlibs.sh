#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# genimportlibs.sh - regenerate the 32-bit import libraries in lib32/.
#
# The MinGW-w64 toolchain on this machine is x86_64-only: it can *compile*
# -m32 objects but ships no 32-bit import libraries to link them against.
# This script derives them automatically:
#
#   1. compile scrctl.c to a 32-bit object,
#   2. read its undefined __imp__Foo@N symbols,
#   3. work out which system DLL exports each one by looking the undecorated
#      name up in the toolchain's own 64-bit import libraries,
#   4. emit lib32/<dll>.def and turn it into lib32/lib<dll>.a with dlltool.
#
# `dlltool -k` (--kill-at) is the important part: it keeps the @N decoration
# on the symbol the linker resolves while writing the *undecorated* name into
# the import table, which is what the real 32-bit system DLLs export.
#
# Only needs re-running when the set of Win32 APIs used by scrctl.c changes.
# Run from the project root:  bash tools/genimportlibs.sh
# ---------------------------------------------------------------------------
set -e
cd "$(dirname "$0")/.."

DLLS="kernel32 user32 gdi32 shell32 advapi32"
SYSLIB=$(dirname "$(command -v gcc)")/../x86_64-w64-mingw32/lib

mkdir -p build lib32

echo "[1/4] compiling probe object"
gcc -m32 -O2 -c scrctl.c -o build/probe.o \
    -nostdlib -nostartfiles -nodefaultlibs -fno-ident -fno-builtin \
    -fno-stack-protector -fno-asynchronous-unwind-tables -w

echo "[2/4] collecting undefined imports"
nm build/probe.o | sed -n 's/.* U __imp__\{0,1\}//p' | sort -u > build/undef.txt

echo "[3/4] resolving each import to its DLL"
for d in $DLLS; do : > "build/$d.list"; done
: > build/unresolved.txt

while read -r sym; do
    [ -n "$sym" ] || continue
    plain=${sym%%@*}                       # strip the @N stdcall suffix
    found=
    for d in $DLLS; do
        if nm --defined-only "$SYSLIB/lib$d.a" 2>/dev/null \
             | grep -qx "[0-9a-f]* T $plain"; then
            echo "$sym" >> "build/$d.list"
            found=$d
            break
        fi
    done
    [ -n "$found" ] || echo "$sym" >> build/unresolved.txt
done < build/undef.txt

if [ -s build/unresolved.txt ]; then
    echo "ERROR: could not attribute these imports to a DLL:" >&2
    cat build/unresolved.txt >&2
    exit 1
fi

echo "[4/4] writing lib32/"
for d in $DLLS; do
    [ -s "build/$d.list" ] || { echo "  $d: unused, skipped"; continue; }
    { echo "LIBRARY \"$d.dll\""; echo "EXPORTS"; cat "build/$d.list"; } \
        > "lib32/$d.def"
    dlltool -m i386 --as-flags=--32 -k -d "lib32/$d.def" -l "lib32/lib$d.a"
    printf "  %-10s %2d imports\n" "$d" "$(wc -l < "build/$d.list")"
done

echo "done."
