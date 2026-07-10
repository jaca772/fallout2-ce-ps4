#ifndef FALLOUT_PLATFORM_PS4_PS4_H_
#define FALLOUT_PLATFORM_PS4_PS4_H_

#ifdef __PS4__

namespace fallout {

// Absolute path on the console where the user copies their Fallout 2 data
// (master.dat, critter.dat, patch000.dat, data/, sound/, saves, config).
// See CLAUDE.md "Filesystem layout on console".
#define PS4_DATA_PATH "/data/fallout2"

// Pre-SDL setup: points SDL's OpenOrbis backend at the bundled piglet/shacc
// modules and pre-loads them. Must run BEFORE SDL_Init (piglet is set up during
// SDL video init). Does NOT touch credentials, so SDL's system-service init is
// left untouched. Safe to call unconditionally.
// Writes a printf-formatted line to the console debug channel (klog). Works
// before SDL_Init and before the heap is up — unlike SDL_Log (its sink is only
// installed during SDL_Init) and printf (its stdout is not wired on this port).
void ps4Log(const char* fmt, ...);

void ps4PreSdlInit();

// Sets Piglet/EGL memory configuration before the first SDL window creation.
// SDL's PS4 backend does not do this on the bundled-module path, but Piglet
// expects it before eglGetDisplay().
bool ps4ConfigurePigletForEgl(int width, int height);

// Reads [screen] resolution_x/resolution_y/scale from fallout2.cfg so the PS4
// bootstrap window matches the resolution the engine will use. Must run after
// ps4MountData(). Outputs default to 640x480 / scale 1 when unset.
void ps4ReadDisplayConfig(int* outWidth, int* outHeight, int* outScale);

// Actions a physical button can be mapped to (see the btn_* config keys).
enum {
    PS4_ACT_NONE = 0,
    PS4_ACT_LMB,         // left mouse button (click/confirm)
    PS4_ACT_RMB,         // right mouse button (cycle cursor mode)
    PS4_ACT_CURSORBOOST, // hold: faster stick cursor
    PS4_ACT_ESC,         // Escape (menu / cancel / skip movies)
    PS4_ACT_INVENTORY,   // 'i'
    PS4_ACT_CHARSHEET,   // 'c'
    PS4_ACT_SKILLDEX,    // 's'
    PS4_ACT_PIPBOY,      // 'p'
    PS4_ACT_COMBAT,      // start combat (out) / end turn — or end fight when safe (in)
    PS4_ACT_ENDCOMBAT,   // end combat (in combat only)
    PS4_ACT_RELOAD,      // reload the active-hand weapon
    PS4_ACT_WEAPONMODE,  // cycle the active weapon's attack mode (fists<->kick, single/aimed/burst)
    PS4_ACT_SWAPHANDS,   // switch to the other equipped weapon
};

// Role a stick can play in the world view (applies to either stick — see
// worldLeftStick / worldRightStick).
enum {
    PS4_WRS_CAMERA = 0,   // pan the view / look around
    PS4_WRS_CURSOR = 1,   // move the cursor freely (MacBook-like pointer)
    PS4_WRS_OFF = 2,      // nothing
    PS4_WRS_CENTERED = 3, // pan the view with the cursor locked to screen centre;
                          // at the map edge the cursor frees toward the border
    PS4_WRS_WALK = 4,     // march the character (continuous walk/run) — left stick only
};

// Melee/unarmed behaviour when firing (Cross) on an out-of-range target while
// aim-assisting in combat. See combatGamepadApproachAndAttack.
enum {
    PS4_MELEE_OFF = 0,      // don't auto-approach; the engine shows "out of range"
    PS4_MELEE_APPROACH = 1, // run up to the target but don't swing (leaves the attack to you) — default
    PS4_MELEE_ATTACK = 2,   // run up to the target and then attack
};

// User-tunable gamepad settings, loaded from /data/fallout2/ps4_controls.cfg so
// they can be edited over FTP without rebuilding. See ps4ReadControlsConfig.
struct Ps4ControlsConfig {
    float touchSensitivity;  // DS4 touchpad base gain when moving SLOWLY (precision)
    float touchAccel;        // extra gain per unit of finger speed (0 = linear/off)
    float touchSmoothing;    // 0..0.9 low-pass on finger motion (higher = smoother/laggier)
    float touchDeadzone;     // ignore finger moves smaller than this (kills drift)
    float cursorSpeed;       // left-stick cursor px/sec (combat/menus)
    float cursorSpeedBoost;  // ...with R1 held
    float walkDeadzone;      // left-stick magnitude below which we don't walk
    float runThreshold;      // ...at/above which we run instead of walk
    int cameraDeadzoneX;     // camera-follow dead-zone half-width  (px)
    int cameraDeadzoneY;     // camera-follow dead-zone half-height (px)
    int worldRightStick;     // right-stick role in world: PS4_WRS_CAMERA/CURSOR/OFF
    // Advanced feel tuning (kept at struct end for positional init).
    int walkProjectTiles;      // hexes to project each walk step (smoothness vs response)
    int runSettleMs;           // ms before the first walk step (helps run detection)
    float touchMaxGain;        // cap on touchpad acceleration gain
    int touchTapMs;            // max touch duration counted as a tap/click
    int touchTapMove;          // max touch movement counted as a tap/click
    int dpadRepeatMs;          // D-pad arrow-key repeat interval (ms)
    float cameraPanThreshold;  // stick deflection needed to pan (camera/centered)
    // Button remapping (each is a PS4_ACT_* value).
    int actCross;      // Cross (X)
    int actCircle;     // Circle (O)
    int actSquare;     // Square
    int actTriangle;   // Triangle
    int actL1;         // L1
    int actR1;         // R1
    int actL3;         // left stick click
    int actR3;         // right stick click
    int actShare;      // Share
    int actOptions;    // Options (read via native scePad)
    // World left-stick role (PS4_WRS_*). Kept at struct end for positional init.
    // Default WALK's replacement is CENTERED; see ps4.cc. Right stick default CURSOR.
    int worldLeftStick;
    // Aim assist (hold L2). combatAutoAim: snap/target in combat. worldAutoAim:
    // snap to people to talk to / interact out of combat. autoAimWholeMap: 1 =
    // consider the whole map, 0 = only targets in the player's viewport.
    int combatAutoAim;
    int worldAutoAim;
    int autoAimWholeMap;
    int autoAimCenterCamera; // 1 = pan the camera to centre the aimed target
    int meleeApproach;       // PS4_MELEE_* : out-of-range melee -> run up (+ attack)
};

extern Ps4ControlsConfig g_ps4Controls;

// Loads g_ps4Controls from /data/fallout2/ps4_controls.cfg (writing a commented
// default template if the file doesn't exist yet). Must run after ps4MountData().
void ps4ReadControlsConfig();

// Mounts the console's /data into the sandbox (via libjbc) and chdir's to
// PS4_DATA_PATH. Must run AFTER SDL_Init but before the engine reads game data:
// libjbc briefly manipulates kernel credentials, which corrupts SDL's VideoOut/
// mbus handshake if done before SDL_Init (observed: SIGSYS in PS4_VideoInit).
// Returns true on success; on failure the engine's data checks report it.
bool ps4MountData();

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_H_
