#ifndef FALLOUT_PLATFORM_PS4_PS4_IME_H_
#define FALLOUT_PLATFORM_PS4_PS4_IME_H_

#ifdef __PS4__

#include <stddef.h>

namespace fallout {

enum Ps4ImeFilterMode {
    PS4_IME_FILTER_TEXT = 0,         // Character name, save description: ASCII 32..122
    PS4_IME_FILTER_DOS_FILENAME = 1, // DOS file name: alnum + _isdoschar punctuation
};

enum Ps4ImeResult {
    PS4_IME_RESULT_CONFIRMED = 0, // Confirmed (Done / Enter)
    PS4_IME_RESULT_CANCELED = 1,  // Canceled (Circle / Cancel)
    PS4_IME_RESULT_SKIPPED = 2,   // IME disabled or initialization failed
};

// Open the native PS4 onscreen keyboard dialog (sceImeDialog). Blocking call.
// Frames continue presenting via ps4VideoPresent while the dialog is active.
Ps4ImeResult ps4ImeEdit(const char* title, const char* initial, int maxLen,
                        Ps4ImeFilterMode filter, char* out, size_t outSize);

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_IME_H_
