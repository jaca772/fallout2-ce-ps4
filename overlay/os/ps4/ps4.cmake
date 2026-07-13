# PS4 / OpenOrbis dependency wiring.
#
# Included from the top-level CMakeLists when building with the OpenOrbis
# toolchain (os/ps4/toolchain is a thin wrapper over ${OPENORBIS}/cmake/ps4.cmake,
# which sets PS4=TRUE). Keeps all PS4 knowledge out of the shared build graph.

if(NOT DEFINED ENV{OPENORBIS})
    message(FATAL_ERROR
        "OPENORBIS env var not set. Run 'source \$OPENORBIS/ps4vars.sh' before configuring.")
endif()
set(OPENORBIS "$ENV{OPENORBIS}")

# --- SDL2 (portlibs) --------------------------------------------------------
# sdl2-config emits the full link line including the Sce backend modules the
# port needs (SceVideoOut, SceAudioOut, ScePad, Pigletv2VSH, ...).
find_program(SDL2_CONFIG
    NAMES sdl2-config
    HINTS "${OPENORBIS}/usr/bin"
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT SDL2_CONFIG)
    message(FATAL_ERROR "sdl2-config not found in ${OPENORBIS}/usr/bin — install ps4-openorbis-portlibs")
endif()

execute_process(COMMAND "${SDL2_CONFIG}" --cflags
    OUTPUT_VARIABLE _sdl2_cflags OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${SDL2_CONFIG}" --libs
    OUTPUT_VARIABLE _sdl2_libs OUTPUT_STRIP_TRAILING_WHITESPACE)

# sdl2-config --cflags is "-I<dir> -D...": extract the include dirs.
string(REGEX MATCHALL "-I[^ ]+" _sdl2_incs "${_sdl2_cflags}")
foreach(_i ${_sdl2_incs})
    string(SUBSTRING "${_i}" 2 -1 _i)
    list(APPEND SDL2_INCLUDE_DIRS "${_i}")
endforeach()
list(APPEND SDL2_INCLUDE_DIRS "${OPENORBIS}/usr/include/SDL2")

# --libs is the raw link line; hand it to the linker verbatim.
separate_arguments(SDL2_LIBRARIES UNIX_COMMAND "${_sdl2_libs}")
set(SDL2_MAIN_LIBRARIES "")

# --- zlib (portlibs) --------------------------------------------------------
set(ZLIB_INCLUDE_DIRS "${OPENORBIS}/usr/include")
set(ZLIB_LIBRARIES "-lz")

# --- libjbc + Sce system modules -------------------------------------------
# libjbc.a is installed into ${OPENORBIS}/usr/lib by os/ps4/build_libjbc.sh.
set(PS4_SYSTEM_LIBRARIES
    -ljbc
    -lSceLibcInternal
    -lkernel
    -lSceComposite
    -lSceSystemService
    -lSceUserService
    -lScePad
    -lSceSysmodule
)

# NOTE: this file is pulled in with include(), which shares the caller's
# directory scope, so plain set() above already exposes these variables to the
# rest of CMakeLists — no PARENT_SCOPE needed.

# --- Proprietary GLES modules: console-provided (default) vs. bundled -------
# ON (default): libScePigletv2VSH/libSceShaccVSH are NOT put in the pkg — the user
#   copies them to a shared console path once (SM64-port style). The DISTRIBUTED
#   pkg is free of Sony modules (publish-safe) AND the build needs no proprietary
#   files at all. Device-verified (no black screen). See os/ps4/sce_module/README.md.
# OFF: bundle them in the pkg (self-contained, install-and-run; contains Sony
#   modules — do not redistribute).
option(PS4_MODULES_ON_CONSOLE "Load Piglet/Shacc from the console, not the pkg (publish-safe)" ON)
if(PS4_MODULES_ON_CONSOLE)
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE PS4_MODULES_ON_CONSOLE)
endif()

# --- Optional native sceVideoOut render path (EXPERIMENTAL, drops Sony modules) ----
# OFF (default): unchanged SDL2/Piglet renderer — the working, shipping build.
# ON: present the software frame via a native sceVideoOut flip queue
#   (src/platform/ps4/ps4_video.cc) and DON'T init SDL video, so Piglet never loads.
#   SDL still statically references its GLES2/EGL/piglet symbols, so we link WITHOUT
#   -lScePigletv2VSH and satisfy those symbols with empty stubs (ps4_piglet_stubs.c);
#   none is ever called. The pkg then needs NO Sony modules and NO console-side file
#   copying. See docs/ps4-native-videoout-plan.md. Everything is behind
#   PS4_NATIVE_VIDEOOUT so the default build is byte-for-byte unchanged.
option(PS4_NATIVE_VIDEOOUT "Render via native sceVideoOut instead of SDL/Piglet (no Sony modules)" OFF)
if(PS4_NATIVE_VIDEOOUT)
    message(STATUS "PS4: native sceVideoOut path ENABLED (no Piglet/Shacc)")
    target_compile_definitions(${EXECUTABLE_NAME} PRIVATE PS4_NATIVE_VIDEOOUT)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ps4/ps4_video.h"
        "src/platform/ps4/ps4_video.cc"
        "src/platform/ps4/ps4_piglet_stubs.c"
    )
    # Drop the Sony piglet import lib (stubs replace it); add SceVideoOut, which
    # ps4_video.cc calls directly (the default build reaches it only via Piglet).
    list(REMOVE_ITEM SDL2_LIBRARIES "-lScePigletv2VSH")
    list(APPEND PS4_SYSTEM_LIBRARIES -lSceVideoOut)
endif()

# --- PS4 sources, symbol wraps, patched link script -------------------------
# Target properties / linker flags below are order-independent, so they live
# here (in the overlay) instead of inline in the top-level CMakeLists — that
# keeps the CMakeLists PS4 hook down to a bare include().
target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/platform/ps4/ps4.h"
    "src/platform/ps4/ps4.cc"
    "src/platform/ps4/ps4_gamepad.h"
    "src/platform/ps4/ps4_gamepad.cc"
)
target_include_directories(${EXECUTABLE_NAME} PRIVATE "src")

# Redirect the C++ runtime's global-destructor registration to the no-op
# __wrap___cxa_atexit in ps4.cc — libSceLibcInternal's atexit machinery
# faults during static init on this target. ld.lld is invoked directly here,
# so pass the raw linker flag.
target_link_options(${EXECUTABLE_NAME} PRIVATE "LINKER:--wrap=__cxa_atexit")

# Redirect exit()/_exit() to __wrap_exit/__wrap__exit in ps4.cc. When main()
# returns (e.g. "Exit Game"), the CRT calls exit() -> ... -> libkernel _exit,
# whose raw process-exit syscall is SIGSYS-blocked for this MiniApp (observed
# on device). The wrappers instead ask LNC to close the app and return to the
# PS4 dashboard via sceSystemServiceLoadExec("exit", ...).
target_link_options(${EXECUTABLE_NAME} PRIVATE "LINKER:--wrap=exit")
target_link_options(${EXECUTABLE_NAME} PRIVATE "LINKER:--wrap=_exit")

# Redirect the whole malloc family to our own mspace (ps4.cc). oo-libc's
# malloc reserves 2.5 GiB and maps it with the System flexible-memory variant,
# which fails on this target and then null-derefs; a right-sized NON-system
# mapping works. Every allocator symbol references below becomes __wrap_*.
foreach(_ps4_alloc malloc free calloc realloc)
    target_link_options(${EXECUTABLE_NAME} PRIVATE "LINKER:--wrap=${_ps4_alloc}")
endforeach()

# Swap the toolchain's link.x for our patched copy. The stock script's
# `.init_array : { *(.init_array); }` matches only priority-less constructors
# and drops every `.init_array.<N>` (libc++ stream init AND our early
# malloc_init primer in ps4.cc). Our copy gathers the sorted priority sections
# so the crt actually runs them. See os/ps4/link.x. The --script flag lives in
# CMAKE_EXE_LINKER_FLAGS (set by the toolchain file); rewrite it in place.
set(_ps4_stock_linkx "--script $ENV{OPENORBIS}/link.x")
set(_ps4_our_linkx "--script ${CMAKE_SOURCE_DIR}/os/ps4/link.x")
if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "os/ps4/link\\.x")
    string(REPLACE "${_ps4_stock_linkx}" "${_ps4_our_linkx}"
        CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
endif()
if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "os/ps4/link\\.x")
    message(FATAL_ERROR
        "PS4: could not substitute link.x into CMAKE_EXE_LINKER_FLAGS "
        "(value: ${CMAKE_EXE_LINKER_FLAGS}). The priority .init_array fix "
        "would be silently lost — aborting.")
endif()
