# About This Computer (`lunduke-about`)

Classic Mac OS 9–inspired **About This Computer** window for LCOS.
Replaces Software-menu **About This Computer** (`xfce4-about.desktop` → `lunduke-about`) in LCOS live-07.

**v0.2.7** — RAM accounting: app rows RssAnon only; LCOS System = physical used − Σ RssAnon (RssShmem/RssFile in System).

**v0.2.6** — Shorter window (~2½ app rows); Supporters: Fuzzy + [Your Name Here]; logo left / Supporters right.

**v0.2.4** — Supporters marquee placeholder `[Your Name Here]`.

**v0.2.3** — LCOS System row at bottom of application list.

**v0.2.2** — LCOS outline window/menu icon; theme background (`@theme_bg_color`);
full-width RAM Used/Free bar; LCOS System row; Software menu Name
`About This Computer`.

**v0.2.1** — Label tweaks: `OS Version:` / `CPU:` (was `LCOS version:` / `System CPU:`).

**v0.2** — UI polish: full LCOS mark (rings), two-column system stats, left-aligned
app names, `MB RAM Used` text (no per-row bars), hide self from the app list, resizable
with a 520×360 minimum.

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

### Supporters of LCOS

Top-right static text block (right-justified): title, blank line, then names from
`data/supporters.txt` (comma-separated or one name per line; `#` comments).
Loaded at startup — no rebuild needed when running against the source `data/` path
or after reinstall. Layout leaves room for a future upward movie-credits scroll.

### Logo

Header image ships Bob’s **full** black LCOS mark (`data/pixmaps/lcos-logo-black.*`) —
circular rings/arcs, banner box, and wordmark.

Window / Software-menu icon uses the **simple LCOS outline** seal
(`org.lunduke.AboutThisComputer` hicolor PNGs), matching `lcos32.png` / `lcos-logo.png`
— not the full rings mark and not a generic document icon.

## Features (v0.2.7)

- OS Version from `/etc/os-release` (falls back to LCOS live recipe sample if host isn’t LCOS)
- Total RAM, CPU model (`/proc/cpuinfo`), GPU (best-effort `lspci`) in a two-column stats row
- Full-width Mac OS 9–style RAM bar (`X RAM Used` / `X RAM Free` overlays)
- Right-justified “Supporters of LCOS” text block (logo left)
- Graphical apps, then **LCOS System** at bottom (system RAM remainder; not Force-Closable)
- App private RAM (RssAnon) as `N MB RAM Used`; LCOS System holds residual physical used (shared/RssFile/RssShmem once); empty-list copy: `No Running Software.`
- About This Computer itself is omitted from the list
- Right-click → **Force Close** (confirm → SIGKILL); protected rows have no menu action
- Theme window background (`@theme_bg_color`); white app-list frame
- Window resizable; minimum size 520×360 (v0.1 default layout)

## Package (.deb)

```bash
./packaging/build-deb.sh
# → packaging/debs/lunduke-about_0.2.7-1_amd64.deb
```

Installs `/usr/bin/lunduke-about`, data under `/usr/share/lunduke-about/`,
desktop file, and hicolor outline icons.

## License

GPL-3.0-or-later
