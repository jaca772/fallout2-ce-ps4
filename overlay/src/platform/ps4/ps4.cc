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
#include <orbis/SystemService.h>

#include <jbc/libjbc.h>

#include "platform_compat.h"
#include "platform/ps4/ps4_boot_menu.h"
#include "platform/ps4/ps4_path.h"
#include "platform/ps4/ps4_video.h"

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
#include "platform/ps4/ps4_video.h"

static void ps4EarlyLog(const char* fmt, ...);

static jbc_cred g_ps4OrigCred;
static bool g_ps4OrigCredValid = false;

extern "C" [[noreturn]] void __wrap_exit(int status)
{
    ps4EarlyLog("[ps4] exit(%d) -> preparing clean exit\n", status);
    fflush(nullptr);
    chdir("/");
    sync();
    fallout::ps4VideoShutdown();
    if (g_ps4OrigCredValid) {
        ps4EarlyLog("[ps4] restoring original sandbox credentials\n");
        jbc_set_cred(&g_ps4OrigCred);
    }
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
void* sceLibcMspaceMemalign(void* mspace, size_t alignment, size_t size);
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

extern "C" void* __wrap_memalign(size_t alignment, size_t size)
{
    return sceLibcMspaceMemalign(ps4Mspace(), alignment, size);
}

extern "C" void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    return sceLibcMspaceMemalign(ps4Mspace(), alignment, size);
}

extern "C" void* aligned_alloc(size_t alignment, size_t size)
    __attribute__((weak, alias("__wrap_aligned_alloc")));

extern "C" int __wrap_posix_memalign(void** memptr, size_t alignment, size_t size)
{
    if (memptr == nullptr) {
        return 22; // EINVAL
    }
    if ((alignment % sizeof(void*) != 0) || (alignment & (alignment - 1)) != 0) {
        return 22; // EINVAL
    }
    void* ptr = sceLibcMspaceMemalign(ps4Mspace(), alignment, size);
    if (ptr == nullptr) {
        return 12; // ENOMEM
    }
    *memptr = ptr;
    return 0;
}

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

static char g_ps4DataPath[512] = "/data/fallout2";

const char* ps4GetDataPath()
{
    return g_ps4DataPath;
}

void ps4SetDataPath(const char* path)
{
    if (path != nullptr && *path != '\0') {
        strncpy(g_ps4DataPath, path, sizeof(g_ps4DataPath) - 1);
        g_ps4DataPath[sizeof(g_ps4DataPath) - 1] = '\0';
    }
}

// Security boundary for the prison0 escaped filesystem access.
// Only allows writes/modifications within the active game directory ps4GetDataPath()
// (including USB mounts /mnt/usb0, /mnt/usb1) and the boot config /data/f2_active_game.txt.
// Both sides are normalized first so ".." components cannot escape the directory.
bool ps4IsPathWritable(const char* nativePath)
{
    char path[1024];
    if (!ps4PathNormalize(nativePath, path, sizeof(path)) || path[0] != '/') {
        return false;
    }

    if (strcmp(path, "/data/f2_active_game.txt") == 0) {
        return true;
    }

    char base[1024];
    if (!ps4PathNormalize(ps4GetDataPath(), base, sizeof(base)) || base[0] != '/') {
        return false;
    }

    return ps4PathIsUnder(path, base);
}

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
    bool hasScreenSection = false;
    char cfgPath[512];
    snprintf(cfgPath, sizeof(cfgPath), "%s/fallout2.cfg", ps4GetDataPath());
    FILE* f = fopen(cfgPath, "r");
    if (f != nullptr) {
        char line[256];
        bool inScreen = false;
        while (fgets(line, sizeof(line), f) != nullptr) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '[') {
                inScreen = (strncmp(p, "[screen]", 8) == 0);
                if (inScreen) {
                    hasScreenSection = true;
                }
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
        ps4Log("[ps4] display config: fallout2.cfg not readable\n");
    }

    // Fallback: check f2_res.ini (used by total conversion mods like Sonora/Nevada)
    if (!hasScreenSection) {
        char resPath[512];
        snprintf(resPath, sizeof(resPath), "%s/f2_res.ini", ps4GetDataPath());
        FILE* rf = fopen(resPath, "r");
        if (rf != nullptr) {
            char line[256];
            bool inMain = false;
            while (fgets(line, sizeof(line), rf) != nullptr) {
                char* p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '[') {
                    inMain = (strncmp(p, "[MAIN]", 6) == 0);
                    continue;
                }
                if (!inMain) continue;
                int v;
                if (sscanf(p, "SCR_WIDTH=%d", &v) == 1) resX = v;
                else if (sscanf(p, "SCR_HEIGHT=%d", &v) == 1) resY = v;
                else if (sscanf(p, "SCALE_2X=%d", &v) == 1) scale = v + 1;
            }
            fclose(rf);
            ps4Log("[ps4] display config fallback to f2_res.ini: %dx%d scale=%d\n", resX, resY, scale);
        }
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

// User-tunable gamepad settings with sensible defaults (see Ps4ControlsConfig
// default member initializers). Overridden by /data/fallout2/ps4_controls.cfg.
Ps4ControlsConfig g_ps4Controls;

static const char* ps4ScaleModeName(int m)
{
    switch (m) {
    case PS4_SCALE_FILL: return "fill";
    case PS4_SCALE_INTEGER: return "integer";
    case PS4_SCALE_LETTERBOX:
    default:
        return "letterbox";
    }
}

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
        "# Display scaling mode on 1080p panel:\n"
        "#  letterbox = preserve original aspect ratio (4:3 gets pillarbox black bars, 16:9 fills with no bars)\n"
        "#  fill      = stretch to fill 100%% of 16:9 screen\n"
        "#  integer   = integer scaling factor centered\n"
        "scale_mode=%s\n"
        "\n"
        "# Analog stick feel:\n"
        "#  stick_deadzone = radial deadzone threshold (0.05..0.70, default 0.15)\n"
        "#  stick_curve    = response curve exponent (1.0 = linear, 1.5 = smooth modern console curve)\n"
        "stick_deadzone=%.2f\n"
        "stick_curve=%.2f\n"
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
        "btn_options=%s\n"
        "\n"
        "# On-screen keyboard (PS4 native IME)\n"
        "#  ime = 1 to use native PS4 keyboard for text fields (character name, save description, file name)\n"
        "#  ime_autoconfirm = 1 to automatically confirm input when keyboard is done\n"
        "ime=%d\n"
        "ime_autoconfirm=%d\n",
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
        ps4ScaleModeName(g_ps4Controls.scaleMode),
        g_ps4Controls.stickDeadzone, g_ps4Controls.stickCurve,
        g_ps4Controls.walkProjectTiles, g_ps4Controls.runSettleMs,
        g_ps4Controls.touchMaxGain, g_ps4Controls.touchTapMs,
        g_ps4Controls.touchTapMove, g_ps4Controls.dpadRepeatMs,
        g_ps4Controls.cameraPanThreshold,
        ps4ActionName(g_ps4Controls.actCross), ps4ActionName(g_ps4Controls.actCircle),
        ps4ActionName(g_ps4Controls.actSquare), ps4ActionName(g_ps4Controls.actTriangle),
        ps4ActionName(g_ps4Controls.actL1), ps4ActionName(g_ps4Controls.actR1),
        ps4ActionName(g_ps4Controls.actL3), ps4ActionName(g_ps4Controls.actR3),
        ps4ActionName(g_ps4Controls.actShare), ps4ActionName(g_ps4Controls.actOptions),
        g_ps4Controls.imeEnabled, g_ps4Controls.imeAutoconfirm);
    fclose(f);
    ps4Log("[ps4] wrote default ps4_controls.cfg\n");
}

static char* ps4Trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return s;
    char* end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

void ps4ReadControlsConfig()
{
    char path[512];
    snprintf(path, sizeof(path), "%s/ps4_controls.cfg", ps4GetDataPath());
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        // First run: keep defaults and drop a commented template to edit.
        ps4WriteDefaultControlsConfig(path);
        ps4VideoSetScaleMode((Ps4ScaleMode)g_ps4Controls.scaleMode);
        ps4Log("[ps4] controls: defaults (touch=%.2f scale=%s dz=%.2f curve=%.2f)\n",
            g_ps4Controls.touchSensitivity, ps4ScaleModeName(g_ps4Controls.scaleMode),
            g_ps4Controls.stickDeadzone, g_ps4Controls.stickCurve);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char* eq = strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char* key = ps4Trim(line);
        char* val = ps4Trim(eq + 1);
        if (*key == '#' || *key == '[' || *key == '\0') {
            continue;
        }

        if (strcmp(key, "scale_mode") == 0) {
            if (strcmp(val, "fill") == 0) g_ps4Controls.scaleMode = PS4_SCALE_FILL;
            else if (strcmp(val, "integer") == 0) g_ps4Controls.scaleMode = PS4_SCALE_INTEGER;
            else g_ps4Controls.scaleMode = PS4_SCALE_LETTERBOX;
            ps4VideoSetScaleMode((Ps4ScaleMode)g_ps4Controls.scaleMode);
        } else if (strcmp(key, "stick_deadzone") == 0) {
            g_ps4Controls.stickDeadzone = (float)atof(val);
        } else if (strcmp(key, "stick_curve") == 0) {
            g_ps4Controls.stickCurve = (float)atof(val);
        } else if (strcmp(key, "world_right_stick") == 0) {
            int r = ps4ParseWrs(val);
            if (r >= 0) g_ps4Controls.worldRightStick = r;
        } else if (strcmp(key, "world_left_stick") == 0) {
            int r = ps4ParseWrs(val);
            if (r >= 0) g_ps4Controls.worldLeftStick = r;
        } else if (strcmp(key, "combat_auto_aim") == 0) {
            g_ps4Controls.combatAutoAim = atoi(val);
        } else if (strcmp(key, "world_auto_aim") == 0) {
            g_ps4Controls.worldAutoAim = atoi(val);
        } else if (strcmp(key, "auto_aim_whole_map") == 0) {
            g_ps4Controls.autoAimWholeMap = atoi(val);
        } else if (strcmp(key, "auto_aim_center_camera") == 0) {
            g_ps4Controls.autoAimCenterCamera = atoi(val);
        } else if (strcmp(key, "melee_approach") == 0) {
            g_ps4Controls.meleeApproach = atoi(val);
        } else if (strcmp(key, "loot_assist") == 0) {
            g_ps4Controls.lootAssist = atoi(val);
        } else if (strcmp(key, "loot_snap_distance") == 0) {
            g_ps4Controls.lootSnapDistance = atoi(val);
        } else if (strcmp(key, "loot_skip_empty") == 0) {
            g_ps4Controls.lootSkipEmpty = atoi(val);
        } else if (strcmp(key, "circle_close_all") == 0) {
            g_ps4Controls.circleCloseAll = atoi(val);
        } else if (strcmp(key, "touch_sensitivity") == 0) {
            g_ps4Controls.touchSensitivity = (float)atof(val);
        } else if (strcmp(key, "touch_accel") == 0) {
            g_ps4Controls.touchAccel = (float)atof(val);
        } else if (strcmp(key, "touch_smoothing") == 0) {
            g_ps4Controls.touchSmoothing = (float)atof(val);
        } else if (strcmp(key, "touch_deadzone") == 0) {
            g_ps4Controls.touchDeadzone = (float)atof(val);
        } else if (strcmp(key, "cursor_speed") == 0) {
            g_ps4Controls.cursorSpeed = (float)atof(val);
        } else if (strcmp(key, "cursor_speed_boost") == 0) {
            g_ps4Controls.cursorSpeedBoost = (float)atof(val);
        } else if (strcmp(key, "walk_deadzone") == 0) {
            g_ps4Controls.walkDeadzone = (float)atof(val);
        } else if (strcmp(key, "run_threshold") == 0) {
            g_ps4Controls.runThreshold = (float)atof(val);
        } else if (strcmp(key, "camera_deadzone_x") == 0) {
            g_ps4Controls.cameraDeadzoneX = atoi(val);
        } else if (strcmp(key, "camera_deadzone_y") == 0) {
            g_ps4Controls.cameraDeadzoneY = atoi(val);
        } else if (strcmp(key, "walk_project_tiles") == 0) {
            g_ps4Controls.walkProjectTiles = atoi(val);
        } else if (strcmp(key, "run_settle_ms") == 0) {
            g_ps4Controls.runSettleMs = atoi(val);
        } else if (strcmp(key, "touch_max_gain") == 0) {
            g_ps4Controls.touchMaxGain = (float)atof(val);
        } else if (strcmp(key, "touch_tap_ms") == 0) {
            g_ps4Controls.touchTapMs = atoi(val);
        } else if (strcmp(key, "touch_tap_move") == 0) {
            g_ps4Controls.touchTapMove = atoi(val);
        } else if (strcmp(key, "dpad_repeat_ms") == 0) {
            g_ps4Controls.dpadRepeatMs = atoi(val);
        } else if (strcmp(key, "camera_pan_threshold") == 0) {
            g_ps4Controls.cameraPanThreshold = (float)atof(val);
        } else if (strcmp(key, "btn_cross") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actCross = a;
        } else if (strcmp(key, "btn_circle") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actCircle = a;
        } else if (strcmp(key, "btn_square") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actSquare = a;
        } else if (strcmp(key, "btn_triangle") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actTriangle = a;
        } else if (strcmp(key, "btn_l1") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actL1 = a;
        } else if (strcmp(key, "btn_r1") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actR1 = a;
        } else if (strcmp(key, "btn_l3") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actL3 = a;
        } else if (strcmp(key, "btn_r3") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actR3 = a;
        } else if (strcmp(key, "btn_share") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actShare = a;
        } else if (strcmp(key, "btn_options") == 0) {
            int a = ps4ParseAction(val); if (a >= 0) g_ps4Controls.actOptions = a;
        } else if (strcmp(key, "ime") == 0) {
            g_ps4Controls.imeEnabled = atoi(val);
        } else if (strcmp(key, "ime_autoconfirm") == 0) {
            g_ps4Controls.imeAutoconfirm = atoi(val);
        }
    }
    fclose(f);

    // Clamp to sane ranges so a typo can't make control impossible.
    if (g_ps4Controls.touchSensitivity <= 0.0f) g_ps4Controls.touchSensitivity = 0.25f;
    if (g_ps4Controls.touchAccel < 0.0f) g_ps4Controls.touchAccel = 0.0f;
    if (g_ps4Controls.touchSmoothing < 0.0f) g_ps4Controls.touchSmoothing = 0.0f;
    if (g_ps4Controls.touchSmoothing > 0.9f) g_ps4Controls.touchSmoothing = 0.9f;
    if (g_ps4Controls.touchDeadzone < 0.0f) g_ps4Controls.touchDeadzone = 0.0f;
    if (g_ps4Controls.stickDeadzone < 0.05f) g_ps4Controls.stickDeadzone = 0.05f;
    if (g_ps4Controls.stickDeadzone > 0.70f) g_ps4Controls.stickDeadzone = 0.70f;
    if (g_ps4Controls.stickCurve < 1.0f) g_ps4Controls.stickCurve = 1.0f;
    if (g_ps4Controls.stickCurve > 3.0f) g_ps4Controls.stickCurve = 3.0f;
    if (g_ps4Controls.cursorSpeed < 50.0f) g_ps4Controls.cursorSpeed = 50.0f;
    if (g_ps4Controls.cursorSpeed > 2500.0f) g_ps4Controls.cursorSpeed = 2500.0f;
    if (g_ps4Controls.cursorSpeedBoost < 100.0f) g_ps4Controls.cursorSpeedBoost = 100.0f;
    if (g_ps4Controls.cursorSpeedBoost > 4000.0f) g_ps4Controls.cursorSpeedBoost = 4000.0f;
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
    if (g_ps4Controls.meleeApproach < 0) g_ps4Controls.meleeApproach = 0;
    if (g_ps4Controls.meleeApproach > 2) g_ps4Controls.meleeApproach = 2;
    if (g_ps4Controls.lootSnapDistance < 1) g_ps4Controls.lootSnapDistance = 1;
    if (g_ps4Controls.lootSnapDistance > 50) g_ps4Controls.lootSnapDistance = 50;
    g_ps4Controls.combatAutoAim = g_ps4Controls.combatAutoAim != 0 ? 1 : 0;
    g_ps4Controls.worldAutoAim = g_ps4Controls.worldAutoAim != 0 ? 1 : 0;
    g_ps4Controls.autoAimWholeMap = g_ps4Controls.autoAimWholeMap != 0 ? 1 : 0;
    g_ps4Controls.autoAimCenterCamera = g_ps4Controls.autoAimCenterCamera != 0 ? 1 : 0;
    g_ps4Controls.lootAssist = g_ps4Controls.lootAssist != 0 ? 1 : 0;
    g_ps4Controls.lootSkipEmpty = g_ps4Controls.lootSkipEmpty != 0 ? 1 : 0;
    g_ps4Controls.circleCloseAll = g_ps4Controls.circleCloseAll != 0 ? 1 : 0;

    ps4VideoSetScaleMode((Ps4ScaleMode)g_ps4Controls.scaleMode);

    ps4Log("[ps4] controls: touch=%.2f cursor=%.0f/%.0f dz=%.2f curve=%.2f scale=%s cam=%d/%d\n",
        g_ps4Controls.touchSensitivity, g_ps4Controls.cursorSpeed, g_ps4Controls.cursorSpeedBoost,
        g_ps4Controls.stickDeadzone, g_ps4Controls.stickCurve, ps4ScaleModeName(g_ps4Controls.scaleMode),
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
// This MUST run AFTER SDL_Init. Doing the cred swap earlier corrupts SDL's
// service handshake (observed: SIGSYS). By here SDL_Init has completed.
bool ps4MountData()
{
    jbc_get_cred(&g_ps4OrigCred);
    g_ps4OrigCredValid = true;
    jbc_cred cred = g_ps4OrigCred;
    jbc_jailbreak_cred(&cred);
    int cr = jbc_set_cred(&cred);
    ps4Log("[ps4] jbc jailbreak cred -> %d\n", cr);
    if (cr != 0) {
        ps4Log("[ps4] ERROR: jbc_set_cred failed (%d) - GoldHEN / jailbreak not active?\n", cr);
        ps4ShowJailbreakError();
        return false;
    }

    std::string chosenPath = ps4SelectGameFolder();
    if (chosenPath.empty()) {
        ps4Log("[ps4] ps4SelectGameFolder returned empty (no valid game found)\n");
        return false;
    }

    ps4SetDataPath(chosenPath.c_str());

    if (chdir(chosenPath.c_str()) == 0) {
        char cwdBuf[512];
        const char* cwd = getcwd(cwdBuf, sizeof(cwdBuf));
        ps4Log("[ps4] using data dir %s (getcwd=%s)\n", chosenPath.c_str(), cwd != nullptr ? cwd : "NULL");

        FILE* f = compat_fopen("master.dat", "rb");
        if (f != nullptr) {
            fclose(f);
            ps4Log("[ps4] relative fopen(master.dat) OK\n");
        } else {
            ps4Log("[ps4] relative fopen(master.dat) FAILED errno=%d\n", errno);
        }

        return true;
    }

    ps4Log("[ps4] chdir(%s) failed errno=%d\n", chosenPath.c_str(), errno);
    return false;
}

} // namespace fallout

#endif // __PS4__
