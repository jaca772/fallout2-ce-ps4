#ifdef PS4_NATIVE_VIDEOOUT

#include "platform/ps4/ps4_video.h"
#include "platform/ps4/ps4.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

namespace fallout {

#define PS4_FB_COUNT 2
#define PS4_FB_ALIGN 0x200000

namespace {

struct Ps4Video {
    int      video = -1;
    // Output = the registered flip-buffer resolution. This is the console's actual
    // video-out mode (720p stays 720p; 4K is capped to 1080p and the display
    // upscales). The game frame is scaled to FILL it.
    uint32_t outW = 1920;
    uint32_t outH = 1080;
    uint32_t pitch = 1920; // pixels
    int      srcW = 0;
    int      srcH = 0;
    void*    fb[PS4_FB_COUNT] = { nullptr, nullptr };
    size_t   memSize = 0;
    OrbisKernelEqueue flipQueue = 0;
    int      activeIdx = 0;
    int64_t  frameID = 0;
    bool     ready = false;

    // Fill-scale lookup tables: colLut[ox] = source column for output column ox,
    // rowLut[oy] = source row for output row oy. Nearest sampling, no per-pixel
    // division. When out == src*integer this is exactly integer nearest (crisp).
    int*     colLut = nullptr;
    int*     rowLut = nullptr;

    // Movie overlay (ARGB8888), staged from movie.cc; dst is in game/logical coords.
    uint32_t* movie = nullptr;
    int       movieCap = 0;
    int       movieW = 0;
    int       movieH = 0;
    SDL_Rect  movieDst = { 0, 0, 0, 0 };
    bool      movieActive = false;
};

Ps4Video g;

inline uint32_t toVout(uint32_t xrgb)
{
    // gSdlTextureSurface is SDL_PIXELFORMAT_RGB888 (0x00RRGGBB); sceVideoOut buffer
    // format 0x80000000 wants 0x80RRGGBB.
    return 0x80000000u | (xrgb & 0x00FFFFFFu);
}

} // namespace

bool ps4VideoInit(int srcW, int srcH)
{
    if (g.ready) {
        ps4VideoShutdown();
    }
    if (srcW <= 0 || srcH <= 0) {
        return false;
    }

    g.srcW = srcW;
    g.srcH = srcH;

    g.video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g.video < 0) {
        ps4Log("[ps4] sceVideoOutOpen FAILED 0x%08x\n", (unsigned)g.video);
        return false;
    }

    OrbisVideoOutResolutionStatus res;
    memset(&res, 0, sizeof(res));
    uint32_t reportedW = 1920, reportedH = 1080;
    if (sceVideoOutGetResolutionStatus(g.video, &res) == 0 && res.width >= 640 && res.width <= 3840) {
        reportedW = res.width;
        reportedH = res.height;
    }
    // Cap to 1080p: 720p and 1080p pass through unchanged (valid flip modes), but 4K
    // (3840x2160) is registered as 1080p and the display upscales — registering 4K
    // buffers needs the 4K privilege and otherwise fails (app exits to the menu).
    g.outW = reportedW > 1920 ? 1920 : reportedW;
    g.outH = reportedH > 1080 ? 1080 : reportedH;
    g.pitch = g.outW;
    ps4Log("[ps4] videoout out %ux%u (panel %ux%u), src %dx%d\n",
        g.outW, g.outH, reportedW, reportedH, srcW, srcH);

    // Build fill-scale LUTs (src -> out, nearest).
    g.colLut = (int*)malloc(sizeof(int) * g.outW);
    g.rowLut = (int*)malloc(sizeof(int) * g.outH);
    if (g.colLut == nullptr || g.rowLut == nullptr) {
        ps4Log("[ps4] LUT alloc FAILED\n");
        return false;
    }
    for (uint32_t ox = 0; ox < g.outW; ox++) {
        int sx = (int)((uint64_t)ox * srcW / g.outW);
        g.colLut[ox] = sx < srcW ? sx : srcW - 1;
    }
    for (uint32_t oy = 0; oy < g.outH; oy++) {
        int sy = (int)((uint64_t)oy * srcH / g.outH);
        g.rowLut[oy] = sy < srcH ? sy : srcH - 1;
    }

    if (sceKernelCreateEqueue(&g.flipQueue, "ps4 vout flip") < 0) {
        ps4Log("[ps4] CreateEqueue FAILED\n");
        return false;
    }
    sceVideoOutAddFlipEvent(g.flipQueue, g.video, 0);

    size_t fbSize = (size_t)g.pitch * g.outH * 4;
    size_t total = fbSize * PS4_FB_COUNT;
    g.memSize = (total + PS4_FB_ALIGN - 1) / PS4_FB_ALIGN * PS4_FB_ALIGN;
    off_t memOff = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), g.memSize,
            PS4_FB_ALIGN, 3 /* WC_GARLIC */, &memOff) < 0) {
        ps4Log("[ps4] AllocateDirectMemory FAILED (%zu)\n", g.memSize);
        return false;
    }
    void* base = nullptr;
    if (sceKernelMapDirectMemory(&base, g.memSize, 0x33 /* CPU_RW|GPU_RW */, 0, memOff, PS4_FB_ALIGN) < 0) {
        ps4Log("[ps4] MapDirectMemory FAILED\n");
        return false;
    }
    for (int i = 0; i < PS4_FB_COUNT; i++) {
        g.fb[i] = (void*)((uintptr_t)base + (size_t)i * fbSize);
    }

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1 /* LINEAR */, 0 /* 16:9 */,
        g.outW, g.outH, g.pitch);
    if (sceVideoOutRegisterBuffers(g.video, 0, g.fb, PS4_FB_COUNT, &attr) < 0) {
        ps4Log("[ps4] RegisterBuffers FAILED (%ux%u)\n", g.outW, g.outH);
        return false;
    }
    sceVideoOutSetFlipRate(g.video, 0 /* 60Hz */);

    g.activeIdx = 0;
    g.frameID = 0;
    g.ready = true;
    ps4Log("[ps4] ps4VideoInit OK (native sceVideoOut, no Piglet)\n");
    return true;
}

void ps4VideoShutdown()
{
    if (g.video >= 0) {
        sceVideoOutClose(g.video);
        g.video = -1;
    }
    g.memSize = 0;
    free(g.colLut); g.colLut = nullptr;
    free(g.rowLut); g.rowLut = nullptr;
    free(g.movie);  g.movie = nullptr;
    g.movieCap = 0;
    g.movieActive = false;
    g.ready = false;
}

static void ps4CompositeMovie(uint32_t* fb)
{
    if (!g.movieActive || g.movie == nullptr || g.movieW <= 0 || g.movieH <= 0
        || g.srcW <= 0 || g.srcH <= 0) {
        return;
    }
    // The engine dst rect is in game/logical (src) coords; map it to output coords.
    int dx0 = (int)((int64_t)g.movieDst.x * g.outW / g.srcW);
    int dy0 = (int)((int64_t)g.movieDst.y * g.outH / g.srcH);
    int dw = (int)((int64_t)g.movieDst.w * g.outW / g.srcW);
    int dh = (int)((int64_t)g.movieDst.h * g.outH / g.srcH);
    if (dw <= 0 || dh <= 0) return;

    for (int y = 0; y < dh; y++) {
        int py = dy0 + y;
        if (py < 0 || py >= (int)g.outH) continue;
        int srcY = y * g.movieH / dh;
        const uint32_t* srow = g.movie + (size_t)srcY * g.movieW;
        uint32_t* drow = fb + (size_t)py * g.pitch;
        for (int x = 0; x < dw; x++) {
            int px = dx0 + x;
            if (px < 0 || px >= (int)g.outW) continue;
            int srcX = x * g.movieW / dw;
            drow[px] = toVout(srow[srcX]); // movies are opaque
        }
    }
}

void ps4VideoPresent(SDL_Surface* frame)
{
    if (!g.ready || frame == nullptr || g.colLut == nullptr || g.rowLut == nullptr) {
        return;
    }

    uint32_t* fb = (uint32_t*)g.fb[g.activeIdx];
    const uint32_t* src = (const uint32_t*)frame->pixels;
    int srcPitch = frame->pitch / 4;

    for (uint32_t oy = 0; oy < g.outH; oy++) {
        const uint32_t* srow = src + (size_t)g.rowLut[oy] * srcPitch;
        uint32_t* drow = fb + (size_t)oy * g.pitch;
        const int* col = g.colLut;
        for (uint32_t ox = 0; ox < g.outW; ox++) {
            drow[ox] = toVout(srow[col[ox]]);
        }
    }

    ps4CompositeMovie(fb);

    sceVideoOutSubmitFlip(g.video, g.activeIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, g.frameID);
    OrbisKernelEvent evt;
    int cnt;
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g.video, &st);
        if (st.flipArg == g.frameID) break;
        if (sceKernelWaitEqueue(g.flipQueue, &evt, 1, &cnt, 0) != 0) break;
    }

    g.activeIdx = (g.activeIdx + 1) % PS4_FB_COUNT;
    g.frameID++;
}

void ps4VideoMovieOverlaySet(const void* pixels, int srcW, int srcH, SDL_Rect dst)
{
    if (pixels == nullptr || srcW <= 0 || srcH <= 0) {
        g.movieActive = false;
        return;
    }
    int need = srcW * srcH;
    if (need > g.movieCap) {
        uint32_t* n = (uint32_t*)realloc(g.movie, (size_t)need * 4);
        if (n == nullptr) {
            g.movieActive = false;
            return;
        }
        g.movie = n;
        g.movieCap = need;
    }
    memcpy(g.movie, pixels, (size_t)need * 4);
    g.movieW = srcW;
    g.movieH = srcH;
    g.movieDst = dst;
    g.movieActive = true;
}

void ps4VideoMovieOverlayClear()
{
    g.movieActive = false;
}

} // namespace fallout

#endif // PS4_NATIVE_VIDEOOUT
