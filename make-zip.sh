#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

./make-exe.sh

ver=$(sed -n 's/^const wchar_t SPEEDCOPY_VERSION\[\] = L"\([^"]*\)";/\1/p' speedcopy.c)
ver=${ver%$'\r'}
if [ -z "$ver" ]; then
	echo "cannot read SPEEDCOPY_VERSION from speedcopy.c" >&2
	exit 1
fi

out="SpeedCopy_v${ver}.zip"
rm -f "$out"
python3 - "$out" speedcopy.exe README.md LICENSE <<'PY'
import sys
import zipfile

out, names = sys.argv[1], sys.argv[2:]
with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
    for name in names:
        zf.write(name, arcname=name)
with zipfile.ZipFile(out) as zf:
    for info in zf.infolist():
        print(f"{info.file_size:8d}  {info.filename}")
PY

echo "ready: $out"
ls -l "$out"
