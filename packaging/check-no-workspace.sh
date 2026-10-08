#!/bin/sh
# Fail if the lunduke-about binary inside a .deb contains a /workspace path.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: check-no-workspace.sh package.deb" >&2
  exit 2
fi

DEB=$1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
dpkg-deb -x "$DEB" "$TMP"
BIN="$TMP/usr/bin/lunduke-about"
if [ ! -f "$BIN" ]; then
  echo "missing $BIN" >&2
  exit 1
fi

hits="$TMP/hits.txt"
: > "$hits"
if strings "$BIN" | grep -F '/workspace' >> "$hits"; then
  :
fi
if grep -a -F -o '/workspace' "$BIN" >> "$hits"; then
  :
fi
if [ -s "$hits" ]; then
  echo "binary contains /workspace:" >&2
  cat "$hits" >&2
  exit 1
fi
echo "ok: $BIN has no /workspace string"
