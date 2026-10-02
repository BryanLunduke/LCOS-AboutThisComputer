# lunduke-about NOTES

## v0.8.2

- Supporters of LCOS: join names with comma-space (`"Fuzzy", Steven P., Chris Hammond`), right-align, and wrap beside the logo (no ellipsis)
- Slow upward credits crawl of the names only when the wrapped lines are too tall for the header next to the logo; fully static when they fit (no idle animation). Title stays fixed
- Meson project version **0.8.2**; Debian **0.8-3** (`lunduke-about_0.8-3_amd64.deb`)

## v0.8.1

- Supporters of LCOS: add Chris Hammond (after "Fuzzy", Steven P.)
- Meson project version **0.8.1**; Debian **0.8-2** (`lunduke-about_0.8-2_amd64.deb`)

## v0.8

- LCOS 0.8 track identity bump. Features and UI unchanged from 0.7
- Meson project version **0.8.0**; Debian **0.8-1** (`lunduke-about_0.8-1_amd64.deb`)

## v0.7

- LCOS 0.7 track identity bump. Features and UI unchanged from 0.2.8
- Meson project version **0.7.0**; Debian **0.7-1** (`lunduke-about_0.7-1_amd64.deb`)

## v0.2.7

- RAM accounting: GUI app rows report **RssAnon only** (private heap)
- **LCOS System** = physical used (MemTotal − MemAvailable) − Σ listed-app RssAnon
- RssShmem / RssFile / shared library pages stay in LCOS System (once)
- Label still `N MB RAM Used`; Force Close / bar / Supporters / size unchanged
- Identity / Debian **0.2.7** / **0.2.7-1**

## v0.2.6

- Shorter default/minimum window (~2½ application rows visible in the list)
- Supporters text: title, blank, `"Fuzzy", Steven P.`, blank, `[Your Name Here]`
- `data/supporters.txt` blank lines preserved (multiline names label)
- Identity / Debian **0.2.6** / **0.2.6-1**

## v0.2.5

- Remove Supporters marquee/ticker entirely
- LCOS logo on the **left**; static **Supporters of LCOS** text block on the right (right-justified): title, blank line, `[Your Name Here]`
- Same font as OS Version / stats (`.about-info`); `data/supporters.txt` remains the name source
- Identity / Debian **0.2.5** / **0.2.5-1**


## v0.2.4

- Supporters marquee → **`[Your Name Here]`** (no Alice/Bob list)
- Identity / Debian **0.2.4** / **0.2.4-1**

## v0.2.3

- Move **LCOS System** row to the **bottom** of the application list (still protected)
- Identity / Debian **0.2.3** / **0.2.3-1**

## v0.2.2 (editor notes)

- Window / Software menu icon: simple LCOS outline seal (`org.lunduke.AboutThisComputer`), not document / not full rings mark
- Window background: `@theme_bg_color` (Paint-style theme), not hardcoded `#c0c0c0`
- Full-width RAM bar with **"X RAM Used"** / **"X RAM Free"** overlays (omit free text when free is 0)
- List row **"LCOS System"** (system remainder RAM; not Force-Closable); always shown
- Empty list copy → **"No Running Software."** (UI prefers LCOS System alone when no GUI apps)
- Software menu Name → **"About This Computer"**
- Identity / Debian **0.2.2** / **0.2.2-1**

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
