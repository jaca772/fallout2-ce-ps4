# Porting notes — what the PS4 delta does, and where it lives

This repo keeps upstream [fallout2-ce](https://github.com/fallout2-ce/fallout2-ce)
**pristine** and adds only the PS4 delta:

- **`overlay/`** — brand-new files copied on top of upstream (no patch needed).
- **`patches/`** — small `#ifdef __PS4__` hooks into existing upstream files.

So each feature below is either a whole overlay file or a focused patch. If you
maintain another fallout2-ce port (Vita, Switch, …), this is the map: most of the
**gameplay** logic is plain engine code and lifts almost verbatim; only the
**platform bring-up** is OpenOrbis-specific. Each item is tagged:

- 🟢 **engine-generic** — no PS4 API, portable to any platform as-is.
- 🟠 **platform-specific** — OpenOrbis/SDL-PS4/scePad; you'd swap the backend.

---

## Gamepad & controls

The entire pad layer is one overlay file, deliberately isolated from the engine so
it's easy to lift or replace:

- 🟠 **`overlay/src/platform/ps4/ps4_gamepad.cc` / `.h`** — pad → virtual
  mouse/keys, the two-stick scheme (walk / centered-camera / free-cursor), touchpad
  finger cursor (accel + smoothing), D-pad, and the **aim-assist** driver. Reads the
  pad through SDL plus native **scePad** for the buttons SDL can't (see below).
- 🟠 **`patches/0007-src_dinput_cc.patch` + `0008-src_dinput_h.patch`** — the single
  hook that calls into `ps4_gamepad` from the engine's input polling.
- 🟠 **`patches/0013-src_main_cc.patch`** — one line: run `ps4GamepadWorldMove()`
  per frame from the main loop (stick-walk + camera follow).
- 🟢 **`patches/0011-src_interface_cc.patch`** — `interfaceGamepadReload()`: reload
  the active-hand weapon inline, honoring AP in combat, without cycling the item
  action mode. Pure engine logic — portable directly.

**Pad-button quirks (PS4-specific, but instructive):** on DS4 via SDL-PS4, the
physical **Options** button arrives as `SDL_CONTROLLER_BUTTON_START` and the
**Share** button is OS-reserved (unreadable); the **touchpad click** and **L2/R2**
digital presses aren't exposed by SDL either. They're read from native **scePad**
bitmasks (`ORBIS_PAD_BUTTON_OPTIONS` / `TOUCH_PAD` / `L2` / `R2`). If your platform
has a different pad backend, this is where you'd re-map.

## Combat assist (aim / VATS / melee auto-approach)

All 🟢 **engine-generic** — this is the reusable heart. In
**`patches/0004-src_combat_cc.patch` (+ `0005-src_combat_h.patch`)**:

- `combatGamepadListTargets()` — nearest-first list of living hostiles, LOS-filtered;
  melee/unarmed uses **perception reach** so non-adjacent enemies are still targetable.
- `combatGamepadAimedAttack()` — forced aimed/called-shot ("VATS") on a target,
  regardless of the weapon's current mode.
- `combatGamepadApproachAndAttack(target, attackAfter)` — **melee auto-approach**:
  registers a run/walk-to-object move like the enemy AI (`animationRegister*ToObject`
  + `_combat_turn_run`, which blocks because `reg_anim_end()` bumps
  `_combat_turn_running` when `isInCombat()`), then optionally attacks now that it's
  adjacent. This is the "run in like the AI does" the base engine lacks.

The reticle % and the "Reach N + attack M = Z AP, hit ~P%" message-monitor preview
live in `ps4_gamepad.cc` (the % uses `_determine_to_hit_no_range`, the AI's
range-free to-hit, because the engine reticle shows nothing out of range).

## Filesystem & data

- 🟢 **`patches/0016-src_platform_compat_cc.patch`** — **case-insensitive `fopen`**:
  on a read miss, walk the path components and match case-insensitively
  (`opendir` + `strcasecmp`). Lets mixed-case game data (`sound/` vs `SOUND/`) work
  on a case-sensitive FS with **no manual renaming**. Directly reusable on any
  case-sensitive platform. Same patch maps `compat_timeGetTime()` to `SDL_GetTicks()`
  off-Windows (movie/pacing timing).
- 🟢 **`patches/0010-src_file_find_cc.patch`** — `fileFindFirst` did `opendir()` on a
  **relative** base, which fails when `getcwd()` isn't the data dir → the save/load
  map scan found 0 entries. Fix prepends the data path before `opendir` (names stay
  relative).
- 🟢 **`patches/0017-src_settings_cc.patch`** — clear `master_patches`/`critter_patches`
  so the "data" dir-xbase that bridges the SAVEGAME path on desktop isn't required.

Together 0010 + 0017 are what make **save/load** work when the process CWD isn't the
game directory — a common console situation.

## Boot / platform bring-up (🟠 all OpenOrbis-specific)

- **`overlay/src/platform/ps4/ps4.cc` / `.h`** — the platform core: `libjbc`
  jailbreak + mount `/data` into the sandbox, read/write `ps4_controls.cfg`, load the
  GLES modules, and the clean-exit wrap. If you port to another OS, this file is what
  you rewrite; everything above it stays.
- **`patches/0018-src_win32_cc.patch`** — **boot ordering** (this fixed the
  black screen): `SDL_Init` → `ps4MountData()` → configure Piglet/EGL → create the
  window. The cred jailbreak must run **before** the EGL surface is created or the
  compositor has no permission to register the canvas. Also wraps `exit()`/`_exit()`
  (`-Wl,--wrap`) to `sceSystemServiceLoadExec("exit")` so quitting returns to the
  dashboard instead of `SIGSYS`.
- **`patches/0003-src_audio_engine_cc.patch`** — leave AUDIO out of the initial
  `SDL_Init`, and skip `SDL_CloseAudioDevice` on shutdown (the SDL-PS4 audio backend
  `abort()`s there). A clean-exit fix.
- **Console-provided GLES modules** — the default build bundles **no Sony modules**
  (publish-safe); the two `.sprx` are loaded from a shared console path at launch
  (same convention as the SM64 OpenOrbis port). See `os/ps4/packaging.cmake` +
  `PS4_MODULES_ON_CONSOLE` and the README.

## Portability gotchas worth stealing

- 🟢 **`patches/0009-src_display_monitor_cc.patch`** — a file-scope object was
  lazily constructed to dodge the **static-initialization-order fiasco** (an early
  ctor touched a not-yet-initialized `iostream`). Bites any toolchain with different
  init order — not just PS4.
- 🟠 **`patches/0006-src_debug_cc.patch`** — route `debugPrint` to **klog**
  unconditionally (the OpenOrbis console log), so on-device diagnostics actually show.
- 🟠 **`patches/0014-src_movie_cc.patch` / `0015-src_movie_lib_cc.patch`** — movie
  playback instrumentation/pacing tweaks (the actual timing fix is the
  `SDL_GetTicks()` mapping in 0016).

## Build system

- **`patches/0002-CMakeLists_txt.patch`** — the only upstream build hook:
  `if(PS4) include(os/ps4/ps4.cmake)`. Everything PS4 (sources, `--wrap`s, `link.x`,
  packaging) lives under **`overlay/os/ps4/`**, so upstream's `CMakeLists.txt` stays
  almost untouched.

---

For how the tree is reconstructed and built (clone upstream@ref → overlay → patches
→ pkg), see the README's **Building** section and `scripts/apply.sh`.
