#ifndef FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_
#define FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_

#ifdef __PS4__

#include "dinput.h" // MouseData

namespace fallout {

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

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_GAMEPAD_H_
