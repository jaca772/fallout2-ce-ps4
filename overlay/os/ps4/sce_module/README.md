# PS4 runtime modules (`sce_module/`)

Modules bundled into the fpkg and made available to the engine at load time.
`packaging.cmake` copies everything here into the pkg's `sce_module/`.

## Shipped (OpenOrbis-provided, required)

| File | Purpose |
|---|---|
| `libc.prx` | OpenOrbis C runtime module |
| `libSceFios2.prx` | file I/O module |

Both are free OpenOrbis runtime modules. **Both are required** — the eboot lists them
as NEEDED module dependencies, and the loader kills the process with
`PRX_SCE_MODULE_LOAD_ERROR` (0xa0020102) before `main()` if they are absent.

## Zero proprietary Sony modules needed

The PS4 port presents its video frame directly via native `sceVideoOut` flip queues.
Unlike default SDL2 GLES ports, it does **not** use Piglet/GLES and requires **no**
proprietary Sony `.sprx` modules (`libScePigletv2VSH.sprx` / `libSceShaccVSH.sprx`).
The generated `.pkg` is publish-safe and requires no files to be copied to the console.
