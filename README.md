# fallout2-ce-ps4 (overlay)

A **thin overlay** that turns upstream [fallout2-ce](https://github.com/fallout2-ce/fallout2-ce)
into a PlayStation 4 (OpenOrbis homebrew) build. Upstream is consumed **pristine**
(cloned at a target commit) — this repo carries only the PS4 delta, so bumping
upstream is trivial and a CE "revolution" breaks at most a few 3-line patches,
never a big merge.

- `overlay/` — new PS4 files (build machinery, platform layer, gamepad, docs).
  Copied on top of pristine upstream. **Zero patches needed.**
- `patches/` — 18 small `#ifdef __PS4__` hook patches into upstream files.
- `scripts/apply.sh` — clone upstream@ref + overlay + patches + private modules → buildable `tree/`.
- `scripts/build.sh` — `apply.sh` + build + package → `*.pkg`.
- `scripts/config.sh` — which upstream ref to build (`UPSTREAM_REF`; `main` = latest CE).
- `.github/workflows/build.yml` — on-demand cloud build (see **Building** below).

---

## Running on the console — what you need

You supply everything below; **no game data or Sony files live in this repo.**

### 1. Game data
This port is the **engine only**. Transfer the files below (FTP, e.g. GoldHEN's FTP
server) to **`/data/fallout2/`** on the console.

**Required** (the engine won't run without these):

```
/data/fallout2/
├── master.dat        # original Fallout 2 (you own it)
├── critter.dat       # original Fallout 2
├── patch000.dat      # original Fallout 2 official patch
├── data/             # loose data / patches
└── ce.dat            # Community Edition assets — see note below
```

**Community Edition needs `ce.dat`.** It's not part of the original game — it's a
resource that ships with fallout2-ce. Just copy it **1:1 from a fallout2-ce
release** (its `ce.dat` download) into `/data/fallout2/`. The engine build doesn't
produce it; it's a straight file copy. It rarely changes, so a recent one is fine.

**Optional (but recommended):**

```
├── sound/            # music + sfx (case-insensitive — see note)
├── f2_res.dat        # high-resolution assets (interface bar, menus, ...).
│                     #   Recommended for widescreen/1080p or the interface bar
│                     #   is drawn at the old 640-wide size. Not required to boot.
├── f2_res.ini        # legacy — auto-migrated into fallout2.cfg on first run; can delete
```

**Where to get `ce.dat` and `f2_res.dat`:** both live in fallout2-ce's own
[`files/` folder](https://github.com/fallout2-ce/fallout2-ce/tree/main/files)
(alongside `f2_res.ini`, `ddraw.ini` and a default `fallout2.cfg`). Copy them
**1:1** into `/data/fallout2/` — nothing to build. `ce.dat` there is a folder of
sources on GitHub; use the packed `ce.dat` from a fallout2-ce **release**.

**Auto-created / yours to configure:**

```
├── fallout2.cfg      # engine config — set your resolution here
└── ps4_controls.cfg  # gamepad config — AUTO-CREATED on first launch
```

`ddraw.ini`, `worldmap.dat` and similar are **not** used by CE (old sfall/HRP
leftovers) — harmless, just not required.

- **Case-insensitive:** the port resolves file casing automatically, so `sound/`,
  `Sound/`, `SOUND/` (and any file) all work — no manual renaming.
- **Resolution:** set your screen size in `fallout2.cfg` `[screen]`
  (`resolution_x` / `resolution_y`, e.g. 1920×1080 for 1080p widescreen).

### 2. The two GLES modules (once, on the console)
The default build ships **no Sony modules** (so it's publish-safe). Copy them once
to the shared console path (any OpenOrbis-SDL2 homebrew, e.g. the SM64 port, uses
the same files):

```
/data/self/system/common/lib/libScePigletv2VSH.sprx
/data/self/system/common/lib/libSceShaccVSH.sprx
```

Without them the app closes at launch with `PRX_NOT_RESOLVED 0xa0020101`.

### 3. Install the pkg
Install `IV0001-FALL20002_00-FALL200020100000.pkg` (GoldHEN Debug Settings → PKG
installer, or your usual method), then launch **Fallout II Community Edition**.

### 4. Saves
Save games live under `/data/fallout2/` (the engine's `SAVEGAME` dir) — they
persist across pkg reinstalls.

---

## Controls

### Default mapping

| Input | Action |
|---|---|
| **Left stick** | Move the camera (cursor pinned to screen centre, frees at the map edge) — *centered* mode |
| **Right stick** | Free cursor (trackpad-like pointer) |
| **Touchpad — slide finger** | Move the cursor; a quick tap = left click |
| **Touchpad — physical press** | Pipboy |
| **Cross ✕** | Left click / confirm — *(during aim: fire / interact)* |
| **Circle ◯** | Right click (cycle cursor mode) — *and closes any open menu* |
| **Square ▢** | Skilldex |
| **Triangle △** | Character sheet |
| **L1** | Inventory |
| **R1** | Reload the active weapon |
| **L2 (hold)** | **Aim assist** — combat: lock onto enemies; out of combat: onto people to talk to / interact |
| **R2 (with L2)** | Held during L2+✕ → **aimed / called shot (VATS)** |
| **L3 (left stick click)** | Switch to the other equipped weapon |
| **R3 (right stick click)** | Combat: start a fight / end turn — or end the fight when no enemy still wants to attack |
| **D-pad** | Arrow keys (menu navigation); **←/→ cycle targets** while aiming |
| **Options** | Game menu (Save / Load / Preferences / Exit) |

### Aim assist (hold L2)

- **In combat:** the reticle snaps to the nearest enemy (crosshair cursor, shows the
  hit %). **D-pad ←/→** cycles targets. **✕** fires; **L2 + R2 + ✕** does an aimed
  (called) shot — pick a body part, **✕** to fire, **◯**/Cancel to back out.
- **Melee / unarmed, target out of reach:** the engine normally just says *"out of
  range"*. Instead the message monitor (bottom-left) previews the move —
  *"Reach 3 + attack 4 = 7 AP (have 9), hit ~85%"* — recomputed as you cycle
  targets (the `hit ~%` is the chance **once you're adjacent**, since the reticle
  can't show a % out of range). **✕** then runs up to the target the way an enemy
  would; whether it also swings on arrival is the `melee_approach` setting below.
- **Out of combat:** the cursor snaps to the nearest person with the *talk/look/use*
  action cursor; **✕** interacts (e.g. starts dialog).
- The target set is **frozen** while L2 is held (D-pad only cycles what was in view
  when you pressed L2). Release and re-press L2 to re-scan.

---

## Modifying the controls

All gamepad behaviour is in **`/data/fallout2/ps4_controls.cfg`** — a plain text
file you can edit over **FTP** and re-launch (no rebuild). **Delete the file** to
regenerate it fresh with every option and inline comments.

### Remapping buttons

Assign one **action** to each button key. Actions:

| Action | Does |
|---|---|
| `lmb` | Left click / confirm |
| `rmb` | Right click (cycle cursor mode) |
| `esc` | Escape (game menu / cancel / skip movies) |
| `inventory` | Open inventory (`i`) |
| `charsheet` | Character sheet (`c`) |
| `skilldex` | Skilldex (`s`) |
| `pipboy` | Pipboy (`p`) |
| `reload` | Reload the active-hand weapon (spends AP in combat) |
| `weaponmode` | Cycle the weapon's attack mode (single / aimed / burst / reload — the "red button") |
| `swaphands` | Switch to the other equipped weapon |
| `combat` | Start a fight (out) / end turn — or end the fight when safe (in) |
| `endcombat` | Force-leave the fight |
| `boost` | Hold for a faster stick cursor |
| `none` | Nothing |

Button keys (defaults in **bold**):

```
btn_cross    = lmb          btn_l1      = inventory
btn_circle   = rmb          btn_r1      = reload
btn_square   = skilldex     btn_l3      = swaphands
btn_triangle = charsheet    btn_r3      = combat
btn_options  = esc          btn_share   = pipboy
```

Notes:
- **`btn_share` controls the touchpad CLICK** (a physical press of the pad), *not*
  the Share button — the DS4 Share button is OS-reserved on PS4 and can't be read.
  The key just kept the "share" name. Its default action is `pipboy`, so **pressing
  the touchpad opens the Pipboy**. Remap it like any other button
  (e.g. `btn_share=inventory`). *(Sliding a finger / tapping the pad is still the
  cursor + left click — that's separate from the physical click.)*
- The **D-pad** and **L2/R2** aim are fixed (not remappable).

### Stick roles in the world

`world_left_stick` / `world_right_stick` — each stick independently:

| Value | Behaviour |
|---|---|
| `centered` | Pan the camera with the cursor locked to screen centre; frees at the map edge *(default left)* |
| `cursor` | Free trackpad-like cursor *(default right)* |
| `camera` | Pan the view (cursor stays where it is) |
| `walk` | March the character (walk/run by push) — left stick only |
| `off` | Nothing |

```
world_left_stick  = centered
world_right_stick = cursor
```

### Aim assist options

```
combat_auto_aim        = 1    # 1 = snap onto the nearest enemy in combat (0 = off)
world_auto_aim         = 1    # 1 = snap onto people to talk to/interact out of combat
auto_aim_whole_map     = 0    # 0 = only targets on screen, 1 = the whole map
auto_aim_center_camera = 0    # 1 = pan the camera so the aimed target is centred
melee_approach         = 1    # ✕ on an out-of-reach melee/unarmed target:
                              #   0 = off (engine "out of range")
                              #   1 = run up to it but DON'T swing (default)
                              #   2 = run up and then attack
```

### Cursor & touchpad feel

```
cursor_speed        = 450     # stick cursor px/sec (combat/menus)
cursor_speed_boost  = 900     # ...with the 'boost' button held
touch_sensitivity   = 0.25    # touchpad gain when moving SLOWLY (lower = more precise)
touch_accel         = 0.001   # extra gain when moving FAST (0 = linear)
touch_smoothing     = 0.40    # 0..0.9 — higher = smoother but a touch laggier
touch_deadzone      = 2.0     # raise if the cursor drifts with a finger at rest
touch_max_gain      = 4.0     # cap on touchpad acceleration
touch_tap_ms        = 200     # a touch shorter than this counts as a click
touch_tap_move      = 40      # ...and smaller movement than this
```

### Camera & advanced

```
camera_deadzone_x    = 100    # camera-follow dead-zone (px)
camera_deadzone_y    = 75
camera_pan_threshold = 0.40   # stick deflection (0..1) needed to pan
dpad_repeat_ms       = 150    # D-pad arrow repeat speed (lower = faster)
# only matter if a stick is set to `walk`:
walk_deadzone        = 0.30
run_threshold        = 0.50
walk_project_tiles   = 2
run_settle_ms        = 50
```

---

## Building

### On demand (cloud, GitHub Actions)
Actions → **build-ps4** → **Run workflow**. It clones fallout2-ce, applies the
overlay + patches, builds with the OpenOrbis toolchain, and then **publishes the
pkg as a GitHub Release** (tagged `build-<n>`, newest on top; the title carries the
exact CE commit) — it's also uploaded as a workflow artifact. Grab the pkg from the
[Releases page](../../releases). Inputs:

- `upstream_ref` — blank for the pinned CE commit, or `main` for the latest CE.
- `publish_release` — on by default; uncheck for a test build (artifact only, no
  Release).

Builds are **manual only** — there is deliberately no nightly/scheduled trigger
(fallout2-ce is experimental and commits daily, so you build when *you* want a new
pkg). The toolchain is seeded once as a release asset — see
`scripts/seed-toolchain.md`. No secrets, no proprietary files required.

### Locally
```sh
source $OPENORBIS/ps4vars.sh
scripts/build.sh            # -> ./IV0001-...pkg  (+ build-manifest.txt)
```
`build-manifest.txt` records the exact upstream commit built against (per-build
snapshot — reproducible even while tracking latest).

### Custom app icon (optional)
The pkg ships with a plain placeholder icon. To use your own, drop a **512×512
`icon0.png`** at the repo root before building — `scripts/apply.sh` feeds it in and
the pkg uses it instead of the placeholder (or pass `-DPS4_ICON=/path/to.png`). It's
**gitignored**, so the repo stays free of any non-free artwork; if it's absent the
placeholder is used. Note: PS4 icons are **opaque** — the system ignores alpha
(transparent pixels render black) and rounds the corners itself, so flatten your art
onto a solid background.

---

## Proprietary Sony modules — not in this repo, not needed to build

The default (`PS4_MODULES_ON_CONSOLE=ON`) produces a pkg with **no Sony modules**;
the build needs none (verified by building with an empty `private/`). The end user
supplies the two `.sprx` on the console (see *Running on the console*). Everything
proprietary (`private/`, all `*.sprx`) is git-ignored, so nothing lands in the repo.

Optional self-contained pkg (modules bundled — but the pkg then contains Sony
modules): `-DPS4_MODULES_ON_CONSOLE=OFF` and drop the `.sprx` under `private/`
mirroring the tree paths.

---

## Provenance & docs

- Overlay + patches are generated from the working port; regenerate with
  `scripts/refresh.sh` against a pristine upstream diff.
- Pinned upstream ref: `scripts/config.sh`.
- **Porting other fallout2-ce ports?** `docs/porting-notes.md` maps every feature
  to the exact overlay file / patch that implements it (and flags what's plain
  engine code vs. OpenOrbis-specific), so you can lift the good parts.
