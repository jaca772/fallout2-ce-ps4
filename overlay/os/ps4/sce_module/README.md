# PS4 runtime modules (`sce_module/`)

Modules bundled into the fpkg and made available to the engine at load time.
`packaging.cmake` copies everything here into the pkg's `sce_module/`, and
`ps4PlatformInit()` points SDL2 at it via `SDL_PS4_PIGLET_MODULES_PATH`.

## Shipped (OpenOrbis-provided, required)

| File | Purpose |
|---|---|
| `libc.prx` | OpenOrbis C runtime module |
| `libSceFios2.prx` | file I/O module |

Copied from `$OPENORBIS/samples/piglet/sce_module/`. **Both are required** — the
eboot lists them as NEEDED module dependencies, and the loader kills the process
with `PRX_SCE_MODULE_LOAD_ERROR` (0xa0020102) before `main()` if they are absent.

## You must supply (proprietary — NOT committed, git-ignored)

SDL2's OpenOrbis backend renders via GLES using the **Piglet** module. The
engine links against SDK stubs for it, so a **matching** runtime module must be
present or you get, at launch:

```
exception: 0xa0020101 (PRX_NOT_RESOLVED_FUNCTION)
Required Module Name : libScePigletv2VSH
Required Function NID : 0x33D46D5E98D262D1
```

Drop these two files here, then rebuild (`os/ps4/build.sh`):

| File | What |
|---|---|
| `libScePigletv2VSH.sprx` | GLES driver (Piglet) |
| `libSceShaccVSH.sprx` | runtime shader compiler Piglet needs |

### Where to get them

The catch: the module must **export NID `0x33D46D5E98D262D1`**, i.e. its ABI has
to match the OpenOrbis portlibs SDL2 was built against. Two reliable sources:

1. **From an existing working OpenOrbis SDL2 homebrew** that already runs on your
   console — copy its `sce_module/libScePigletv2VSH.sprx` + `libSceShaccVSH.sprx`.
   These are proven-compatible with OpenOrbis and are the safest choice.
2. **Dumped from console firmware** at `/system/common/lib/` (FTP as root). Use a
   firmware whose Piglet exports the required NID — if your own console's copy
   still crashes with the same NID error, it's the wrong version; use option 1.

These are Sony system modules, so they are **not redistributed in this repo**
(hence `.gitignore`). This is separate from the game-asset rule in CLAUDE.md, but
the same principle: proprietary binaries stay off the repo.

## Bundled (default) vs console-provided (`-DPS4_MODULES_ON_CONSOLE`)

- **Default build:** drop the two `.sprx` here; they are packaged into the pkg
  (`/app0/sce_module`). The pkg is self-contained (install & run) but **contains
  the Sony modules** — do not redistribute it publicly.
- **`-DPS4_MODULES_ON_CONSOLE=ON`:** the two `.sprx` are **not** put in the pkg.
  Instead copy them **once onto the console** (SM64-port style) at:
  ```
  /data/self/system/common/lib/libScePigletv2VSH.sprx
  /data/self/system/common/lib/libSceShaccVSH.sprx
  ```
  Then the distributed pkg carries **no Sony modules** and is safe to publish.
  (Loaded before the /data mount, so this path must be readable that early — a
  black screen at launch means it isn't; verify on device.)
