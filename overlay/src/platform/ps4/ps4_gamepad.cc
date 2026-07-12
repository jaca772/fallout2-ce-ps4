// PS4 DS4 gamepad -> mouse/keys layer. Extracted from dinput.cc (behavior
// unchanged) so dinput.cc keeps only tiny __PS4__ hooks. See ps4_gamepad.h.
#include "platform/ps4/ps4_gamepad.h"

#ifdef __PS4__

#include "input.h"
#include "kb.h"
#include "svga.h"
#include "sfall_kb_helpers.h"
#include <math.h>
#include <stdio.h>

#include "platform/ps4/ps4.h"

#include <orbis/Pad.h>
#include <orbis/UserService.h>

// Phase B (world movement): the gamepad drives the player character directly, so
// the PS4 input path needs the engine's movement/tile/context APIs.
#include "actions.h"
#include "animation.h"
#include "combat.h"
#include "display_monitor.h"
#include "game.h"
#include "game_mouse.h"
#include "interface.h"
#include "item.h"
#include "map.h"
#include "object.h"
#include "tile.h"

namespace fallout {

static SDL_GameController* g_gamepad = nullptr;
static float g_virtualMouseX = 320.0f;
static float g_virtualMouseY = 240.0f;
static uint32_t g_lastTicks = 0;
static bool g_prevButtons[SDL_CONTROLLER_BUTTON_MAX] = { false };
static uint32_t g_lastDpadTicks = 0;

// Native scePad handle, used ONLY to read the true button bitmask so we can tell
// the OPTIONS button apart from the touchpad click. The SDL-PS4 backend reports
// BOTH as SDL_CONTROLLER_BUTTON_START (joystick button 6), so a touchpad click
// would otherwise fire our Options->ESC mapping and pop the menu. scePad exposes
// them as separate bits (ORBIS_PAD_BUTTON_OPTIONS vs ORBIS_PAD_BUTTON_TOUCH_PAD).
// Handle is obtained via scePadGetHandle (the pad SDL already opened) — we do NOT
// open a second handle. -1 = unavailable, in which case we fall back to SDL.
static int32_t g_padHandle = -1;

// Touchpad-cursor state (see the DS4 touchpad section in mouseDeviceGetData).
// g_touchpadFingerActive: a finger is currently on the touchpad -> suppress
// world-walk so you can point instead. g_touchpadClickFrames: countdown holding a
// synthetic left-click after a tap, so the engine registers a press+release.
static bool g_touchpadFingerActive = false;
static int g_touchpadClickFrames = 0;

// Per-frame results of the remappable button processing (see the button section
// in mouseDeviceGetData). Held across the two __PS4__ blocks in that function.
static bool g_padLmb = false;   // a button mapped to LMB is held
static bool g_padRmb = false;   // a button mapped to RMB is held
static bool g_padBoost = false; // a button mapped to cursor-boost is held

// Key a keyboard-style action sends (0 for non-key actions).
static int ps4ActionKey(int action)
{
    switch (action) {
    case PS4_ACT_ESC: return KEY_ESCAPE;
    case PS4_ACT_INVENTORY: return 'i';
    case PS4_ACT_CHARSHEET: return 'c';
    case PS4_ACT_SKILLDEX: return 's';
    case PS4_ACT_PIPBOY: return 'p';
    case PS4_ACT_WEAPONMODE: return 'n'; // cycle attack mode (the weapon "red" button)
    case PS4_ACT_SWAPHANDS: return 'b';  // switch to the other equipped weapon
    default: return 0;
    }
}

// Apply a physical button's mapped action. Level actions (LMB/RMB/boost) raise
// the shared flags; key actions fire once on the press edge. *prev is updated.
static void ps4ApplyButton(int action, bool held, bool* prev)
{
    switch (action) {
    case PS4_ACT_LMB: if (held) g_padLmb = true; break;
    case PS4_ACT_RMB: if (held) g_padRmb = true; break;
    case PS4_ACT_CURSORBOOST: if (held) g_padBoost = true; break;
    case PS4_ACT_NONE: break;
    case PS4_ACT_COMBAT:
        // Out of combat: start a fight ('a'). In combat: end the player's turn,
        // or end the whole fight if nothing hostile still wants to attack — one
        // button covers both end-turn and end-combat.
        if (held && !*prev) {
            if (isInCombat()) {
                enqueueInputEvent(combatGamepadCanEndCombat() ? KEY_RETURN : KEY_SPACE);
            } else {
                enqueueInputEvent((int)'a');
            }
        }
        break;
    case PS4_ACT_ENDCOMBAT:
        // Leave the fight (only meaningful in combat).
        if (held && !*prev && isInCombat()) {
            enqueueInputEvent(KEY_RETURN);
        }
        break;
    case PS4_ACT_RELOAD:
        // Reload the active-hand weapon (spends AP in combat).
        if (held && !*prev) {
            interfaceGamepadReload();
        }
        break;
    default: {
        int key = ps4ActionKey(action);
        if (key != 0 && held && !*prev) {
            enqueueInputEvent(key);
        }
        break;
    }
    }
    *prev = held;
}

// Reads the native pad button bitmask. Returns false if scePad is unavailable.
static bool ps4PadButtons(uint32_t* outButtons)
{
    if (g_padHandle < 0) {
        return false;
    }
    OrbisPadData data;
    if (scePadReadState(g_padHandle, &data) != 0) {
        return false;
    }
    *outButtons = data.buttons;
    return true;
}

// --- Phase B: direct world movement (left stick) -----------------------------
// True when the left stick should walk the character rather than move the cursor:
// the free-roam exploration view (no UI window, not on the worldmap, not in
// combat). Combat and every menu/dialog fall back to the cursor system untouched.
static bool ps4InWorldWalkMode()
{
    // gGameLoaded gates out the main menu and character creation etc. — there
    // getCurrentGameMode() is also 0 and gDude is non-null, so without this the
    // cursor would be suppressed on the startup menu (can't click Load Game).
    return gGameLoaded
        && gDude != nullptr
        && GameMode::getCurrentGameMode() == 0
        && !isInCombat()
        && !gameUiIsDisabled();
}

// True when the isometric map view owns the screen — in the world OR in combat,
// but not while a full-screen menu is up (dialog, inventory, pipboy, worldmap,
// save/load, ...). Used to decide the right stick pans the camera (as it does in
// the world) rather than acting as a menu scroll wheel. Combat leaves the map on
// screen, so kCombat/kPlayerTurn are masked out; any other mode bit means a menu.
static bool ps4InMapView()
{
    if (!gGameLoaded || gDude == nullptr || gameUiIsDisabled()) {
        return false;
    }
    int mode = GameMode::getCurrentGameMode();
    mode &= ~(GameMode::kCombat | GameMode::kPlayerTurn);
    return mode == 0;
}

// Drive the player with the left stick. Called once per frame from mainLoop().
// Continuous march: light stick = walk, hard = run; camera follows the player;
// direction snaps to the nearest of the 6 hex rotations. No-op outside world mode.
void ps4GamepadWorldMove()
{
    // Tracks when the stick left the deadzone, so the very first step reflects the
    // settled deflection (run vs walk) instead of the mid-ramp first frame.
    static bool s_active = false;
    static uint32_t s_activeSince = 0;

    if (g_gamepad == nullptr || !ps4InWorldWalkMode()) {
        s_active = false;
        return;
    }

    // Only march when the left stick is configured to walk. Otherwise the left
    // stick plays another role (centered camera / cursor / ...) handled in
    // mouseDeviceGetData, and there is no character movement here.
    if (g_ps4Controls.worldLeftStick != PS4_WRS_WALK) {
        s_active = false;
        return;
    }

    // A finger on the DS4 touchpad means the player is pointing (cursor mode), not
    // walking — suppress movement until the finger lifts and the stick moves again.
    if (g_touchpadFingerActive) {
        s_active = false;
        return;
    }

    const float AXIS_MAX = 32768.0f;
    const float DEADZONE = g_ps4Controls.walkDeadzone;      // below this: not walking
    const float RUN_THRESHOLD = g_ps4Controls.runThreshold; // at/above: run, else walk
    const int PROJECT_TILES = g_ps4Controls.walkProjectTiles;   // hexes projected per step
    const uint32_t SETTLE_MS = (uint32_t)g_ps4Controls.runSettleMs; // settle before first step

    float lx = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX) / AXIS_MAX;
    float ly = SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY) / AXIS_MAX;
    float mag = sqrtf(lx * lx + ly * ly);
    if (mag < DEADZONE) {
        s_active = false;
        return; // centered: cursor system stays as-is
    }

    uint32_t now = SDL_GetTicks();
    if (!s_active) {
        s_active = true;
        s_activeSince = now;
    }

    // --- Smooth camera follow (engine edge-scroll mechanism) --------------------
    // mapScroll shifts the map buffer one tile (32x24 px) toward the player and
    // redraws only the exposed strip — fluid and glitch-free, self-throttled to
    // ~30 Hz. A dead-zone keeps the player roughly centered without jitter. This
    // runs every frame the stick is held (even mid-step). Do NOT use per-frame
    // tileSetCenter — it corrupts the scroll buffer (only the area around the
    // player refreshes).
    {
        int px, py, cx, cy;
        if (tileToScreenXY(gDude->tile, &px, &py) == 0
            && tileToScreenXY(gCenterTile, &cx, &cy) == 0) {
            int offx = px - cx; // how far the player is from the view centre (px)
            int offy = py - cy;
            const int DEADX = g_ps4Controls.cameraDeadzoneX; // dead-zone half-width  (px)
            const int DEADY = g_ps4Controls.cameraDeadzoneY; // dead-zone half-height (px)
            int sdx = (offx > DEADX) ? 1 : (offx < -DEADX ? -1 : 0);
            int sdy = (offy > DEADY) ? 1 : (offy < -DEADY ? -1 : 0);
            if (sdx != 0 || sdy != 0) {
                mapScroll(sdx, sdy);
            }
        }
    }

    // Only issue a new step when the previous one finished — continuous march.
    if (animationIsBusy(gDude)) {
        return;
    }

    // Settle window: on the first step from idle, wait briefly so a quick hard
    // push registers as a run from the start instead of taking a walk step first.
    // A light push never reaches RUN_THRESHOLD and just walks after the window.
    if ((now - s_activeSince) < SETTLE_MS) {
        return;
    }

    bool run = mag >= RUN_THRESHOLD;

    // Stick angle -> nearest hex direction. Screen up = -y, so atan2(-ly, lx)
    // gives right=0deg, up=+90deg. Sector centers: E=0, NE=60, NW=120, W=180,
    // SW=240, SE=300. (Most likely on-device tuning spot if directions are wrong.)
    const float kPi = 3.14159265358979f;
    float ang = atan2f(-ly, lx) * (180.0f / kPi);
    if (ang < 0.0f) {
        ang += 360.0f;
    }
    int idx = ((int)(ang / 60.0f + 0.5f)) % 6;
    static const int kIdxToRotation[6] = {
        ROTATION_E, ROTATION_NE, ROTATION_NW, ROTATION_W, ROTATION_SW, ROTATION_SE
    };
    int rotation = kIdxToRotation[idx];

    int destTile = tileGetTileInDirection(gDude->tile, rotation, PROJECT_TILES);

    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    if (run) {
        animationRegisterRunToTile(gDude, destTile, gDude->elevation, -1, 0);
    } else {
        animationRegisterMoveToTile(gDude, destTile, gDude->elevation, -1, 0);
    }
    reg_anim_end();
}

bool ps4GamepadIsOpen()
{
    return g_gamepad != nullptr;
}

void ps4GamepadPoll(int* wheelX, int* wheelY)
{
    // Gamepad to mouse emulation (Vita-style controls)
    if (g_gamepad != nullptr) {
        uint32_t currentTicks = SDL_GetTicks();
        if (g_lastTicks == 0) {
            g_lastTicks = currentTicks;
        }
        float dt = (float)(currentTicks - g_lastTicks) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f; // cap to 100ms to avoid jumps after pauses
        g_lastTicks = currentTicks;

        const int DEADZONE = 8000;
        const float AXIS_MAX = 32768.0f;

        // Helper: apply deadzone and normalize axis to [-1, 1]
        auto normalizeAxis = [&](int16_t raw) -> float {
            if (abs(raw) < DEADZONE) return 0.0f;
            return (float)(raw + (raw > 0 ? -DEADZONE : DEADZONE)) / (AXIS_MAX - DEADZONE);
        };

        bool worldWalk = ps4InWorldWalkMode();

        // Read the native pad once up front. SDL's PS4 backend does not surface
        // the L2/R2 analog triggers (its GameController axes stay 0), so the
        // digital L2/R2 bits from scePad are the only reliable source. The same
        // buttons drive the remapping block below.
        uint32_t padButtons = 0;
        bool havePadButtons = ps4PadButtons(&padButtons);

        // "Combat aim" engages while L2 is held during the player's combat turn.
        bool l2Held = havePadButtons && (padButtons & ORBIS_PAD_BUTTON_L2) != 0;
        bool r2Held = havePadButtons && (padButtons & ORBIS_PAD_BUTTON_R2) != 0;
        bool l1Held = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;

        // While the called-shot (VATS) body-part window is up, hand full control
        // back to the modal: aim assist must NOT hijack the cursor or the Cross
        // button, otherwise the window can't be pointed at or its Cancel clicked.
        // The modal runs its own input loop and re-enters this code, so this guard
        // is what breaks the fight over the cursor. The window stays open until a
        // body part is picked (Cross) or it is cancelled (Cancel button / Circle).
        bool calledShotActive = combatGamepadCalledShotActive();
        // Hold L2 for aim assist. In combat it snaps onto enemies (combat_auto_aim);
        // out of combat, in the explorable world, it snaps onto people to talk to /
        // interact with (world_auto_aim). Both are gated by their config toggle.
        // L2 + L1 = loot/interact assist: snap onto containers / corpses / doors,
        // and Cross loots / opens (or applies an active Skilldex skill, e.g.
        // Lockpick). Works BOTH in and out of combat, and takes priority over the
        // L2-only aim modes while L1 is held.
        bool lootAim = l2Held && l1Held && g_ps4Controls.lootAssist && !calledShotActive
            && (worldWalk || (isInCombat() && GameMode::isInGameMode(GameMode::kPlayerTurn)));
        bool combatAim = l2Held && g_ps4Controls.combatAutoAim && isInCombat()
            && GameMode::isInGameMode(GameMode::kPlayerTurn)
            && !calledShotActive && !lootAim;
        bool worldAim = l2Held && g_ps4Controls.worldAutoAim && worldWalk && !isInCombat() && !lootAim;
        bool aimActive = combatAim || worldAim || lootAim;
        // While the hold-Cross action menu (Talk/Look/Use/...) modal is up, the pad
        // drives its navigation with the D-pad; aim, arrows and cursor-mode forcing
        // all pause so they don't fight the modal.
        bool actionMenu = gGameMouseActionMenuActive;
        // Loot assist with a Skilldex skill active (Lockpick, Steal, ...): Cross must
        // apply the skill at the cursor, so keep the plain positional click for that.
        // Otherwise Cross acts on the SELECTED object directly (below).
        bool lootSkillMode = lootAim && gameMouseGetMode() >= FIRST_GAME_MOUSE_MODE_SKILL;

        // Circle acts as a universal "close/back": whenever an in-game menu is open
        // (pipboy, inventory, char sheet, automap, skilldex, the Esc options menu,
        // save/load, loot/barter, ...) Circle sends Escape to close it, instead of
        // its usual RMB. Every such menu sets a GameMode flag via ScopedGameMode and
        // closes on Escape; the plain map view (only kCombat/kPlayerTurn, or none)
        // keeps Circle as RMB so it can still cycle the cursor. The VATS window is a
        // raw modal that sets no GameMode flag, so it's folded in via calledShotActive.
        int currentMode = GameMode::getCurrentGameMode();
        bool menuOpen = gGameLoaded
            && (currentMode & ~(GameMode::kCombat | GameMode::kPlayerTurn)) != 0;
        // Inventory / barter / loot are cursor-driven: right-click there cycles the
        // cursor to the "examine" mode to inspect items, so Circle must stay RMB, not
        // Escape. Detect by the PRESENCE of one of those bits — barter is a sub-screen
        // of dialog (kDialog|kBarter), so masking them out isn't enough; their bit has
        // to WIN over the other bits. circle_close_all=1 forgoes this (veterans who
        // want one-button close-everything). Close those windows with Options/Done.
        int kCursorCycleModes = GameMode::kInventory | GameMode::kBarter | GameMode::kLoot;
        bool inCursorWindow = (currentMode & kCursorCycleModes) != 0;
        bool circleCloses = calledShotActive
            || (menuOpen && !(inCursorWindow && g_ps4Controls.circleCloseAll == 0));

        // --- 0. Button mapping (remappable via ps4_controls.cfg) ---
        // Done first so the cursor-boost result is available to the cursor below,
        // and the LMB/RMB results to the mouse-state block at the end of the frame.
        g_padLmb = false;
        g_padRmb = false;
        g_padBoost = false;

        static const struct {
            SDL_GameControllerButton button;
            int* action;
        } kFaceButtons[] = {
            { SDL_CONTROLLER_BUTTON_A, &g_ps4Controls.actCross },
            { SDL_CONTROLLER_BUTTON_B, &g_ps4Controls.actCircle },
            { SDL_CONTROLLER_BUTTON_X, &g_ps4Controls.actSquare },
            { SDL_CONTROLLER_BUTTON_Y, &g_ps4Controls.actTriangle },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, &g_ps4Controls.actL1 },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, &g_ps4Controls.actR1 },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, &g_ps4Controls.actL3 },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, &g_ps4Controls.actR3 },
            // NOTE: SDL_CONTROLLER_BUTTON_BACK is NOT the Share button on this
            // backend — it reports the physical Options button, which we already
            // read via scePad below. Mapping it here made one Options press fire
            // two actions at once, so it is intentionally omitted. The Share button
            // itself is OS-reserved on PS4 and not readable; actShare lives on the
            // touchpad click instead (handled after the Options block).
        };
        for (const auto& fb : kFaceButtons) {
            // During combat aim, Cross is the fire button (handled below), so skip
            // its normal action here (and leave its prev-state to the aim section).
            if ((combatAim || (lootAim && !lootSkillMode)) && !actionMenu
                && fb.button == SDL_CONTROLLER_BUTTON_A) {
                continue;
            }
            // While L2 is held, L1 is the loot-assist modifier (L2+L1), not its
            // mapped action (inventory) — consume it so it doesn't also fire.
            if (g_ps4Controls.lootAssist && l2Held && fb.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                g_prevButtons[fb.button] = SDL_GameControllerGetButton(g_gamepad, fb.button) != 0;
                continue;
            }
            bool held = SDL_GameControllerGetButton(g_gamepad, fb.button) != 0;
            bool prev = g_prevButtons[fb.button];
            // Circle closes any open menu / the VATS window (sends Escape) instead
            // of acting as RMB, giving every menu a one-button close on the pad.
            if (circleCloses && fb.button == SDL_CONTROLLER_BUTTON_B) {
                if (held && !prev) {
                    enqueueInputEvent(KEY_ESCAPE);
                }
                g_prevButtons[fb.button] = held;
                continue;
            }
            ps4ApplyButton(*fb.action, held, &prev);
            g_prevButtons[fb.button] = prev;
        }

        // Options is read via native scePad (its OPTIONS bit is distinct from the
        // touchpad CLICK, which the SDL-PS4 backend otherwise reports as START).
        {
            bool held = havePadButtons
                ? (padButtons & ORBIS_PAD_BUTTON_OPTIONS) != 0
                : SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_START) != 0;
            bool prev = g_prevButtons[SDL_CONTROLLER_BUTTON_START];
            ps4ApplyButton(g_ps4Controls.actOptions, held, &prev);
            g_prevButtons[SDL_CONTROLLER_BUTTON_START] = prev;
        }

        // DS4 touchpad physical click. The Share button is OS-reserved on PS4, so
        // actShare (default pipboy) lives here instead. The click bit is distinct
        // from a finger tap, which the touchpad-cursor block turns into a left
        // click, so normal pointing never triggers this.
        {
            static bool s_prevTouchpadClick = false;
            bool held = havePadButtons && (padButtons & ORBIS_PAD_BUTTON_TOUCH_PAD) != 0;
            ps4ApplyButton(g_ps4Controls.actShare, held, &s_prevTouchpadClick);
        }

        // Hold-Cross action menu: D-pad up/down moves the cursor vertically so the
        // modal (which highlights on mouse-Y deltas > 10px) steps prev/next; release
        // Cross to pick. Reuses the arrow repeat cadence.
        if (actionMenu) {
            bool dpUp   = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)   != 0;
            bool dpDown = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
            if (dpUp || dpDown) {
                if ((int)(currentTicks - g_lastDpadTicks) > g_ps4Controls.dpadRepeatMs) {
                    const float step = 14.0f; // clears the modal's 10px threshold
                    int lw = screenGetWidth();
                    int lh = screenGetHeight();
                    g_virtualMouseY += dpUp ? -step : step;
                    if (g_virtualMouseY < 0.0f) g_virtualMouseY = 0.0f;
                    if (g_virtualMouseY > (float)(lh - 1)) g_virtualMouseY = (float)(lh - 1);
                    if (gSdlWindow != nullptr) {
                        int winW, winH;
                        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
                        SDL_WarpMouseInWindow(gSdlWindow,
                            (int)(g_virtualMouseX * (float)winW / (float)lw),
                            (int)(g_virtualMouseY * (float)winH / (float)lh));
                    }
                    g_lastDpadTicks = currentTicks;
                }
            } else {
                g_lastDpadTicks = 0;
            }
        }
        // D-Pad -> arrow keys (with repeat). Suppressed during aim assist, where
        // left/right cycle the target instead (handled in the aim section).
        else if (!aimActive) {
            bool dpUp    = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP)    != 0;
            bool dpDown  = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  != 0;
            bool dpLeft  = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  != 0;
            bool dpRight = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
            if (dpUp || dpDown || dpLeft || dpRight) {
                if ((int)(currentTicks - g_lastDpadTicks) > g_ps4Controls.dpadRepeatMs) {
                    if (dpUp)    enqueueInputEvent(KEY_ARROW_UP);
                    if (dpDown)  enqueueInputEvent(KEY_ARROW_DOWN);
                    if (dpLeft)  enqueueInputEvent(KEY_ARROW_LEFT);
                    if (dpRight) enqueueInputEvent(KEY_ARROW_RIGHT);
                    g_lastDpadTicks = currentTicks;
                }
            } else {
                g_lastDpadTicks = 0;
            }
        }

        // --- 0b. Aim assist (hold L2) ---
        // In combat: snap onto the nearest enemy (crosshair cursor), D-pad L/R
        // cycles, Cross fires, Cross+R2 forces an aimed (called) shot. Out of
        // combat, in the world: snap onto the nearest person to talk to/interact
        // with (normal action cursor), D-pad cycles, and Cross stays a plain LMB
        // click (handled by the face-button loop) so it opens the interaction.
        // Target list (living, in-range, in-LOS, on-screen unless whole-map) comes
        // from combat.cc and works both in and out of combat.
        // The target set is FROZEN on engage (the L2 press): we snapshot whatever
        // is reachable/in-LOS in the viewport (or whole map, per config) at that
        // moment. While L2 stays held, D-pad only cycles within that frozen set —
        // panning the camera (center-camera below) never adds new targets. Dead or
        // removed targets fall out. The selection is tracked by object pointer, so
        // it never jumps when the list is pruned. Releasing and re-pressing L2
        // re-snapshots.
        static Object* s_aimList[16];
        static int s_aimCount = 0;
        static Object* s_aimSelected = nullptr;
        static bool s_aimEngaged = false;
        // Game-mouse mode forced while aiming (saved/restored). Combat uses the
        // CROSSHAIR (with the % to-hit box); out of combat the ARROW/action mode,
        // which makes the engine show the talk/look/use cursor and inspect the
        // pointed critter — the "interaction" cursor. -1 = not aiming, restore.
        static int s_savedMouseMode = -1;
        // Target we last printed the AP-cost line for, so combat aim prints it once
        // per target (on change) instead of every frame. Cleared when combat aim is
        // off, so re-engaging L2 reprints.
        static Object* s_apMsgTarget = nullptr;
        int desiredMouseMode;
        if (actionMenu) {
            desiredMouseMode = -1; // don't touch the cursor the modal manages
        } else if (combatAim) {
            desiredMouseMode = GAME_MOUSE_MODE_CROSSHAIR;
        } else if (lootAim) {
            // Keep an active Skilldex skill cursor (Lockpick, Steal, ...) so Cross
            // applies the skill to the snapped target; otherwise the action (arrow)
            // cursor, so Cross loots / opens.
            desiredMouseMode = (gameMouseGetMode() >= FIRST_GAME_MOUSE_MODE_SKILL)
                ? -1 : GAME_MOUSE_MODE_ARROW;
        } else if (worldAim) {
            desiredMouseMode = GAME_MOUSE_MODE_ARROW;
        } else {
            desiredMouseMode = -1;
        }

        // Restore the saved mode whenever we're not forcing an aim cursor (L2
        // release / not in an aimable context). Skipped while the action menu is up
        // so we don't reset the cursor it manages.
        if (!actionMenu && desiredMouseMode < 0 && s_savedMouseMode >= 0) {
            gameMouseSetMode(s_savedMouseMode);
            s_savedMouseMode = -1;
        }
        if (!combatAim) {
            s_apMsgTarget = nullptr;
        }

        // Loot-assist and target-aim draw from different candidate sets, so toggling
        // L1 (while L2 is held) must re-freeze — otherwise a stale combat target
        // would be pruned against the interactables list. Pick the source per mode.
        static bool s_aimWasLoot = false;
        if (lootAim != s_aimWasLoot) {
            s_aimEngaged = false;
        }
        s_aimWasLoot = lootAim;
        auto buildAimList = [&](Object** buf, int max, bool whole) -> int {
            return lootAim
                ? gamepadListInteractables(buf, max, whole,
                      g_ps4Controls.lootSnapDistance, g_ps4Controls.lootSkipEmpty != 0)
                : combatGamepadListTargets(buf, max, whole);
        };

        if (aimActive && !actionMenu) {
            if (!s_aimEngaged) {
                // Engage: freeze the current viewport (or whole-map) target set.
                s_aimCount = buildAimList(s_aimList, 16, g_ps4Controls.autoAimWholeMap != 0);
                s_aimSelected = (s_aimCount > 0) ? s_aimList[0] : nullptr;
                s_aimEngaged = (s_aimCount > 0);
            } else {
                // Prune the frozen set to targets still valid (alive, in range, in
                // LOS) WITHOUT adding new ones. Validate by address against the live
                // whole-map list so panning doesn't drop off-screen frozen targets
                // and a stale pointer is never dereferenced.
                Object* live[64];
                int nLive = buildAimList(live, 64, true);
                int w = 0;
                for (int i = 0; i < s_aimCount; i++) {
                    for (int j = 0; j < nLive; j++) {
                        if (live[j] == s_aimList[i]) {
                            s_aimList[w++] = s_aimList[i];
                            break;
                        }
                    }
                }
                s_aimCount = w;
                if (s_aimCount == 0) {
                    s_aimEngaged = false;
                }
            }

            if (s_aimEngaged && s_aimCount > 0) {
                // Track the selection by object; default to nearest if it's gone.
                int idx = 0;
                for (int i = 0; i < s_aimCount; i++) {
                    if (s_aimList[i] == s_aimSelected) {
                        idx = i;
                        break;
                    }
                }

                // D-pad L/R cycles within the frozen set (edge-detected).
                bool dl = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
                bool dr = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
                if (dr && !g_prevButtons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT]) {
                    idx = (idx + 1) % s_aimCount;
                }
                if (dl && !g_prevButtons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]) {
                    idx = (idx + s_aimCount - 1) % s_aimCount;
                }
                g_prevButtons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = dr;
                g_prevButtons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = dl;
                s_aimSelected = s_aimList[idx];
                Object* target = s_aimSelected;

                // Combat only: on each target change, print the AP cost to reach +
                // attack this target to the message monitor (bottom-left), so a melee
                // "run in and hit" is an informed choice, not a blind dash. Approach
                // is estimated as the hex distance beyond the weapon's reach and only
                // for short-range (melee/unarmed) weapons — ranged weapons don't
                // auto-approach. It ignores obstacles, so it's a lower bound.
                if (combatAim && target != s_apMsgTarget) {
                    int hitMode;
                    bool aiming;
                    if (interfaceGetCurrentHitMode(&hitMode, &aiming) != -1) {
                        int attackAP = itemGetActionPointCost(gDude, hitMode, aiming);
                        int range = weaponGetRange(gDude, hitMode);
                        int dist = objectGetDistanceBetween(gDude, target);
                        int haveAP = gDude->data.critter.combat.ap;
                        char buf[96];
                        if (range <= 2 && dist > range) {
                            // Out-of-range melee: the engine reticle shows no % (its
                            // to-hit test fails the range check), so surface the
                            // projected chance AS IF adjacent — the same range-free
                            // figure the enemy AI uses to decide to charge in.
                            int approachAP = dist - range;
                            unsigned char rotations[800];
                            int hitChance = _determine_to_hit_no_range(gDude, target,
                                HIT_LOCATION_UNCALLED, hitMode, rotations);
                            snprintf(buf, sizeof(buf),
                                "Reach %d + attack %d = %d AP (have %d), hit ~%d%%",
                                approachAP, attackAP, approachAP + attackAP, haveAP, hitChance);
                        } else {
                            snprintf(buf, sizeof(buf), "Attack: %d AP (have %d)", attackAP, haveAP);
                        }
                        displayMonitorAddMessage(buf);
                    }
                    s_apMsgTarget = target;
                }

                // Force the context aim cursor (save the prior mode once): combat
                // crosshair with the % to-hit box, or the out-of-combat action mode
                // that shows the talk/look/use cursor over the pointed person.
                if (desiredMouseMode >= 0) {
                    if (s_savedMouseMode < 0) {
                        s_savedMouseMode = gameMouseGetMode();
                    }
                    if (gameMouseGetMode() != desiredMouseMode) {
                        gameMouseSetMode(desiredMouseMode);
                    }
                }

                Rect tr;
                objectGetRect(target, &tr);

                // Optional: pan the camera to centre the target (dead-zone follow,
                // like the centered stick mode). Re-fetch the rect afterwards so
                // the cursor lands on the target's new on-screen position.
                if (g_ps4Controls.autoAimCenterCamera && (tr.right > tr.left || tr.bottom > tr.top)) {
                    int tcx = (tr.left + tr.right) / 2;
                    int tcy = (tr.top + tr.bottom) / 2;
                    int offx = tcx - screenGetWidth() / 2;
                    int offy = tcy - screenGetVisibleHeight() / 2;
                    int sdx = (offx > g_ps4Controls.cameraDeadzoneX) ? 1 : (offx < -g_ps4Controls.cameraDeadzoneX ? -1 : 0);
                    int sdy = (offy > g_ps4Controls.cameraDeadzoneY) ? 1 : (offy < -g_ps4Controls.cameraDeadzoneY ? -1 : 0);
                    if (sdx != 0 || sdy != 0) {
                        mapScroll(sdx, sdy);
                        objectGetRect(target, &tr);
                    }
                }

                // Snap the cursor onto the CENTRE of the target's sprite (not the
                // tile floor at its feet). Aiming at the feet sits below the
                // sprite, so the engine's mouse-over hit test never fires (no % box
                // in combat, no action cursor out of combat). objectGetRect gives
                // the sprite's on-screen bounds; its midpoint lands on the body.
                if (tr.right > tr.left || tr.bottom > tr.top) {
                    g_virtualMouseX = (float)((tr.left + tr.right) / 2);
                    g_virtualMouseY = (float)((tr.top + tr.bottom) / 2);
                    int lw = screenGetWidth();
                    int lh = screenGetHeight();
                    if (g_virtualMouseX < 0.0f) g_virtualMouseX = 0.0f;
                    if (g_virtualMouseX > (float)(lw - 1)) g_virtualMouseX = (float)(lw - 1);
                    if (g_virtualMouseY < 0.0f) g_virtualMouseY = 0.0f;
                    if (g_virtualMouseY > (float)(lh - 1)) g_virtualMouseY = (float)(lh - 1);
                    if (gSdlWindow != nullptr) {
                        int winW, winH;
                        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
                        SDL_WarpMouseInWindow(gSdlWindow,
                            (int)(g_virtualMouseX * (float)winW / (float)lw),
                            (int)(g_virtualMouseY * (float)winH / (float)lh));
                    }
                }

                // Combat only: Cross = fire; with R2 also held = aimed (called)
                // shot. Out of combat Cross is left to the face-button loop as a
                // normal LMB click, which opens the interaction on the target.
                if (combatAim) {
                    bool fire = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_A) != 0;
                    if (fire && !g_prevButtons[SDL_CONTROLLER_BUTTON_A]) {
                        if (r2Held) {
                            combatGamepadAimedAttack(target);
                        } else {
                            // Melee/unarmed (reach <= 2) out of range: run up and
                            // then swing, mirroring the enemy AI. The engine's
                            // _combat_attack_this alone just says "out of range".
                            // melee_approach config: 0 off, 1 run-up only, 2 run+attack.
                            int hm;
                            bool aim;
                            int range = (interfaceGetCurrentHitMode(&hm, &aim) != -1)
                                ? weaponGetRange(gDude, hm) : 0;
                            int dist = objectGetDistanceBetween(gDude, target);
                            if (range <= 2 && dist > range
                                && g_ps4Controls.meleeApproach != PS4_MELEE_OFF) {
                                combatGamepadApproachAndAttack(target,
                                    g_ps4Controls.meleeApproach == PS4_MELEE_ATTACK);
                            } else {
                                _combat_attack_this(target);
                            }
                        }
                    }
                    g_prevButtons[SDL_CONTROLLER_BUTTON_A] = fire;
                }
                else if (lootAim && !lootSkillMode && target != nullptr) {
                    // Loot assist: act on the SELECTED object directly (walk up +
                    // open/loot/door), not a positional click — so a container behind
                    // clutter (e.g. a merchant's "Stuff" that overlaps a table) is
                    // used instead of whatever the cursor overlaps. Dispatch per type
                    // exactly as the engine's own ARROW-mode click does
                    // (game_mouse.cc): objectUse only handles scenery, so items need
                    // actionPickUp (which opens containers + runs their script/barter)
                    // and corpses need actionLootCritter.
                    bool fire = SDL_GameControllerGetButton(g_gamepad, SDL_CONTROLLER_BUTTON_A) != 0;
                    if (fire && !g_prevButtons[SDL_CONTROLLER_BUTTON_A]) {
                        switch (FID_TYPE(target->fid)) {
                        case OBJ_TYPE_ITEM:
                            actionPickUp(gDude, target);
                            break;
                        case OBJ_TYPE_CRITTER:
                            actionLootCritter(gDude, target);
                            break;
                        case OBJ_TYPE_SCENERY:
                            _action_use_an_object(gDude, target);
                            break;
                        }
                    }
                    g_prevButtons[SDL_CONTROLLER_BUTTON_A] = fire;
                }
            }
        } else {
            s_aimEngaged = false;
        }

        // --- Sticks in the map view -------------------------------------------
        // Each stick plays a configurable role (world_left_stick /
        // world_right_stick): walk, centered (pan with the cursor pinned to the
        // screen centre, freed at the map edge), camera (plain pan), cursor (free
        // MacBook-like pointer), off. Default world scheme: LEFT = centered,
        // RIGHT = free cursor. Combat keeps left = cursor / right = camera pan;
        // menus keep left = cursor / right = mouse-wheel. WALK is handled in
        // ps4GamepadWorldMove(); aim assist (L2) owns the cursor and idles both.
        float lx = normalizeAxis(SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTX));
        float ly = normalizeAxis(SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_LEFTY));
        float rx = normalizeAxis(SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTX));
        float ry = normalizeAxis(SDL_GameControllerGetAxis(g_gamepad, SDL_CONTROLLER_AXIS_RIGHTY));

        auto warpVirtualCursor = [&]() {
            int lw = screenGetWidth();
            int lh = screenGetHeight();
            if (g_virtualMouseX < 0.0f) g_virtualMouseX = 0.0f;
            if (g_virtualMouseX > (float)(lw - 1)) g_virtualMouseX = (float)(lw - 1);
            if (g_virtualMouseY < 0.0f) g_virtualMouseY = 0.0f;
            if (g_virtualMouseY > (float)(lh - 1)) g_virtualMouseY = (float)(lh - 1);
            if (gSdlWindow != nullptr) {
                int winW, winH;
                SDL_GetWindowSize(gSdlWindow, &winW, &winH);
                SDL_WarpMouseInWindow(gSdlWindow,
                    (int)(g_virtualMouseX * (float)winW / (float)lw),
                    (int)(g_virtualMouseY * (float)winH / (float)lh));
            }
        };

        // Free MacBook-like pointer from a stick (with the boost button).
        auto stickFreeCursor = [&](float ax, float ay) {
            if (ax == 0.0f && ay == 0.0f) return;
            float speed = g_padBoost ? g_ps4Controls.cursorSpeedBoost : g_ps4Controls.cursorSpeed;
            g_virtualMouseX += ax * speed * dt;
            g_virtualMouseY += ay * speed * dt;
            warpVirtualCursor();
        };

        // Plain camera pan via the engine edge-scroll (self-throttled).
        auto stickCameraPan = [&](float ax, float ay) {
            float pt = g_ps4Controls.cameraPanThreshold;
            int sdx = (ax > pt) ? 1 : (ax < -pt ? -1 : 0);
            int sdy = (ay > pt) ? 1 : (ay < -pt ? -1 : 0);
            if (sdx != 0 || sdy != 0) {
                mapScroll(sdx, sdy);
            }
        };

        // Centered-cursor camera: pan with the cursor pinned to screen centre;
        // at the map edge (can't scroll) free the cursor toward the border.
        auto stickCenteredCamera = [&](float ax, float ay) {
            float mag = sqrtf(ax * ax + ay * ay);
            if (mag < 0.30f) return;
            float pt = g_ps4Controls.cameraPanThreshold;
            int sdx = (ax > pt) ? 1 : (ax < -pt ? -1 : 0);
            int sdy = (ay > pt) ? 1 : (ay < -pt ? -1 : 0);
            int before = gCenterTile;
            int rc = -1;
            if (sdx != 0 || sdy != 0) {
                rc = mapScroll(sdx, sdy);
            }
            bool panned = (gCenterTile != before);
            if (panned) {
                g_virtualMouseX = (float)(screenGetWidth() / 2);
                g_virtualMouseY = (float)(screenGetVisibleHeight() / 2);
            } else if (rc != -2) {
                g_virtualMouseX += ax * g_ps4Controls.cursorSpeed * dt;
                g_virtualMouseY += ay * g_ps4Controls.cursorSpeed * dt;
            }
            // rc == -2 (throttled): hold the cursor; the camera pans next tick.
            if (panned || rc != -2) {
                warpVirtualCursor();
            }
        };

        auto applyStickRole = [&](int role, float ax, float ay) {
            switch (role) {
            case PS4_WRS_CURSOR: stickFreeCursor(ax, ay); break;
            case PS4_WRS_CAMERA: stickCameraPan(ax, ay); break;
            case PS4_WRS_CENTERED: stickCenteredCamera(ax, ay); break;
            // PS4_WRS_WALK handled in ps4GamepadWorldMove(); PS4_WRS_OFF = nothing.
            default: break;
            }
        };

        bool mapView = ps4InMapView();
        if (aimActive) {
            // Aim assist owns the cursor; leave both sticks idle this frame.
        } else if (worldWalk) {
            // World exploration: each stick plays its configured role.
            applyStickRole(g_ps4Controls.worldLeftStick, lx, ly);
            applyStickRole(g_ps4Controls.worldRightStick, rx, ry);
        } else if (mapView) {
            // Combat: left stick = cursor, right stick = camera pan.
            stickFreeCursor(lx, ly);
            stickCameraPan(rx, ry);
        } else {
            // Menus / non-map views: left stick = cursor, right stick = wheel.
            stickFreeCursor(lx, ly);
            if (rx != 0.0f || ry != 0.0f) {
                static float scrollAccX = 0.0f, scrollAccY = 0.0f;
                scrollAccX += rx * 8.0f * dt; // ~8 scroll-ticks/sec at full deflection
                scrollAccY += ry * 8.0f * dt;
                int ticksX = (int)scrollAccX;
                int ticksY = (int)scrollAccY;
                if (ticksX != 0 || ticksY != 0) {
                    *wheelX += ticksX;
                    *wheelY += ticksY;
                    scrollAccX -= (float)ticksX;
                    scrollAccY -= (float)ticksY;
                }
            }
        }

        // --- 3. DS4 Touchpad cursor (native scePad) ---
        // The SDL touchpad API crashes on this backend, so read the finger via
        // scePad (OrbisPadData.touch). A finger acts as a trackpad: relative
        // movement drives the cursor, and a quick tap is a left click. While a
        // finger is down, world-walk is suppressed (g_touchpadFingerActive) so you
        // can point without walking; move the left stick again to resume walking.
        // This is the world-mode pointer and removes the cursor-stick swap.
        // Suppressed while aim assist owns the cursor (reticle snap).
        if (!aimActive) {
            static bool s_prevFinger = false;
            static uint16_t s_prevX = 0, s_prevY = 0;
            static uint32_t s_tapStart = 0;
            static int s_tapMoveAccum = 0;
            static float s_smoothDx = 0.0f, s_smoothDy = 0.0f;

            bool fingerDown = false;
            OrbisPadData pd;
            if (g_padHandle >= 0 && scePadReadState(g_padHandle, &pd) == 0 && pd.touch.fingers > 0) {
                fingerDown = true;
                uint16_t fx = pd.touch.touch[0].x;
                uint16_t fy = pd.touch.touch[0].y;
                if (s_prevFinger) {
                    // Relative trackpad move. DS4 touchpad is ~1920x943 units.
                    // All gains are user-tunable via /data/fallout2/ps4_controls.cfg.
                    int ddx = (int)fx - (int)s_prevX;
                    int ddy = (int)fy - (int)s_prevY;

                    // Jitter filter: the touchpad reports a wobbling position even
                    // when a finger rests still, which would drift the cursor.
                    // Ignore sub-threshold movement (baseline still follows below).
                    if ((float)(abs(ddx) + abs(ddy)) < g_ps4Controls.touchDeadzone) {
                        ddx = 0;
                        ddy = 0;
                    }
                    s_tapMoveAccum += abs(ddx) + abs(ddy);

                    // Low-pass smoothing to tame jitter/steppiness (EMA).
                    float sm = g_ps4Controls.touchSmoothing;
                    s_smoothDx = s_smoothDx * sm + (float)ddx * (1.0f - sm);
                    s_smoothDy = s_smoothDy * sm + (float)ddy * (1.0f - sm);

                    // Acceleration curve (trackpad feel): slow finger = precise
                    // (base gain), fast finger = fast. gain grows with finger speed.
                    float speed = sqrtf(s_smoothDx * s_smoothDx + s_smoothDy * s_smoothDy);
                    float gain = g_ps4Controls.touchSensitivity + g_ps4Controls.touchAccel * speed;
                    if (gain > g_ps4Controls.touchMaxGain) {
                        gain = g_ps4Controls.touchMaxGain;
                    }

                    g_virtualMouseX += s_smoothDx * gain;
                    g_virtualMouseY += s_smoothDy * gain;

                    int logicalW = screenGetWidth();
                    int logicalH = screenGetHeight();
                    if (g_virtualMouseX < 0.0f) g_virtualMouseX = 0.0f;
                    if (g_virtualMouseX > (float)(logicalW - 1)) g_virtualMouseX = (float)(logicalW - 1);
                    if (g_virtualMouseY < 0.0f) g_virtualMouseY = 0.0f;
                    if (g_virtualMouseY > (float)(logicalH - 1)) g_virtualMouseY = (float)(logicalH - 1);

                    if (gSdlWindow != nullptr) {
                        int winW, winH;
                        SDL_GetWindowSize(gSdlWindow, &winW, &winH);
                        SDL_WarpMouseInWindow(gSdlWindow,
                            (int)(g_virtualMouseX * (float)winW / (float)logicalW),
                            (int)(g_virtualMouseY * (float)winH / (float)logicalH));
                    }
                } else {
                    // Finger just landed — start tap tracking, clear smoothing.
                    s_tapStart = currentTicks;
                    s_tapMoveAccum = 0;
                    s_smoothDx = 0.0f;
                    s_smoothDy = 0.0f;
                }
                s_prevX = fx;
                s_prevY = fy;
            }

            // On release: a short, low-movement touch is a tap -> left click. Hold
            // the synthetic click for a few polls so the engine sees a down->up.
            if (s_prevFinger && !fingerDown) {
                if ((int)(currentTicks - s_tapStart) < g_ps4Controls.touchTapMs && s_tapMoveAccum < g_ps4Controls.touchTapMove) {
                    g_touchpadClickFrames = 3;
                }
            }

            s_prevFinger = fingerDown;
            g_touchpadFingerActive = fingerDown;
        }
    }
}

void ps4GamepadFillMouse(MouseData* mouseState)
{
        mouseState->x = (int)g_virtualMouseX;
        mouseState->y = (int)g_virtualMouseY;
        // LMB = a button mapped to lmb (default Cross) OR a held touchpad-tap
        // click (counted down over a few polls so the engine sees press->release).
        mouseState->buttons[0] = g_padLmb || (g_touchpadClickFrames > 0);
        if (g_touchpadClickFrames > 0) {
            g_touchpadClickFrames--;
        }
        mouseState->buttons[1] = g_padRmb;
}

void ps4GamepadOpen()
{
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; i++) {
        if (SDL_IsGameController(i)) {
            g_gamepad = SDL_GameControllerOpen(i);
            if (g_gamepad) {
                break;
            }
        }
    }

    // Grab the native scePad handle for the pad SDL just opened, so we can read
    // the true button bitmask (OPTIONS vs TOUCH_PAD). scePadGetHandle returns the
    // EXISTING handle — no second open, no conflict with SDL. scePadInit is
    // idempotent (SDL already called it).
    if (g_gamepad != nullptr) {
        scePadInit();
        int32_t userId = -1;
        if (sceUserServiceGetInitialUser(&userId) == 0) {
            g_padHandle = scePadGetHandle(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        }
        ps4Log("[ps4] scePadGetHandle(user=%d) -> %d\n", userId, g_padHandle);
    }
}

void ps4GamepadClose()
{
    // Don't scePadClose g_padHandle — SDL owns the pad; we only borrowed the
    // handle via scePadGetHandle. Just drop our reference.
    g_padHandle = -1;
    if (g_gamepad != nullptr) {
        SDL_GameControllerClose(g_gamepad);
        g_gamepad = nullptr;
    }
}

} // namespace fallout

#endif // __PS4__
