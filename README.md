# About This Computer (`lunduke-about`)

Classic Mac OS 9–inspired **About This Computer** window for LCOS.
Replaces Software-menu **About your Computer** (`xfce4-about.desktop` → `lunduke-about`) in LCOS live-07.

**v0.2** — UI polish: full LCOS mark (rings), two-column system stats, left-aligned
app names, `MB RAM Used` text (no bars), hide self from the app list, resizable
with a 520×480 minimum.

## Requirements

- C++17 toolchain
- Meson ≥ 0.56, Ninja
- gtkmm-3.0 (≥ 3.24)
- gdk-x11-3.0, libX11
- gdk-pixbuf-2.0

## Build

```bash
cd /workspace/lunduke-about   # or your checkout
meson setup build
meson compile -C build
```

Binary: `build/lunduke-about`

## Run

Needs an X11 display (GDK_BACKEND=x11 is set automatically if unset):

```bash
./build/lunduke-about
```

Data files (`supporters.txt`, logo pixmaps) are resolved from:

1. Install prefix `share/lunduke-about/`
2. Source-tree `data/` (so running from the builddir works without install)

### Supporters marquee

Edit `data/supporters.txt` (comma-separated or one name per line; `#` comments).
Loaded at startup — no rebuild needed when running against the source `data/` path
or after reinstall.

### Logo

Ships Bob’s **full** black LCOS mark (`data/pixmaps/lcos-logo-black.*`) — circular
rings/arcs, banner box, and wordmark — copied from the canonical branding assets.
Does **not** use the old color `/usr/share/pixmaps/lcos-logo.png` or the wordmark-only bake.

## Features (v0.2)

- LCOS version from `/etc/os-release` (falls back to LCOS live recipe sample if host isn’t LCOS)
- Total RAM, CPU model (`/proc/cpuinfo`), GPU (best-effort `lspci`) in a two-column stats row
- Scrolling “Supporters of LCOS” marquee
- List of graphical toplevel apps (X11 `_NET_CLIENT_LIST` / WM_STATE), RSS as `N MB RAM Used`
- About This Computer itself is omitted from the list
- Right-click → **Force Close** (confirm → SIGKILL); skips session-critical bits
- Window resizable; minimum size 520×480 (v0.1 default layout)


## Package (.deb)

```bash
./packaging/build-deb.sh
# → packaging/debs/lunduke-about_0.2-1_amd64.deb
```

Installs `/usr/bin/lunduke-about`, data under `/usr/share/lunduke-about/`,
desktop file, and hicolor icons.

## License

GPL-3.0-or-later
