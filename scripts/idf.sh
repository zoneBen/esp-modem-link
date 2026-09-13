#!/usr/bin/env bash
# Git Bash front end for scripts/idf.ps1.
#
#   ./scripts/idf.sh build
#   ./scripts/idf.sh -p COM7 flash monitor
#   ./scripts/idf.sh              # set up and print how to continue
#
# The environment setup lives in idf.ps1, because ESP-IDF's own activation is
# PowerShell on Windows; this only translates the call. It exists because idf.py
# cannot be driven from this shell directly: idf.py sees MSYSTEM, concludes it is
# on MSys/Mingw, and does nothing at all - and IDF's activation script refuses
# outright. idf.ps1 drops MSYSTEM before either runs.

set -euo pipefail

for tool in cygpath powershell.exe; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "idf.sh: $tool not found - run this from Git Bash on Windows." >&2
    exit 1
  fi
done

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps1=$(cygpath -w "$script_dir/idf.ps1")

exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ps1" "$@"
