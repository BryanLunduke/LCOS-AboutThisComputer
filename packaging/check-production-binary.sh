#!/bin/sh
# Fail if a production lunduke-about binary contains a developer workspace
# path or a test-only environment hook. The release build leaves those
# hooks out at compile time.
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: check-production-binary.sh binary" >&2
  exit 2
fi

BIN=$1
if [ ! -f "$BIN" ]; then
  echo "missing $BIN" >&2
  exit 1
fi

hits="$(mktemp)"
trap 'rm -f "$hits"' EXIT
: > "$hits"

scan() {
  needle=$1
  if strings "$BIN" | grep -F "$needle" >> "$hits"; then
    :
  fi
  if grep -a -F -o "$needle" "$BIN" >> "$hits"; then
    :
  fi
}

scan '/workspace'
scan 'LUNDUKE_ABOUT_OS_RELEASE'
scan 'LUNDUKE_SUPPORTERS_TXT'
scan 'LUNDUKE_ABOUT_TEST'

if [ -s "$hits" ]; then
  echo "production binary contains a workspace path or a test hook:" >&2
  sort -u "$hits" >&2
  exit 1
fi
echo "ok: $BIN has no /workspace string or test hook"
