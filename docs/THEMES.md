# Reverie Themes

A **theme** is a wallpaper as a self-contained unit: its scene definition,
its metadata, and every asset it references. Themes come in two shapes with
identical content:

* **Folder themes** — development-friendly directories.
* **Packed themes** — the same files inside one `.revpack` container,
  loaded **without extraction** (entries are memory-mapped views).

Both are first-class: `reverie themes/ripple-lake` and
`reverie ripple-lake.revpack` produce **byte-identical** output.

---

## Folder layout

```
themes/
  ripple-lake/
    theme.json          <- metadata (this file makes a folder a theme)
    scene.rev           <- entry scene (or whatever "scene" says)
    assets/             <- images referenced by the scene
      lake.png
    shaders/            <- RESERVED: not loaded (see Security)
    videos/             <- video resources for video layers
```

Rules:

* All paths are **theme-relative** — a theme is relocatable and never
  depends on absolute paths or the working directory.
* The presence of `theme.json` is what turns a directory into a theme;
  plain `.rev` scene files keep working everywhere (a theme is a superset).
* `shaders/` is reserved for a future sandboxed effects system. It is
  deliberately NOT loaded today — see *Security*.

## theme.json

```json
{
  "id": "ripple-lake",
  "name": "Ripple Lake",
  "description": "A calm lake scene: drag the pointer across the water...",
  "author": "FemBoyGamerTechGuy",
  "version": "1.0.0",
  "scene": "scene.rev",
  "engine_min": "0.1.0",
  "preview": "assets/preview.png",
  "quality": [
    { "name": "low",    "fps": 30, "resolution_scale": 0.75 },
    { "name": "medium", "fps": 60, "resolution_scale": 1.0 }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Theme identifier (used by tooling) |
| `name` | no | Display name (defaults to `id`) |
| `description` | no | Human description |
| `author` | no | Author name |
| `version` | no | Theme version (informational) |
| `scene` | no | Entry scene, theme-relative (default `scene.rev`) |
| `engine_min` | no | Minimum engine version; older engines refuse to load with a clear message |
| `preview` | no | Preview image path (used by future pickers) |
| `quality` | no | Up to 4 named presets: `fps` (0 = scene default), `resolution_scale` (clamped 0.25..2.0) |

The engine parses theme.json with its own strict minimal JSON reader
(`src/json.c`) — objects, arrays, strings with escapes, numbers, booleans,
null. Trailing garbage is rejected.

## Resource interface

The scene loader resolves every `src = "assets/lake.png"` reference through
one interface (`theme.h: theme_asset`):

* folder themes: read from `<theme>/assets/lake.png`, cached on first use
  (bounded at 256 MB — a hostile theme cannot balloon the process);
* packed themes: direct view into the mapped container.

The engine never learns which side it is talking to, and a single
scene-loading implementation serves both (`rtscene_compile_assets`).

---

## Packed themes (`.revpack` v2)

A `.revpack` is a small checksummed container (see `src/pack.h` for the
byte layout). Version 2 turns it into a theme container:

* entry names are **nested relative paths** (`assets/lake.png`,
  `videos/loop.mp4`);
* one entry is `theme.json`;
* the entry scene occupies the header's scene slot;
* every entry carries a CRC-32, verified at load; the header CRC covers
  the directory.

Reading is on-demand and in place — nothing is extracted, and total
container size is capped at 2 GiB with a 512-entry limit.

### Security

* Entry paths are validated at load: absolute paths, `..` traversal, empty
  components, backslashes, and control characters are **rejected** (the
  whole pack is refused, with a message naming the offending entry).
* Themes carry **data, not code**: packing refuses `.so`/`.dll`/`.exe`
  entries and any file with an executable bit.
* `shaders/` and any `.frag`/`.vert` files are not loaded by the engine —
  a theme cannot ship executable GPU programs today. Custom shaders will
  arrive later behind an explicit, sandboxed effect model.
* The engine version gate (`engine_min`) refuses themes that need a newer
  engine instead of failing mysteriously mid-render.

---

## Tooling

```
revtool theme list themes/            # what's installed?
revtool theme info themes/ripple-lake # metadata
revtool theme validate themes/ripple-lake
revtool theme pack themes/ripple-lake -o ripple-lake.revpack
revtool theme inspect ripple-lake.revpack   # entries + sizes (+ CRC verified)
```

`validate` checks: metadata parses, the entry scene parses AND compiles,
every referenced asset resolves through the theme, and the engine version
gate passes.

## Running

```
reverie themes/ripple-lake          # folder theme
reverie ripple-lake.revpack         # packed theme
reverie scenes/ripple-lake.rev      # plain scene (unchanged)
reverie old-scene.revpack           # v1 scene pack (unchanged)
```

Hot-reload polls a snapshot of every file under the theme (scene,
`theme.json`, all assets) roughly every 0.5 s — folders, packs (the single
container file), and plain scenes alike. Editing any file, adding one, or
removing one reloads the theme; a failed reload keeps the old scene running
and logs the parse error. (Watching a directory's own mtime would never
fire on an in-place edit — a directory's mtime only moves when entries are
added or removed — so the watch walks the tree and compares
(path, mtime, size) records; bounded at 256 files / 5 levels deep.)

Themes can also be switched at RUNTIME without restarting the daemon —
`revtool ipc "THEME themes/snowfall 800"` switches with a dip-through-black
fade, and `PLAYLIST` + `ROTATE` cycle a set of themes unattended. Switching
re-arms the hot-reload watcher on the new theme (its snapshot is taken at
load time), so file edits on the active theme keep hot-reloading across
switches. See docs/IPC.md for the full control protocol.

## Compatibility

* v1 `.revpack` scene packs load exactly as before.
* Plain `.rev` scenes are unaffected.
* The bundled themes in `themes/` are the same five wallpapers as
  `scenes/`, reorganized — identical renders, verified byte-for-byte
  in the e2e suite.
