# lunduke-about NOTES

## v0.2.1 (label tweaks)

- "LCOS version:" → "OS Version:"
- "System CPU:" → "CPU:"
- Identity / Debian **0.2.1** / **0.2.1-1**
- Deb rebuilt; seeded into lcos-live-07 packages.chroot + packaging/debs

## v0.2 (UI-only pass)

Editor review notes implemented; deb/menu still held (resolved in packaging commit).

- Left-justified running application names
- System stats slightly larger + bold; two columns (OS Version + Built-in Memory | CPU + GPU)
- Full LCOS mark from `/workspace/artifacts/lcos-logo-black.svg` / `lcos-logo-black-preview.png` re-copied and re-baked into `data/pixmaps/` (128/256 from 512 preview)
- Resizable with Gtk min size 520×480 (v0.1 default)
- Removed RAM progress bars; append ` RAM Used` after MB figure
- Hide About This Computer entirely from the app list

Identity bumped to **0.2**. Packaging landed in a follow-up commit.
