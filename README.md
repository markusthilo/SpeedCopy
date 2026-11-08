# SpeedCopy

Fast recursive copy for Windows 10/11 (64-bit), with a size or MD5 verify pass afterward.

One portable `speedcopy.exe`. No runtime DLLs to ship. With no arguments, a window opens. With arguments, it runs as a command-line tool.

## Features

- Copy files and folders into one destination directory or volume
- Each source is placed under its own name (`D:\photos` → `E:\backup\photos\...`)
- A drive root is placed directly (`D:\` → children of `E:\backup\`)
- Size verify by default; optional MD5 (`-m` / `-md5`)
- Optional UTF-8 CSV protocol (`-c` / `--csv`)
- Parallel jobs (`-j` / `--job`), default one job per CPU core
- Keeps creation, access, and write times, plus ordinary attributes
- Junctions and symbolic links are skipped
- Long paths are supported

## Usage

```text
speedcopy.exe [-f|--force] [-m|-md5] [-j|--job N] [-c|--csv FILE] [-s|--source PATH]... <destination>
speedcopy.exe -h|--help
```

Example:

```text
speedcopy.exe -s D:\photos -s D:\notes.txt E:\backup
```

Paths use `\` between directories. The command line never asks a question: without `-f` / `--force`, it stops when existing files would be overwritten. The window asks before it replaces a file.

## Build

Requires MinGW-w64 (`x86_64-w64-mingw32-gcc`, `windres`).

```bash
./make-exe.sh
```

Release zip (`speedcopy.exe`, `README.md`, `LICENSE`):

```bash
./make-zip.sh
```

## License

MIT. See [LICENSE](LICENSE).

Author: Markus Thilo \<markus.thilo@proton.me\>
