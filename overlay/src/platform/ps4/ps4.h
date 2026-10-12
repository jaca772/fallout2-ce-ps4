#ifndef FALLOUT_PLATFORM_PS4_PS4_H_
#define FALLOUT_PLATFORM_PS4_PS4_H_

#ifdef __PS4__

namespace fallout {

// Dynamic game data path on the console (e.g. /data/fallout2, /data/FalloutSonora).
const char* ps4GetDataPath();
void ps4SetDataPath(const char* path);
#define PS4_DATA_PATH ps4GetDataPath()

// Write guard for the jailbroken (prison0) process: true only if [nativePath] is
// the active game directory or lies below it, or is the boot config file.
// The path is lexically normalized first, so ".." cannot be used to escape.
// Symlinks are not resolved.
bool ps4IsPathWritable(const char* nativePath);

// Writes a printf-formatted line to the console debug channel (klog). Works
// before SDL_Init and before the heap is up — unlike SDL_Log (its sink is only
// installed during SDL_Init) and printf (its stdout is not wired on this port).
void ps4Log(const char* fmt, ...);

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

enum Ps4ScaleMode {
    PS4_SCALE_LETTERBOX = 0, // Preserve aspect ratio with pillarbox/letterbox black bars (default)
    PS4_SCALE_FILL = 1,      // Stretch to fill 100% of 16:9 output display
    PS4_SCALE_INTEGER = 2,   // Center with nearest integer scale factor
};

// User-tunable gamepad settings, loaded from /data/fallout2/ps4_controls.cfg so
// they can be edited over FTP without rebuilding. See ps4ReadControlsConfig.
struct Ps4ControlsConfig {
    float touchSensitivity = 0.25f;  // DS4 touchpad base gain when moving SLOWLY (precision)
    float touchAccel = 0.001f;        // extra gain per unit of finger speed (0 = linear/off)
    float touchSmoothing = 0.40f;    // 0..0.9 low-pass on finger motion (higher = smoother/laggier)
    float touchDeadzone = 2.0f;     // ignore finger moves smaller than this (kills drift)
    float cursorSpeed = 450.0f;       // left-stick cursor px/sec (combat/menus)
    float cursorSpeedBoost = 900.0f;  // ...with R1 held
    float walkDeadzone = 0.30f;      // left-stick magnitude below which we don't walk
    float runThreshold = 0.50f;      // ...at/above which we run instead of walk
    int cameraDeadzoneX = 100;     // camera-follow dead-zone half-width  (px)
    int cameraDeadzoneY = 75;     // camera-follow dead-zone half-height (px)
    int worldRightStick = PS4_WRS_CURSOR;     // right-stick role in world: PS4_WRS_CAMERA/CURSOR/OFF
    // Advanced feel tuning
    int walkProjectTiles = 2;      // hexes to project each walk step (smoothness vs response)
    int runSettleMs = 50;           // ms before the first walk step (helps run detection)
    float touchMaxGain = 4.0f;        // cap on touchpad acceleration gain
    int touchTapMs = 200;            // max touch duration counted as a tap/click
    int touchTapMove = 40;          // max touch movement counted as a tap/click
    int dpadRepeatMs = 150;          // D-pad arrow-key repeat interval (ms)
    float cameraPanThreshold = 0.40f;  // stick deflection needed to pan (camera/centered)
    // Button remapping (each is a PS4_ACT_* value).
    int actCross = PS4_ACT_LMB;      // Cross (X)
    int actCircle = PS4_ACT_RMB;     // Circle (O)
    int actSquare = PS4_ACT_SKILLDEX;     // Square
    int actTriangle = PS4_ACT_CHARSHEET;   // Triangle
    int actL1 = PS4_ACT_INVENTORY;         // L1
    int actR1 = PS4_ACT_RELOAD;         // R1
    int actL3 = PS4_ACT_SWAPHANDS;         // left stick click
    int actR3 = PS4_ACT_COMBAT;         // right stick click
    int actShare = PS4_ACT_PIPBOY;      // Share
    int actOptions = PS4_ACT_ESC;    // Options (read via native scePad)
    int worldLeftStick = PS4_WRS_CENTERED;
    // Aim assist (hold L2)
    int combatAutoAim = 1;
    int worldAutoAim = 1;
    int autoAimWholeMap = 0;
    int autoAimCenterCamera = 0; // 1 = pan the camera to centre the aimed target
    int meleeApproach = PS4_MELEE_APPROACH;       // PS4_MELEE_* : out-of-range melee -> run up (+ attack)
    int lootAssist = 1;          // 1 = L2+L1 snaps onto containers/corpses/doors
    int lootSnapDistance = 20;    // loot-assist reach in hexes
    int lootSkipEmpty = 1;       // 1 = skip empty containers/corpses when cycling
    int circleCloseAll = 0;      // 1 = Circle closes EVERY menu
    int scaleMode = PS4_SCALE_LETTERBOX;           // Ps4ScaleMode (0 = letterbox/pillarbox, 1 = fill 16:9, 2 = integer)
    float stickDeadzone = 0.15f;     // radial analog stick deadzone (0.05..0.70, default 0.15)
    float stickCurve = 1.50f;        // analog response curve exponent (1.0..3.0, default 1.50)
    int imeEnabled = 1;              // 1 = use native PS4 onscreen keyboard for text fields
    int imeAutoconfirm = 1;          // 1 = automatically confirm input when keyboard is done
};

extern Ps4ControlsConfig g_ps4Controls;

// Loads g_ps4Controls from /data/fallout2/ps4_controls.cfg (writing a commented
// default template if the file doesn't exist yet). Must run after ps4MountData().
void ps4ReadControlsConfig();

// Escapes the sandbox via libjbc credential jailbreak (prison0) and chdir's to
// the selected game folder. Must run AFTER SDL_Init but before the engine reads
// game data: libjbc manipulates kernel credentials, which corrupts SDL's service
// handshake if done before SDL_Init (observed: SIGSYS).
// Returns true on success; on failure the engine's data checks report it.
bool ps4MountData();

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_H_
