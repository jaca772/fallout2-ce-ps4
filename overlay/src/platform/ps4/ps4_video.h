#ifndef FALLOUT_PLATFORM_PS4_PS4_VIDEO_H_
#define FALLOUT_PLATFORM_PS4_PS4_VIDEO_H_

// Native sceVideoOut presentation path (optional build, PS4_NATIVE_VIDEOOUT).
//
// Replaces SDL2's GLES renderer (which pulls the Sony Piglet/Shacc modules) with a
// direct sceVideoOut flip queue. The engine keeps rendering its software frame into
// gSdlTextureSurface exactly as before; here we just upscale + present it, plus
// composite the movie overlay. Nothing in this file runs in the default build.

#ifdef PS4_NATIVE_VIDEOOUT

#include <SDL.h>

namespace fallout {

// Open sceVideoOut, allocate + register flip buffers. srcW/srcH are the engine's
// internal render resolution (what gSdlTextureSurface is). Returns false on failure.
bool ps4VideoInit(int srcW, int srcH);

// Release the flip queue + video memory.
void ps4VideoShutdown();

// Upscale `frame` (the engine's RGB888 software surface) into the current flip
// buffer, composite any staged movie overlay, and flip. vsync-paced.
void ps4VideoPresent(SDL_Surface* frame);

// Movie overlay staging (called from movie.cc in place of the SDL texture path).
// `pixels` is ARGB8888, srcW x srcH; `dst` is in engine/logical (game-res) coords.
void ps4VideoMovieOverlaySet(const void* pixels, int srcW, int srcH, SDL_Rect dst);
void ps4VideoMovieOverlayClear();

} // namespace fallout

#endif // PS4_NATIVE_VIDEOOUT

#endif // FALLOUT_PLATFORM_PS4_PS4_VIDEO_H_
