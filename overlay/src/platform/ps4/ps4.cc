#include "platform/ps4/ps4.h"

#ifdef __PS4__

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include <orbis/libkernel.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/SystemService.h>

#include <jbc/libjbc.h>

#include "platform_compat.h"

// --- C++ static-init workaround (PS4/OpenOrbis) -----------------------------
// The engine has many file-scope C++ objects whose constructors run before
// main(). On this target their global-destructor registration goes through
// __cxa_atexit -> libSceLibcInternal, which faults during static init because
// its atexit machinery isn't initialized yet (SIGSEGV, fault addr 0x38, inside
// libSceLibcInternal). A homebrew process never exits cleanly — it runs until
// the system kills it — so global destructors never need to run.
//
// We can't just redefine __cxa_atexit (libc.a already provides one -> duplicate
// symbol), so the PS4 link uses -Wl,--wrap=__cxa_atexit (see CMakeLists) to
// redirect every call to this no-op. Registration simply does nothing.
extern "C" int __wrap___cxa_atexit(void (*)(void*), void*, void*)
{
    return 0;
}

// --- Clean process exit (PS4/OpenOrbis) -------------------------------------
// When main() returns (e.g. the user picks "Exit Game") the CRT calls exit(),
// which runs atexit handlers and then libkernel's _exit — whose raw process-exit
// syscall is SIGSYS-blocked for this MiniApp (observed on device: signal 12
// right after "falloutMain returned 0"). Redirect exit()/_exit() (via
// -Wl,--wrap in CMakeLists) to ask LNC to close the app and return to the PS4
// dashboard. sceSystemServiceLoadExec("exit", ...) is the OpenOrbis idiom for
// this and never issues the forbidden syscall. Never returns.
static void ps4EarlyLog(const char* fmt, ...);

extern "C" [[noreturn]] void __wrap_exit(int status)
{
    ps4EarlyLog("[ps4] exit(%d) -> sceSystemServiceLoadExec(exit)\n", status);
    const char* argv[] = { "exit", nullptr };
    int rc = sceSystemServiceLoadExec("exit", argv);
    ps4EarlyLog("[ps4] LoadExec(exit) -> 0x%08x (hanging)\n", rc);
    for (;;) {
        sceKernelUsleep(100000);
    }
}

extern "C" [[noreturn]] void __wrap__exit(int status)
{
    __wrap_exit(status);
}

// --- Early heap initialization (PS4/OpenOrbis) ------------------------------
// Root cause of the "operator new -> libSceLibcInternal, fault addr 0x38 during
// static init" crash: OpenOrbis' oo-libc malloc creates its allocator (an
// "mspace" over 2.5 GiB of reserved flexible memory) LAZILY on the first malloc.
// If any of that lazy setup fails, malloc's error path falls through to
// sceLibcMspaceMalloc(NULL, size), which dereferences a null mspace at offset
// 0x38 inside libSceLibcInternal. The engine has file-scope C++ objects whose
// constructors allocate before main(), so the very first one trips this.
//
// oo-libc ships malloc_init() for exactly this: it runs the same reserve/map/
// mspace-create sequence but RETURNS 0/1 instead of crashing, and is idempotent
// (no-op once the mspace exists). Nothing in the crt's startup path calls it, so
// we call it ourselves as the earliest possible init_array entry — before any
// allocating global constructor. Once the mspace exists, every later malloc/new
// takes malloc's fast path.
//
// Ordering: the pointer below is placed in .init_array.00001 so it sorts ahead
// of every other constructor (libc++'s iostream init is .init_array.00101, the
// engine's are priority-less). This only works because os/ps4/link.x is patched
// to gather SORT_BY_INIT_PRIORITY(.init_array.*); the stock toolchain script
// dropped these sections entirely. Keep the two changes together.
// Allocation-free klog write that works before SDL_Init and before the heap is
// up. SDL_Log's sink is only installed during SDL_Init; sceKernelDebugOutText is
// the primitive the debug channel forwards. Format into a stack buffer.
static void ps4EarlyLog(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sceKernelDebugOutText(0, buf);
}

// --- Custom malloc backed by our own mspace ---------------------------------
// Device diagnosis (fault-address encoding, 2026-07-07): a small NON-system
// flexible mapping (sceKernelReserveVirtualRange + sceKernelMapNamedFlexibleMemory)
// SUCCEEDS, but oo-libc's own malloc — which reserves 2.5 GiB and maps it with
// the *System* flexible-memory variant — FAILS (malloc_init() -> 1), and its
// error path then dereferences a null mspace (the original fault 0x38). Rather
// than fight oo-libc's hardcoded 2.5 GiB System mapping, we build our own mspace
// over a right-sized NON-system flexible mapping and redirect the whole malloc
// family to it via -Wl,--wrap (see CMakeLists). Every allocation in the process
// — engine, libc++, and libc-internal — flows through here.
//
// libSceLibcInternal's headers declare these with no real prototypes, so we
// declare the ABI ourselves (taken from disassembling oo-libc's malloc). They
// resolve against -lSceLibcInternal, which the PS4 link already pulls in.
extern "C" {
void* sceLibcMspaceCreate(const char* name, void* base, size_t capacity, unsigned int flag);
void* sceLibcMspaceMalloc(void* mspace, size_t size);
void sceLibcMspaceFree(void* mspace, void* ptr);
void* sceLibcMspaceCalloc(void* mspace, size_t nelem, size_t size);
void* sceLibcMspaceRealloc(void* mspace, void* ptr, size_t size);
}

static void* g_ps4Mspace = nullptr;

// Reserve + flexible-map + create an mspace. Tries decreasing sizes so we grab
// the largest region the process's flexible-memory budget allows (Fallout 2
// itself needs only tens of MiB, so even the smallest is plenty).
static void* ps4CreateHeap()
{
    static const size_t kSizes[] = {
        1024ull * 1024 * 1024, 512ull * 1024 * 1024, 256ull * 1024 * 1024,
        128ull * 1024 * 1024, 64ull * 1024 * 1024,
    };
    for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i) {
        size_t len = kSizes[i];
        void* base = nullptr;
        if (sceKernelReserveVirtualRange(&base, len, 0, 16 * 1024) != 0) {
            continue;
        }
        if (sceKernelMapNamedFlexibleMemory(&base, len, 0x3 /*RW*/, 0, "fallout2-heap") != 0) {
            continue; // leaked VA reservation is fine (47-bit space); try smaller
        }
        void* ms = sceLibcMspaceCreate("fallout2", base, len, 0);
        if (ms != nullptr) {
            ps4EarlyLog("[ps4] heap: mspace=%p size=%zuMiB\n", ms, len >> 20);
            return ms;
        }
    }
    return nullptr;
}

// Lazily construct the mspace on first use. ps4HeapInit() forces this before any
// other constructor runs, so the pointer is set single-threaded before main().
static inline void* ps4Mspace()
{
    if (g_ps4Mspace == nullptr) {
        g_ps4Mspace = ps4CreateHeap();
    }
    return g_ps4Mspace;
}

extern "C" void* __wrap_malloc(size_t size)
{
    return sceLibcMspaceMalloc(ps4Mspace(), size);
}

extern "C" void __wrap_free(void* ptr)
{
    if (ptr != nullptr) {
        sceLibcMspaceFree(g_ps4Mspace, ptr);
    }
}

extern "C" void* __wrap_calloc(size_t nelem, size_t size)
{
    return sceLibcMspaceCalloc(ps4Mspace(), nelem, size);
}

extern "C" void* __wrap_realloc(void* ptr, size_t size)
{
    if (ptr == nullptr) {
        return sceLibcMspaceMalloc(ps4Mspace(), size);
    }
    if (size == 0) {
        sceLibcMspaceFree(g_ps4Mspace, ptr);
        return nullptr;
    }
    return sceLibcMspaceRealloc(ps4Mspace(), ptr, size);
}

// NB: aligned_alloc / posix_memalign / memalign are defined in the toolchain's
// libc.a on top of malloc/free (which are undefined imports there), so wrapping
// malloc is enough to route them through our mspace — no separate wrappers.

extern "C" void ps4HeapInit(void)
{
    ps4EarlyLog("[ps4] heap ctor: enter\n");
    void* ms = ps4Mspace();
    if (ms == nullptr) {
        // Our own heap couldn't be built either — flag it distinctly in the
        // kernel crash dump (fault address 0x71..) so it's obvious in klog.
        ps4EarlyLog("[ps4] heap: CREATE FAILED\n");
        volatile char sink = *(volatile char*)(uintptr_t)0x0000710000000000ULL;
        (void)sink;
    }
}

__attribute__((used, section(".init_array.00001"))) static void (*const ps4HeapInitEntry)(void) = &ps4HeapInit;

namespace fallout {

// Public klog helper (declared in ps4.h) for the shared startup code in
// win32.cc. Same allocation-free debug-channel write as the file-scope
// ps4EarlyLog used above.
void ps4Log(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sceKernelDebugOutText(0, buf);
}

// Directory (inside the app package) that holds bundled modules (libc.prx,
// libSceFios2.prx). Must match where packaging.cmake stages sce_module.
#define PS4_SCE_MODULE_PATH "/app0/sce_module"

// Where the proprietary GLES modules (libScePigletv2VSH.sprx + libSceShaccVSH.sprx)
// live. Default: bundled in the pkg (self-contained, install-and-run). With
// -DPS4_MODULES_ON_CONSOLE they instead come from a shared console location the
// user copies them to once (SM64-port style) — this keeps the DISTRIBUTED pkg free
// of Sony modules (publish-safe). See os/ps4/sce_module/README.md.
#ifdef PS4_MODULES_ON_CONSOLE
#define PS4_GLES_MODULE_PATH "/data/self/system/common/lib"
#else
#define PS4_GLES_MODULE_PATH PS4_SCE_MODULE_PATH
#endif

// Explicitly load a GLES module from our pkg before SDL's own PS4_PigletInit
// runs. This is load-bearing, not just diagnostic: SDL's internal
// sceKernelLoadStartModule for piglet fails silently on this port, so its later
// patch step can't find the module by name (GetModuleInfoByName ENOENT ->
// "unable to patch" -> SIGSYS). Loading here registers the module under its
// basename first, so SDL's load returns the existing handle and the patch step
// succeeds ("patching module done" / "PS4_PigletInit: Ok").
//
// The logged return code helps future debugging: a small value is a module id
// (success); 0x8xxxxxxx is an error (0x80020002 ENOENT = file missing; a
// decrypt/auth error would mean the .sprx isn't a loadable fake-self).
static void ps4LoadGlesModule(const char* path)
{
    uint32_t r = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
    ps4EarlyLog("[ps4] sceKernelLoadStartModule(%s) -> 0x%08x\n", path, r);
}

void ps4PreSdlInit()
{
#ifdef PS4_NATIVE_VIDEOOUT
    // Native sceVideoOut build: we never init SDL video, so Piglet/Shacc are neither
    // loaded nor needed. Skipping this is what makes the pkg free of Sony modules.
    ps4Log("[ps4] native sceVideoOut: skipping Piglet/Shacc module load\n");
    return;
#else
    // Point the SDL2 OpenOrbis backend at the piglet/shacc modules bundled in
    // our pkg. Without this SDL loads the system VSH copy from
    // /<sandbox>/common/lib, whose ABI doesn't match the SDK stubs the engine
    // linked against -> PRX_NOT_RESOLVED_FUNCTION / failed piglet patch.
    //
    // This MUST go through SDL's hint system, not libc setenv: SDL's PS4 port
    // reads the path via SDL_GetHint/SDL_getenv, which uses SDL's own env table,
    // not libc's environ. A libc setenv() here is silently ignored (observed:
    // piglet still loaded from the default /<sandbox>/common/lib path).
    // OVERRIDE priority beats any pre-existing value. Safe before SDL_Init.
    SDL_SetHintWithPriority("SDL_PS4_PIGLET_MODULES_PATH", PS4_GLES_MODULE_PATH, SDL_HINT_OVERRIDE);
    SDL_setenv("SDL_PS4_PIGLET_MODULES_PATH", PS4_GLES_MODULE_PATH, 1);

    // Pre-load the GLES modules so their load result is visible in klog and so
    // they are registered before SDL's own PS4_PigletInit runs. Shacc first
    // (piglet's shader compiler dependency), then piglet. With PS4_MODULES_ON_CONSOLE
    // these come from the shared console path (must be readable this early, before
    // ps4MountData) — if the load logs an error the user hasn't copied them there.
    ps4LoadGlesModule(PS4_GLES_MODULE_PATH "/libSceShaccVSH.sprx");
    ps4LoadGlesModule(PS4_GLES_MODULE_PATH "/libScePigletv2VSH.sprx");
#endif
}

bool ps4ConfigurePigletForEgl(int width, int height)
{
    OrbisPglConfig config;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    config.processOrder = 1;
    config.systemSharedMemorySize = 0x1000000;       // 16 MB
    config.videoSharedMemorySize = 0x3000000;        // 48 MB
    config.maxMappedFlexibleMemory = 0xFFFFFFFF;     // 4 GB
    config.drawCommandBufferSize = 0x100000;        // 1 MB
    config.lcueResourceBufferSize = 0x1000000;       // 16 MB
    config.dbgPosCmd_0x40 = 1920;                    // Physical display width
    config.dbgPosCmd_0x44 = 1080;                    // Physical display height
    config.dbgPosCmd_0x48 = 0;
    config.dbgPosCmd_0x4C = 0;
    config.unk_0x5C = 2;

    bool ok = scePigletSetConfigurationVSH(&config);
    ps4Log("[ps4] scePigletSetConfigurationVSH(sys=16 vid=48 flex=max MiB, %dx%d) -> %d\n", width, height, ok ? 1 : 0);
    return ok;
}

// Reads the engine's target resolution from fallout2.cfg's [screen] section.
// The PS4 bootstrap creates the SDL window BEFORE the engine loads its config;
// _GNW95_init_window's "already created" guard then makes the engine keep that
// surface. If the bootstrap hardcodes 640x480 but the user configured widescreen
// (e.g. 1280x720), the engine lays out the UI at the configured size yet renders
// into a 640x480 surface -> clipped loading screen, 4:3 menu/game. Reading the
// config here lets the bootstrap create the window at the right size. Must run
// after ps4MountData (cwd = /data/fallout2). Defaults 640x480 / scale 1.
void ps4ReadDisplayConfig(int* outWidth, int* outHeight, int* outScale)
{
    int resX = 640, resY = 480, scale = 1;
    FILE* f = fopen(PS4_DATA_PATH "/fallout2.cfg", "r");
    if (f != nullptr) {
        char line[256];
        bool inScreen = false;
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '[') {
                inScreen = (strncmp(p, "[screen]", 8) == 0);
                continue;
            }
            if (!inScreen) {
                continue;
            }
            int v;
            if (sscanf(p, "resolution_x=%d", &v) == 1) resX = v;
            else if (sscanf(p, "resolution_y=%d", &v) == 1) resY = v;
            else if (sscanf(p, "scale=%d", &v) == 1) scale = v;
        }
        fclose(f);
    } else {
        ps4Log("[ps4] display config: fallout2.cfg not readable, using 640x480\n");
    }
    // Match the clamps settings.cc applies (resolution_x 640..7680,
    // resolution_y 480..4320, scale 1..4) so the bootstrap size can never diverge
    // from what the engine computes.
    if (resX < 640) resX = 640;
    if (resX > 7680) resX = 7680;
    if (resY < 480) resY = 480;
    if (resY > 4320) resY = 4320;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    *outWidth = resX;
    *outHeight = resY;
    *outScale = scale;
    ps4Log("[ps4] display config: %dx%d scale=%d\n", resX, resY, scale);
}

// User-tunable gamepad settings with sensible defaults. Overridden by
// /data/fallout2/ps4_controls.cfg (see ps4ReadControlsConfig).
Ps4ControlsConfig g_ps4Controls = {
    /* touchSensitivity */ 0.25f,
    /* touchAccel       */ 0.001f,
    /* touchSmoothing   */ 0.40f,
    /* touchDeadzone    */ 2.0f,
    /* cursorSpeed      */ 450.0f,
    /* cursorSpeedBoost */ 900.0f,
    /* walkDeadzone     */ 0.30f,
    /* runThreshold     */ 0.50f,
    /* cameraDeadzoneX  */ 100,
    /* cameraDeadzoneY  */ 75,
    /* worldRightStick  */ PS4_WRS_CURSOR,
    /* walkProjectTiles   */ 2,
    /* runSettleMs        */ 50,
    /* touchMaxGain       */ 4.0f,
    /* touchTapMs         */ 200,
    /* touchTapMove       */ 40,
    /* dpadRepeatMs       */ 150,
    /* cameraPanThreshold */ 0.40f,
    /* actCross    */ PS4_ACT_LMB,
    /* actCircle   */ PS4_ACT_RMB,
    /* actSquare   */ PS4_ACT_SKILLDEX,
    /* actTriangle */ PS4_ACT_CHARSHEET,
    /* actL1       */ PS4_ACT_INVENTORY,
    /* actR1       */ PS4_ACT_RELOAD,
    /* actL3       */ PS4_ACT_SWAPHANDS,
    /* actR3       */ PS4_ACT_COMBAT,
    /* actShare    */ PS4_ACT_PIPBOY,
    /* actOptions  */ PS4_ACT_ESC,
    /* worldLeftStick */ PS4_WRS_CENTERED,
    /* combatAutoAim   */ 1,
    /* worldAutoAim    */ 1,
    /* autoAimWholeMap */ 0,
    /* autoAimCenterCamera */ 0,
    /* meleeApproach */ PS4_MELEE_APPROACH,
    /* lootAssist */ 1,
    /* lootSnapDistance */ 20,
    /* lootSkipEmpty */ 1,
    /* circleCloseAll */ 0,
};

// Map an action name to a PS4_ACT_* value (-1 = unknown, keep current).
static int ps4ParseAction(const char* s)
{
    if (strncmp(s, "lmb", 3) == 0) return PS4_ACT_LMB;
    if (strncmp(s, "rmb", 3) == 0) return PS4_ACT_RMB;
    if (strncmp(s, "boost", 5) == 0) return PS4_ACT_CURSORBOOST;
    if (strncmp(s, "esc", 3) == 0) return PS4_ACT_ESC;
    if (strncmp(s, "inventory", 9) == 0) return PS4_ACT_INVENTORY;
    if (strncmp(s, "charsheet", 9) == 0) return PS4_ACT_CHARSHEET;
    if (strncmp(s, "skilldex", 8) == 0) return PS4_ACT_SKILLDEX;
    if (strncmp(s, "pipboy", 6) == 0) return PS4_ACT_PIPBOY;
    if (strncmp(s, "endcombat", 9) == 0) return PS4_ACT_ENDCOMBAT;
    if (strncmp(s, "combat", 6) == 0) return PS4_ACT_COMBAT;
    if (strncmp(s, "reload", 6) == 0) return PS4_ACT_RELOAD;
    if (strncmp(s, "weaponmode", 10) == 0) return PS4_ACT_WEAPONMODE;
    if (strncmp(s, "swaphands", 9) == 0) return PS4_ACT_SWAPHANDS;
    if (strncmp(s, "none", 4) == 0) return PS4_ACT_NONE;
    return -1;
}

static const char* ps4ActionName(int a)
{
    switch (a) {
    case PS4_ACT_LMB: return "lmb";
    case PS4_ACT_RMB: return "rmb";
    case PS4_ACT_CURSORBOOST: return "boost";
    case PS4_ACT_ESC: return "esc";
    case PS4_ACT_INVENTORY: return "inventory";
    case PS4_ACT_CHARSHEET: return "charsheet";
    case PS4_ACT_SKILLDEX: return "skilldex";
    case PS4_ACT_PIPBOY: return "pipboy";
    case PS4_ACT_COMBAT: return "combat";
    case PS4_ACT_ENDCOMBAT: return "endcombat";
    case PS4_ACT_RELOAD: return "reload";
    case PS4_ACT_WEAPONMODE: return "weaponmode";
    case PS4_ACT_SWAPHANDS: return "swaphands";
    default: return "none";
    }
}

// World stick role name / parse (PS4_WRS_*), shared by both sticks.
static const char* ps4WrsName(int r)
{
    switch (r) {
    case PS4_WRS_CURSOR: return "cursor";
    case PS4_WRS_CENTERED: return "centered";
    case PS4_WRS_WALK: return "walk";
    case PS4_WRS_OFF: return "off";
    default: return "camera";
    }
}

static int ps4ParseWrs(const char* s)
{
    if (strncmp(s, "cursor", 6) == 0) return PS4_WRS_CURSOR;
    if (strncmp(s, "centered", 8) == 0) return PS4_WRS_CENTERED;
    if (strncmp(s, "camera", 6) == 0) return PS4_WRS_CAMERA;
    if (strncmp(s, "walk", 4) == 0) return PS4_WRS_WALK;
    if (strncmp(s, "off", 3) == 0) return PS4_WRS_OFF;
    return -1;
}

// Parse a "key=action" line; on match, store the action (if known) and return true.
static bool ps4ParseButtonLine(const char* line, const char* key, int* out)
{
    char fmt[32];
    char buf[16];
    snprintf(fmt, sizeof(fmt), "%s=%%15s", key);
    if (sscanf(line, fmt, buf) == 1) {
        int a = ps4ParseAction(buf);
        if (a >= 0) {
            *out = a;
        }
        return true;
    }
    return false;
}

static void ps4WriteDefaultControlsConfig(const char* path)
{
    FILE* f = fopen(path, "w");
    if (f == nullptr) {
        return;
    }
    fprintf(f,
        "# Fallout 2 PS4 gamepad tuning. Edit over FTP and relaunch (no rebuild).\n"
        "# Lower touch_sensitivity if the touchpad cursor moves too fast.\n"
        "[controls]\n"
        "# Touchpad cursor (acceleration curve, like a laptop trackpad):\n"
        "#  touch_sensitivity = gain when moving SLOWLY (lower = more precise)\n"
        "#  touch_accel       = extra speed when moving FAST (0 = linear)\n"
        "#  touch_smoothing   = 0..0.9, higher = smoother but a touch laggier\n"
        "#  touch_deadzone    = raise if the cursor drifts with a finger at rest\n"
        "touch_sensitivity=%.2f\n"
        "touch_accel=%.3f\n"
        "touch_smoothing=%.2f\n"
        "touch_deadzone=%.1f\n"
        "cursor_speed=%.0f\n"
        "cursor_speed_boost=%.0f\n"
        "walk_deadzone=%.2f\n"
        "run_threshold=%.2f\n"
        "camera_deadzone_x=%d\n"
        "camera_deadzone_y=%d\n"
        "# Stick roles in the world (each stick independently):\n"
        "#  walk     = march the character (walk/run)   [left stick only]\n"
        "#  camera   = pan/look around (cursor stays where it is)\n"
        "#  centered = pan with the cursor locked to screen centre; frees at map edge\n"
        "#  cursor   = move the cursor freely (like the touchpad)   |   off = nothing\n"
        "# Default scheme: left = centered camera, right = free cursor.\n"
        "world_left_stick=%s\n"
        "world_right_stick=%s\n"
        "\n"
        "# Aim assist (hold L2):\n"
        "#  combat_auto_aim = 1 to snap onto the nearest enemy in combat (0 = off)\n"
        "#  world_auto_aim  = 1 to snap onto the nearest person to talk to / interact\n"
        "#                    with out of combat (0 = off)\n"
        "#  auto_aim_whole_map = 1 to consider the whole map, 0 = only what's on screen\n"
        "#  auto_aim_center_camera = 1 to pan the camera so the aimed target is centred\n"
        "#  (targets always require a clear line of sight and be within reach.)\n"
        "combat_auto_aim=%d\n"
        "world_auto_aim=%d\n"
        "auto_aim_whole_map=%d\n"
        "auto_aim_center_camera=%d\n"
        "# Melee/unarmed on an out-of-range target (Cross while aiming in combat):\n"
        "#  0 = off  (the engine just says \"out of range\")\n"
        "#  1 = run up to the target but DON'T swing (default)\n"
        "#  2 = run up and then attack\n"
        "melee_approach=%d\n"
        "# Loot/interact assist: hold L2 + L1 to snap onto the nearest container,\n"
        "#  corpse or door (works in and out of combat). D-pad L/R cycles, Cross opens\n"
        "#  it (loot / open-close a door). With a Skilldex skill active (e.g. Lockpick)\n"
        "#  Cross applies that skill to the snapped target.\n"
        "#  loot_snap_distance = reach in hexes; loot_skip_empty = 1 skips empty ones.\n"
        "loot_assist=%d\n"
        "loot_snap_distance=%d\n"
        "loot_skip_empty=%d\n"
        "# Circle in the item windows (inventory / barter / loot):\n"
        "#  0 = Circle stays right-click there (cycle to the examine cursor, inspect\n"
        "#      items); close those windows with Options/Esc or their Done button.\n"
        "#  1 = Circle closes EVERY menu including those (one-button close-all).\n"
        "circle_close_all=%d\n"
        "\n"
        "# Advanced feel tuning:\n"
        "#  walk_project_tiles = hexes projected per walk step (higher = smoother, less responsive)\n"
        "#  run_settle_ms      = delay before the first walk step so a hard push starts a run\n"
        "#  touch_max_gain     = cap on touchpad acceleration\n"
        "#  touch_tap_ms / touch_tap_move = a touch shorter/smaller than these counts as a click\n"
        "#  dpad_repeat_ms     = D-pad arrow repeat speed (lower = faster)\n"
        "#  camera_pan_threshold = right-stick deflection (0..1) needed to pan the camera\n"
        "walk_project_tiles=%d\n"
        "run_settle_ms=%d\n"
        "touch_max_gain=%.1f\n"
        "touch_tap_ms=%d\n"
        "touch_tap_move=%d\n"
        "dpad_repeat_ms=%d\n"
        "camera_pan_threshold=%.2f\n"
        "\n"
        "# Button mapping. Assign one action to each button.\n"
        "# Actions: lmb rmb esc inventory charsheet skilldex pipboy boost combat\n"
        "#          endcombat reload weaponmode swaphands none\n"
        "#  lmb = click/confirm, rmb = right-click (cursor mode), boost = hold for faster cursor\n"
        "#  combat = start fight (out) / end turn — or end the fight when no enemy still\n"
        "#           wants to attack (in) ; endcombat = force-leave the fight\n"
        "#  reload = reload the active-hand weapon (spends AP in combat)\n"
        "#  weaponmode = cycle the weapon's attack mode (fists<->kick, single/aimed/burst)\n"
        "#  swaphands  = switch to the other equipped weapon\n"
        "# (D-pad is fixed to the arrow keys / target cycling in combat.)\n"
        "# NOTE: btn_share is the TOUCHPAD CLICK (the Share button is reserved by\n"
        "#  the PS4 system and cannot be read by games).\n"
        "btn_cross=%s\n"
        "btn_circle=%s\n"
        "btn_square=%s\n"
        "btn_triangle=%s\n"
        "btn_l1=%s\n"
        "btn_r1=%s\n"
        "btn_l3=%s\n"
        "btn_r3=%s\n"
        "btn_share=%s\n"
        "btn_options=%s\n",
        g_ps4Controls.touchSensitivity, g_ps4Controls.touchAccel,
        g_ps4Controls.touchSmoothing, g_ps4Controls.touchDeadzone,
        g_ps4Controls.cursorSpeed, g_ps4Controls.cursorSpeedBoost,
        g_ps4Controls.walkDeadzone, g_ps4Controls.runThreshold,
        g_ps4Controls.cameraDeadzoneX, g_ps4Controls.cameraDeadzoneY,
        ps4WrsName(g_ps4Controls.worldLeftStick),
        ps4WrsName(g_ps4Controls.worldRightStick),
        g_ps4Controls.combatAutoAim, g_ps4Controls.worldAutoAim,
        g_ps4Controls.autoAimWholeMap, g_ps4Controls.autoAimCenterCamera,
        g_ps4Controls.meleeApproach,
        g_ps4Controls.lootAssist, g_ps4Controls.lootSnapDistance, g_ps4Controls.lootSkipEmpty,
        g_ps4Controls.circleCloseAll,
        g_ps4Controls.walkProjectTiles, g_ps4Controls.runSettleMs,
        g_ps4Controls.touchMaxGain, g_ps4Controls.touchTapMs,
        g_ps4Controls.touchTapMove, g_ps4Controls.dpadRepeatMs,
        g_ps4Controls.cameraPanThreshold,
        ps4ActionName(g_ps4Controls.actCross), ps4ActionName(g_ps4Controls.actCircle),
        ps4ActionName(g_ps4Controls.actSquare), ps4ActionName(g_ps4Controls.actTriangle),
        ps4ActionName(g_ps4Controls.actL1), ps4ActionName(g_ps4Controls.actR1),
        ps4ActionName(g_ps4Controls.actL3), ps4ActionName(g_ps4Controls.actR3),
        ps4ActionName(g_ps4Controls.actShare), ps4ActionName(g_ps4Controls.actOptions));
    fclose(f);
    ps4Log("[ps4] wrote default ps4_controls.cfg\n");
}

void ps4ReadControlsConfig()
{
    const char* path = PS4_DATA_PATH "/ps4_controls.cfg";
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        // First run: keep defaults and drop a commented template to edit.
        ps4WriteDefaultControlsConfig(path);
        ps4Log("[ps4] controls: defaults (touch=%.2f)\n", g_ps4Controls.touchSensitivity);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        float fv;
        int iv;
        char wrs[16];
        if (sscanf(line, "world_right_stick=%15s", wrs) == 1) {
            int r = ps4ParseWrs(wrs);
            if (r >= 0) g_ps4Controls.worldRightStick = r;
        } else if (sscanf(line, "world_left_stick=%15s", wrs) == 1) {
            int r = ps4ParseWrs(wrs);
            if (r >= 0) g_ps4Controls.worldLeftStick = r;
        } else if (sscanf(line, "combat_auto_aim=%d", &iv) == 1) g_ps4Controls.combatAutoAim = iv;
        else if (sscanf(line, "world_auto_aim=%d", &iv) == 1) g_ps4Controls.worldAutoAim = iv;
        else if (sscanf(line, "auto_aim_whole_map=%d", &iv) == 1) g_ps4Controls.autoAimWholeMap = iv;
        else if (sscanf(line, "auto_aim_center_camera=%d", &iv) == 1) g_ps4Controls.autoAimCenterCamera = iv;
        else if (sscanf(line, "melee_approach=%d", &iv) == 1) g_ps4Controls.meleeApproach = iv;
        else if (sscanf(line, "loot_assist=%d", &iv) == 1) g_ps4Controls.lootAssist = iv;
        else if (sscanf(line, "loot_snap_distance=%d", &iv) == 1) g_ps4Controls.lootSnapDistance = iv;
        else if (sscanf(line, "loot_skip_empty=%d", &iv) == 1) g_ps4Controls.lootSkipEmpty = iv;
        else if (sscanf(line, "circle_close_all=%d", &iv) == 1) g_ps4Controls.circleCloseAll = iv;
        else if (sscanf(line, "touch_sensitivity=%f", &fv) == 1) g_ps4Controls.touchSensitivity = fv;
        else if (sscanf(line, "touch_accel=%f", &fv) == 1) g_ps4Controls.touchAccel = fv;
        else if (sscanf(line, "touch_smoothing=%f", &fv) == 1) g_ps4Controls.touchSmoothing = fv;
        else if (sscanf(line, "touch_deadzone=%f", &fv) == 1) g_ps4Controls.touchDeadzone = fv;
        else if (sscanf(line, "cursor_speed=%f", &fv) == 1) g_ps4Controls.cursorSpeed = fv;
        else if (sscanf(line, "cursor_speed_boost=%f", &fv) == 1) g_ps4Controls.cursorSpeedBoost = fv;
        else if (sscanf(line, "walk_deadzone=%f", &fv) == 1) g_ps4Controls.walkDeadzone = fv;
        else if (sscanf(line, "run_threshold=%f", &fv) == 1) g_ps4Controls.runThreshold = fv;
        else if (sscanf(line, "camera_deadzone_x=%d", &iv) == 1) g_ps4Controls.cameraDeadzoneX = iv;
        else if (sscanf(line, "camera_deadzone_y=%d", &iv) == 1) g_ps4Controls.cameraDeadzoneY = iv;
        else if (sscanf(line, "walk_project_tiles=%d", &iv) == 1) g_ps4Controls.walkProjectTiles = iv;
        else if (sscanf(line, "run_settle_ms=%d", &iv) == 1) g_ps4Controls.runSettleMs = iv;
        else if (sscanf(line, "touch_max_gain=%f", &fv) == 1) g_ps4Controls.touchMaxGain = fv;
        else if (sscanf(line, "touch_tap_ms=%d", &iv) == 1) g_ps4Controls.touchTapMs = iv;
        else if (sscanf(line, "touch_tap_move=%d", &iv) == 1) g_ps4Controls.touchTapMove = iv;
        else if (sscanf(line, "dpad_repeat_ms=%d", &iv) == 1) g_ps4Controls.dpadRepeatMs = iv;
        else if (sscanf(line, "camera_pan_threshold=%f", &fv) == 1) g_ps4Controls.cameraPanThreshold = fv;
        else if (ps4ParseButtonLine(line, "btn_cross", &g_ps4Controls.actCross)) { }
        else if (ps4ParseButtonLine(line, "btn_circle", &g_ps4Controls.actCircle)) { }
        else if (ps4ParseButtonLine(line, "btn_square", &g_ps4Controls.actSquare)) { }
        else if (ps4ParseButtonLine(line, "btn_triangle", &g_ps4Controls.actTriangle)) { }
        else if (ps4ParseButtonLine(line, "btn_l1", &g_ps4Controls.actL1)) { }
        else if (ps4ParseButtonLine(line, "btn_r1", &g_ps4Controls.actR1)) { }
        else if (ps4ParseButtonLine(line, "btn_l3", &g_ps4Controls.actL3)) { }
        else if (ps4ParseButtonLine(line, "btn_r3", &g_ps4Controls.actR3)) { }
        else if (ps4ParseButtonLine(line, "btn_share", &g_ps4Controls.actShare)) { }
        else if (ps4ParseButtonLine(line, "btn_options", &g_ps4Controls.actOptions)) { }
    }
    fclose(f);
    // Clamp to sane ranges so a typo can't make control impossible.
    if (g_ps4Controls.touchSensitivity <= 0.0f) g_ps4Controls.touchSensitivity = 0.25f;
    if (g_ps4Controls.touchAccel < 0.0f) g_ps4Controls.touchAccel = 0.0f;
    if (g_ps4Controls.touchSmoothing < 0.0f) g_ps4Controls.touchSmoothing = 0.0f;
    if (g_ps4Controls.touchSmoothing > 0.9f) g_ps4Controls.touchSmoothing = 0.9f;
    if (g_ps4Controls.touchDeadzone < 0.0f) g_ps4Controls.touchDeadzone = 0.0f;
    if (g_ps4Controls.walkDeadzone < 0.05f) g_ps4Controls.walkDeadzone = 0.05f;
    if (g_ps4Controls.walkDeadzone > 0.90f) g_ps4Controls.walkDeadzone = 0.90f;
    if (g_ps4Controls.runThreshold < g_ps4Controls.walkDeadzone) g_ps4Controls.runThreshold = g_ps4Controls.walkDeadzone;
    if (g_ps4Controls.cameraDeadzoneX < 0) g_ps4Controls.cameraDeadzoneX = 0;
    if (g_ps4Controls.cameraDeadzoneY < 0) g_ps4Controls.cameraDeadzoneY = 0;
    if (g_ps4Controls.walkProjectTiles < 1) g_ps4Controls.walkProjectTiles = 1;
    if (g_ps4Controls.walkProjectTiles > 8) g_ps4Controls.walkProjectTiles = 8;
    if (g_ps4Controls.runSettleMs < 0) g_ps4Controls.runSettleMs = 0;
    if (g_ps4Controls.runSettleMs > 500) g_ps4Controls.runSettleMs = 500;
    if (g_ps4Controls.touchMaxGain < 1.0f) g_ps4Controls.touchMaxGain = 1.0f;
    if (g_ps4Controls.touchTapMs < 0) g_ps4Controls.touchTapMs = 0;
    if (g_ps4Controls.touchTapMove < 0) g_ps4Controls.touchTapMove = 0;
    if (g_ps4Controls.dpadRepeatMs < 30) g_ps4Controls.dpadRepeatMs = 30;
    if (g_ps4Controls.cameraPanThreshold < 0.1f) g_ps4Controls.cameraPanThreshold = 0.1f;
    if (g_ps4Controls.cameraPanThreshold > 0.95f) g_ps4Controls.cameraPanThreshold = 0.95f;
    ps4Log("[ps4] controls: touch=%.2f cursor=%.0f/%.0f dz=%.2f run=%.2f cam=%d/%d\n",
        g_ps4Controls.touchSensitivity, g_ps4Controls.cursorSpeed, g_ps4Controls.cursorSpeedBoost,
        g_ps4Controls.walkDeadzone, g_ps4Controls.runThreshold,
        g_ps4Controls.cameraDeadzoneX, g_ps4Controls.cameraDeadzoneY);
}

// Makes the console's game data (PS4_DATA_PATH = /data/fallout2) reachable and
// chdir's into it so the engine's cwd-relative file I/O finds master.dat & co.
//
// We permanently jailbreak our credentials here: jbc_jailbreak_cred sets the
// process into prison0 with cdir/rdir/jdir = the real rootvnode, escaping the
// app sandbox so absolute paths resolve on the real filesystem. A nullfs-mount-
// only approach (keeping normal creds) does NOT work: a sandboxed process gets
// EPERM traversing a mount whose backing lives in the system prison (device
// 2026-07-07: mount succeeded, chdir returned EPERM).
//
// This MUST run AFTER SDL_Init. Doing the cred swap earlier makes the VideoOut/
// mbus handshake in PS4_VideoInit reject our creds and the app SIGSYSes right
// after piglet init (observed on 9.00). By here video init has completed.
// Logs up to 16 entries of a directory to klog (post-jailbreak diagnostics).
static void ps4ListDir(const char* dir)
{
    DIR* d = opendir(dir);
    if (d == nullptr) {
        ps4Log("[ps4] opendir(%s) failed errno=%d\n", dir, errno);
        return;
    }
    ps4Log("[ps4] listing %s:\n", dir);
    struct dirent* e;
    int n = 0;
    while ((e = readdir(d)) != nullptr && n < 16) {
        ps4Log("[ps4]   %s\n", e->d_name);
        n++;
    }
    closedir(d);
}

bool ps4MountData()
{
    jbc_cred cred;
    jbc_get_cred(&cred);
    jbc_jailbreak_cred(&cred);
    int cr = jbc_set_cred(&cred);
    ps4Log("[ps4] jbc jailbreak cred -> %d\n", cr);

    // The user's data lives at /data/fallout2 as seen over GoldHEN FTP, but the
    // jailbroken (real-root) view can differ (e.g. /data vs /user/data, or an
    // external drive at /mnt/usb0). Probe likely roots and chdir into whichever
    // actually contains master.dat.
    static const char* const kCandidates[] = {
        "/data/fallout2",
        "/user/data/fallout2",
        "/mnt/usb0/fallout2",
        "/mnt/usb1/fallout2",
    };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i) {
        const char* dir = kCandidates[i];
        char master[512];
        snprintf(master, sizeof(master), "%s/master.dat", dir);
        if (access(master, F_OK) == 0) {
            if (chdir(dir) == 0) {
                // The engine opens data files via RELATIVE paths against cwd
                // (dbOpen("master.dat")). Our access() check above used an
                // ABSOLUTE path, which says nothing about whether chdir()
                // actually repoints relative lookups (OpenOrbis libc has known
                // POSIX gaps here, and the cred-jailbreak's own cdir swap adds
                // another way this could silently not stick). Verify BOTH
                // getcwd() and an actual relative open before trusting chdir.
                char cwdBuf[512];
                const char* cwd = getcwd(cwdBuf, sizeof(cwdBuf));
                ps4Log("[ps4] using data dir %s (getcwd=%s)\n", dir, cwd != nullptr ? cwd : "NULL");

                FILE* f = compat_fopen("master.dat", "rb");
                if (f != nullptr) {
                    fclose(f);
                    ps4Log("[ps4] relative fopen(master.dat) OK\n");
                } else {
                    ps4Log("[ps4] relative fopen(master.dat) FAILED errno=%d\n", errno);
                }

                return true;
            }
            ps4Log("[ps4] chdir(%s) failed errno=%d\n", dir, errno);
        } else {
            ps4Log("[ps4] no master.dat at %s (errno=%d)\n", dir, errno);
        }
    }

    // Nothing matched — dump a few roots so klog shows where the data really is.
    ps4ListDir("/data");
    ps4ListDir("/user/data");
    ps4ListDir("/mnt/usb0");
    ps4Log("[ps4] game data not found in any candidate path\n");
    return false;
}

} // namespace fallout

#endif // __PS4__
