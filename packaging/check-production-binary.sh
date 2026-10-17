#!/bin/sh
# Fail if a production lunduke-about binary contains a developer workspace
# path or a test-only environment hook. The release build leaves those
# hooks out at compile time.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: check-production-binary.sh binary [buildtype]" >&2
  exit 2
fi

BIN=$1
# Only release-type builds strip the checkout path (meson test: 77 = SKIP),
# so a debug build of a clean checkout is not a red suite.
case "${2:-release}" in
  release|plain) ;;
  *) echo "SKIP: buildtype '$2' is not a release build"; exit 77 ;;
esac
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
