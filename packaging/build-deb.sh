#!/bin/sh
# Build lunduke-about_0.9-4_amd64.deb into packaging/debs/ (repo-local).
# Does NOT seed lcos-live-07 (Phil seeds by hand into packages.chroot).
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
VERSION="0.9-4"
PKGNAME="lunduke-about_${VERSION}_amd64"
BUILD="$ROOT/build-deb"
DEST="$ROOT/packaging/src/lunduke-about"
DEB_DIR="$ROOT/packaging/debs"

cd "$ROOT"

rm -rf "$BUILD"
meson setup "$BUILD" --prefix=/usr --buildtype=release -Dstrip=true
meson compile -C "$BUILD"
# A red logic test must not produce a .deb.
meson test -C "$BUILD"

rm -rf "$DEST"
meson install -C "$BUILD" --destdir "$DEST"

# Ensure hicolor outline icons are present (meson installs them; reinforce copy).
for size in 16x16 32x32 48x48 128x128 256x256; do
  mkdir -p "$DEST/usr/share/icons/hicolor/${size}/apps"
  cp -a "$ROOT/data/icons/hicolor/${size}/apps/org.lunduke.AboutThisComputer.png" \
    "$DEST/usr/share/icons/hicolor/${size}/apps/org.lunduke.AboutThisComputer.png"
done

mkdir -p "$DEST/debian"
cp "$ROOT/debian/control" "$DEST/debian/control"

SHLIBS="$(
  cd "$DEST"
  dpkg-shlibdeps --ignore-missing-info -O \
    -e usr/bin/lunduke-about
)"
SHLIBS_DEPS="${SHLIBS#shlibs:Depends=}"

SIZE="$(du -sk "$DEST/usr" | awk '{print $1}')"

mkdir -p "$DEST/DEBIAN"
cat > "$DEST/DEBIAN/control" << CTRL
Package: lunduke-about
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: amd64
Installed-Size: ${SIZE}
Maintainer: LCOS <lcos@lunduke.com>
Homepage: https://lunduke.com
Depends: ${SHLIBS_DEPS}, desktop-file-utils, gtk-update-icon-cache
Description: About This Computer for LCOS (Mac OS 9 style)
 Classic Mac OS 9–inspired About This Computer window for the Lunduke
 Computer Operating System. Shows LCOS version, RAM, CPU, GPU, a
 Supporters of LCOS block, and running graphical apps with Force Close.
CTRL

cat > "$DEST/DEBIAN/postinst" << 'POST'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
  fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
  fi
fi
exit 0
POST
chmod 0755 "$DEST/DEBIAN/postinst"

(
  cd "$DEST"
  find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)

rm -rf "$DEST/debian"

mkdir -p "$DEB_DIR"
fakeroot dpkg-deb --root-owner-group --build "$DEST" "$DEB_DIR/${PKGNAME}.deb"

echo "built $DEB_DIR/${PKGNAME}.deb"
