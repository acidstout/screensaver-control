@echo off
rem ===========================================================================
rem  build.bat - Bildschirmschoner-Steuerung / Screen Saver Control
rem
rem  Produces a 32-bit scrctl.exe that runs on Windows 95 through Windows 11.
rem
rem  Requires MinGW-w64 gcc/windres/dlltool on PATH.  The toolchain may be a
rem  x86_64 one: nothing here links against a 32-bit CRT, only against the
rem  import libraries in lib32\ (regenerate with tools\genimportlibs.sh when
rem  the set of Win32 APIs used by scrctl.c changes).
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist build mkdir build

set CFLAGS=-m32 -O2 -nostdlib -nostartfiles -nodefaultlibs ^
 -fno-ident -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables ^
 -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter ^
 -Wno-cast-function-type

rem --subsystem windows:4.0 and --major-os-version 4 are what mark the image as
rem loadable by the Windows 95 / NT 4 PE loader.
set LDFLAGS=-m32 -nostdlib -nostartfiles -nodefaultlibs -Llib32 ^
 -Wl,--gc-sections -Wl,--entry,_WinMainCRTStartup ^
 -Wl,--subsystem,windows:4.0 -Wl,--major-os-version,4 -Wl,--minor-os-version,0 ^
 -Wl,--file-alignment,512 -Wl,-s
set LIBS=-lkernel32 -luser32 -lgdi32 -lshell32 -ladvapi32

echo [1/3] compiling scrctl.c
gcc %CFLAGS% -c scrctl.c -o build\scrctl.o
if errorlevel 1 goto :fail

echo [2/3] compiling resources
rem codepage 1252 makes the \xNN escapes in the .rc files mean Latin-1.
windres --codepage=1252 -F pe-i386 -O coff -I. -i scrctl.rc -o build\scrctl.res.o
if errorlevel 1 goto :fail

echo [3/3] linking
gcc %LDFLAGS% -o build\scrctl.exe build\scrctl.o build\scrctl.res.o %LIBS%
if errorlevel 1 goto :fail

echo.
echo Built build\scrctl.exe
for %%F in (build\scrctl.exe) do echo Size: %%~zF bytes
goto :eof

:fail
echo.
echo BUILD FAILED
exit /b 1
