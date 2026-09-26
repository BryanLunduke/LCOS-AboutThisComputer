# About This Computer (`lunduke-about`)

Classic Mac OS 9–inspired **About This Computer** window for LCOS.
Replaces `xfce4-about` later (not wired yet — visual review first).

**v0.1** — window UI, real system/process data, Force Close via right-click.

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

Ships Bob’s black LCOS logo (`data/pixmaps/lcos-logo-black.*`). Does **not** use
the old color `/usr/share/pixmaps/lcos-logo.png`.

## Features (v0.1)

- LCOS version from `/etc/os-release` (falls back to LCOS live recipe sample if host isn’t LCOS)
- Total RAM, CPU model (`/proc/cpuinfo`), GPU (best-effort `lspci`)
- Scrolling “Supporters of LCOS” marquee
- List of graphical toplevel apps (X11 `_NET_CLIENT_LIST` / WM_STATE), RSS + memory bar
- Right-click → **Force Close** (confirm → SIGKILL); skips self and session-critical bits

## License

GPL-3.0-or-later
