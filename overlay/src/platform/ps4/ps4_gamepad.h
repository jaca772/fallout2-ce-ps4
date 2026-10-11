#ifndef FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_
#define FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_

#ifdef __PS4__

#include "dinput.h" // MouseData

namespace fallout {

struct Object;

enum Ps4GamepadActionType {
    PS4_PAD_ACTION_NONE = 0,
    PS4_PAD_ACTION_ATTACK,
    PS4_PAD_ACTION_AIMED_ATTACK,
    PS4_PAD_ACTION_MELEE_APPROACH,
    PS4_PAD_ACTION_LOOT_INTERACT,
    PS4_PAD_ACTION_RELOAD,
};

struct Ps4GamepadPendingAction {
    Ps4GamepadActionType type;
    Object* target;
    int targetId;
    int targetPid;
    int targetElevation;
    int param;
};

// PS4 DS4 gamepad -> mouse/keys layer. All the controls logic lives in
// ps4_gamepad.cc; dinput.cc only calls these hooks under __PS4__.

// Open the pad (SDL controller + native scePad handle). Call from mouseDeviceInit.
void ps4GamepadOpen();

// Close the pad. Call from mouseDeviceFree.
void ps4GamepadClose();

// True while a pad is open (drives the mouse-override branch in mouseDeviceGetData).
bool ps4GamepadIsOpen();

// Per-frame: read the pad and drive the virtual cursor / buttons / camera / aim
// assist. Accumulates menu mouse-wheel scroll into *wheelX / *wheelY. Call where
// the old inline gamepad block sat in mouseDeviceGetData.
void ps4GamepadPoll(int* wheelX, int* wheelY);

// Write the virtual cursor position + button state into mouseState, overriding
// the SDL mouse read. Call only when ps4GamepadIsOpen().
void ps4GamepadFillMouse(MouseData* mouseState);

// Drive the player with the left stick (continuous march) when world_left_stick
// is set to walk. Called once per frame from mainLoop().
void ps4GamepadWorldMove();

// Queue a gamepad action for deferred execution outside of input polling.
bool ps4GamepadQueueAction(Ps4GamepadActionType type, Object* target, int param);

// Execute any pending gamepad actions. Called from safe points in mainLoop()
// and _combat_input().
void ps4GamepadProcessPendingActions();

// Clear the pending action queue (e.g. on map/mode change).
void ps4GamepadClearPendingActions();

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_
