#!/bin/bash
set -e

PKGDIR="${MESON_BUILD_ROOT}/pkg"

mkdir -p "$PKGDIR"
cd "$PKGDIR"

mkdir -p spout2pw-dlls/x86_64-windows
cp "${MESON_BUILD_ROOT}/spout2pw.exe" spout2pw-dlls/x86_64-windows
# Provide a spout2pw.dll so LoadLibraryW("spout2pw.dll") in WinMain succeeds.
# Marked as a Wine builtin DLL so it also maps to the spout2pw.so unixlib.
cp "${MESON_BUILD_ROOT}/spout2pw.exe" spout2pw-dlls/x86_64-windows/spout2pw.dll
mkdir -p spout2pw-dlls/x86_64-unix
cp "${MESON_BUILD_ROOT}/spout2pw_unixlib.so" spout2pw-dlls/x86_64-unix/spout2pw.so
cp "${MESON_SOURCE_ROOT}/misc/spout2pw.inf" .
cp "${MESON_SOURCE_ROOT}/misc/spout2pw.sh" .

# Mark both PE files as Wine builtin DLLs so Wine automatically loads the
# matching spout2pw.so unixlib from WINEDLLPATH/x86_64-unix/ when either is
# loaded. This registers the module in builtin_modules, allowing
# __wine_init_unix_call() to succeed.
for i in spout2pw-dlls/x86_64-windows/*; do
    echo -ne "Wine builtin DLL\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" | \
        dd of=$i bs=1 seek=64 conv=notrunc \
        2>/dev/null
done
