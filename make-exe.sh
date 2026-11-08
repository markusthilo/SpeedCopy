#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

# MinGW-w64, Windows 10/11, 64-bit. Static console exe: no MinGW DLLs beside it.
MINGW_CC=${MINGW_CC:-x86_64-w64-mingw32-gcc}
MINGW_WINDRES=${MINGW_WINDRES:-x86_64-w64-mingw32-windres}
MINGW_OBJDUMP=${MINGW_OBJDUMP:-x86_64-w64-mingw32-objdump}
OUT=speedcopy.exe

if [ ! -f icons/speedcopy.ico ]; then
	echo "missing icons/speedcopy.ico" >&2
	exit 1
fi

mkdir -p build

# speedcopy.rc embeds speedcopy.manifest (long paths, asInvoker) and icons/speedcopy.ico
echo "$MINGW_WINDRES" -O coff --target=pe-x86-64 -o build/speedcopy_res.o speedcopy.rc
"$MINGW_WINDRES" -O coff --target=pe-x86-64 -o build/speedcopy_res.o speedcopy.rc

echo "$MINGW_CC" -O2 -s -static -mconsole -o "$OUT" speedcopy.c speedcopy_gui.c build/speedcopy_res.o -lcomdlg32 -lshell32 -lole32 -lgdi32 -lcomctl32 -luuid
"$MINGW_CC" -O2 -s -static -mconsole -o "$OUT" speedcopy.c speedcopy_gui.c build/speedcopy_res.o -lcomdlg32 -lshell32 -lole32 -lgdi32 -lcomctl32 -luuid

# Windows 10/11 system libraries only. api-ms-win-crt-* is the system UCRT, not a DLL to ship.
echo "runtime DLLs:"
"$MINGW_OBJDUMP" -p "$OUT" | grep 'DLL Name:'
if "$MINGW_OBJDUMP" -p "$OUT" | grep 'DLL Name:' | grep -Ei 'libgcc|libwinpthread|libstdc|mingw'; then
	echo "unexpected runtime DLL (want a single portable exe)" >&2
	exit 1
fi

echo "ready: $OUT"
ls -l "$OUT"
