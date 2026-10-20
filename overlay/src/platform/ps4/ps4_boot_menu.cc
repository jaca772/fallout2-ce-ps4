#include "ps4_boot_menu.h"

#ifdef __PS4__

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <SDL.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include "platform/ps4/ps4.h"
#include "platform/ps4/ps4_video.h"
#include "platform/ps4/ps4_boot_menu_font.h"

namespace fallout {

namespace {

// --- Bitmap font & drawing primitives (1280x720 RGB888) ---

static void drawChar(SDL_Surface* s, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg, int scale)
{
    if (ch >= 128) ch = '?';
    uint32_t* pixels = static_cast<uint32_t*>(s->pixels);
    int pitch = s->pitch / sizeof(uint32_t);

    for (int r = 0; r < 8; ++r) {
        unsigned char row = font8x8_basic[ch][r];
        for (int c = 0; c < 8; ++c) {
            bool bit = (row & (1 << c)) != 0;
            uint32_t color = bit ? fg : bg;
            if (color == 0 && !bit) {
                continue;
            }
            for (int sy = 0; sy < scale; ++sy) {
                int py = y + r * scale + sy;
                if (py < 0 || py >= s->h) continue;
                for (int sx = 0; sx < scale; ++sx) {
                    int px = x + c * scale + sx;
                    if (px < 0 || px >= s->w) continue;
                    pixels[py * pitch + px] = color;
                }
            }
        }
    }
}

static void drawString(SDL_Surface* s, int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale)
{
    int curX = x;
    int curY = y;
    int charW = 8 * scale;
    int charH = 8 * scale;
    while (*str != '\0') {
        if (*str == '\n') {
            curX = x;
            curY += charH + 4;
        } else {
            drawChar(s, curX, curY, (unsigned char)*str, fg, bg, scale);
            curX += charW;
        }
        str++;
    }
}

// drawString with a right-edge guard: stops drawing a line once the next character
// would cross x + maxW. Used for long game titles/paths/values so they never spill
// past the TV safe area.
static void drawStringMaxW(SDL_Surface* s, int x, int y, int maxW, const char* str, uint32_t fg, uint32_t bg, int scale)
{
    int curX = x;
    int curY = y;
    int charW = 8 * scale;
    int charH = 8 * scale;
    int limit = x + maxW;
    while (*str != '\0') {
        if (*str == '\n') {
            curX = x;
            curY += charH + 4;
        } else {
            if (curX + charW > limit) {
                break;
            }
            drawChar(s, curX, curY, (unsigned char)*str, fg, bg, scale);
            curX += charW;
        }
        str++;
    }
}

static void fillRect(SDL_Surface* s, int x, int y, int w, int h, uint32_t color)
{
    uint32_t* pixels = static_cast<uint32_t*>(s->pixels);
    int pitch = s->pitch / sizeof(uint32_t);
    for (int py = y; py < y + h && py < s->h; ++py) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < s->w; ++px) {
            if (px < 0) continue;
            pixels[py * pitch + px] = color;
        }
    }
}

static void applyScanlines(SDL_Surface* s)
{
    uint32_t* pixels = static_cast<uint32_t*>(s->pixels);
    int pitch = s->pitch / sizeof(uint32_t);
    for (int y = 0; y < s->h; y += 2) {
        uint32_t* row = pixels + y * pitch;
        for (int x = 0; x < s->w; ++x) {
            uint32_t c = row[x];
            uint32_t r = ((c >> 16) & 0xFF) * 82 / 100;
            uint32_t g = ((c >> 8) & 0xFF) * 82 / 100;
            uint32_t b = (c & 0xFF) * 82 / 100;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

// --- Controller helpers ---

static SDL_GameController* ps4OpenFirstController()
{
    SDL_PumpEvents();
    int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* pad = SDL_GameControllerOpen(i);
            if (pad != nullptr) {
                ps4Log("[ps4] opened GameController %d: %s\n", i, SDL_GameControllerName(pad));
                return pad;
            }
        }
    }
    return nullptr;
}

static int ps4GetPadHandle()
{
    scePadInit();
    int32_t userId = -1;
    if (sceUserServiceGetInitialUser(&userId) == 0 && userId != -1) {
        int handle = scePadGetHandle(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        return handle;
    }
    return -1;
}

static void ps4ReadInputState(SDL_GameController* controller, int padHandle,
                              bool* outUp, bool* outDown, bool* outLeft, bool* outRight,
                              bool* outConfirm, bool* outCancel, bool* outMenu,
                              bool* outSquare, bool* outTriangle)
{
    SDL_PumpEvents();

    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool confirm = false;
    bool cancel = false;
    bool menu = false;
    bool square = false;
    bool triangle = false;

    if (controller != nullptr) {
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0) up = true;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0) down = true;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0) left = true;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0) right = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) < -14000) up = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) > 14000) down = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) < -14000) left = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) > 14000) right = true;

        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A) != 0) confirm = true; // Cross
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START) != 0) confirm = true; // Options
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B) != 0) cancel = true; // Circle
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X) != 0) square = true; // Square
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y) != 0) triangle = true; // Triangle

        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0) menu = true; // L1
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) menu = true; // L2
    }

    if (padHandle >= 0) {
        OrbisPadData pd;
        if (scePadReadState(padHandle, &pd) == 0) {
            if (pd.buttons & ORBIS_PAD_BUTTON_UP) up = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_DOWN) down = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_LEFT) left = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_RIGHT) right = true;
            if (pd.buttons & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS)) confirm = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_CIRCLE) cancel = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_SQUARE) square = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_TRIANGLE) triangle = true;

            if (pd.buttons & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_L2)) {
                menu = true;
            }
        }
    }

    if (outUp) *outUp = up;
    if (outDown) *outDown = down;
    if (outLeft) *outLeft = left;
    if (outRight) *outRight = right;
    if (outConfirm) *outConfirm = confirm;
    if (outCancel) *outCancel = cancel;
    if (outMenu) *outMenu = menu;
    if (outSquare) *outSquare = square;
    if (outTriangle) *outTriangle = triangle;
}

// --- Folder scanner & metadata ---

static bool dirHasMasterDat(const std::string& dirPath)
{
    std::string p1 = dirPath + "/master.dat";
    if (access(p1.c_str(), F_OK) == 0) return true;
    std::string p2 = dirPath + "/MASTER.DAT";
    if (access(p2.c_str(), F_OK) == 0) return true;
    std::string p3 = dirPath + "/Master.dat";
    if (access(p3.c_str(), F_OK) == 0) return true;

    DIR* dir = opendir(dirPath.c_str());
    if (dir == nullptr) return false;
    bool found = false;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        if (SDL_strcasecmp(e->d_name, "master.dat") == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static std::string getFolderTitle(const std::string& folderPath, const std::string& folderName)
{
    std::string titleFile = folderPath + "/title.txt";
    FILE* f = fopen(titleFile.c_str(), "r");
    if (f != nullptr) {
        char buf[128];
        if (fgets(buf, sizeof(buf), f) != nullptr) {
            char* nl = strpbrk(buf, "\r\n");
            if (nl != nullptr) *nl = '\0';
            char* p = buf;
            while (*p == ' ') p++;
            if (*p != '\0') {
                fclose(f);
                return std::string(p);
            }
        }
        fclose(f);
    }

    if (SDL_strcasecmp(folderName.c_str(), "fallout2") == 0) {
        return "Fallout 2 (Default)";
    }
    if (SDL_strcasecmp(folderName.c_str(), "falloutsonora") == 0 || SDL_strcasecmp(folderName.c_str(), "sonora") == 0) {
        return "Fallout Sonora";
    }
    if (SDL_strcasecmp(folderName.c_str(), "fallout_nevada") == 0 || SDL_strcasecmp(folderName.c_str(), "nevada") == 0) {
        return "Fallout Nevada";
    }
    if (SDL_strcasecmp(folderName.c_str(), "olympus2207") == 0 || SDL_strcasecmp(folderName.c_str(), "olympus") == 0) {
        return "Olympus 2207";
    }
    if (SDL_strcasecmp(folderName.c_str(), "fallout1") == 0 || SDL_strcasecmp(folderName.c_str(), "fallout_et_tu") == 0) {
        return "Fallout 1 (et tu)";
    }
    if (SDL_strcasecmp(folderName.c_str(), "fallout2_rpu") == 0 || SDL_strcasecmp(folderName.c_str(), "rpu") == 0) {
        return "Fallout 2 (RPU Restoration Project)";
    }

    std::string title = folderName;
    for (char& c : title) {
        if (c == '_') c = ' ';
    }
    return title;
}

static std::string readActiveGameConfig()
{
    FILE* f = fopen("/data/f2_active_game.txt", "r");
    if (f == nullptr) {
        f = fopen("/user/data/f2_active_game.txt", "r");
    }
    if (f == nullptr) return "";
    char buf[256];
    std::string result = "";
    if (fgets(buf, sizeof(buf), f) != nullptr) {
        char* nl = strpbrk(buf, "\r\n");
        if (nl != nullptr) *nl = '\0';
        char* p = buf;
        while (*p == ' ') p++;
        result = p;
    }
    fclose(f);
    return result;
}

static void saveActiveGameConfig(const std::string& pathOrName)
{
    FILE* f = fopen("/data/f2_active_game.txt", "w");
    if (f != nullptr) {
        fprintf(f, "%s\n", pathOrName.c_str());
        fclose(f);
        ps4Log("[ps4] saved active game to /data/f2_active_game.txt: %s\n", pathOrName.c_str());
    }
}

static std::string ps4GetConsoleIpAddress()
{
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0 || ifap == nullptr) {
        return "";
    }
    std::string ipStr;
    for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &(sa->sin_addr), buf, sizeof(buf)) != nullptr) {
                if (strcmp(buf, "127.0.0.1") != 0 && strncmp(buf, "0.", 2) != 0) {
                    ipStr = buf;
                    break;
                }
            }
        }
    }
    freeifaddrs(ifap);
    return ipStr;
}

static void ps4ShowNoDataError()
{
    ps4Log("[ps4] showing no-data error screen\n");
    if (!ps4VideoInit(1280, 720)) {
        return;
    }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return;
    }

    fillRect(surface, 0, 0, 1280, 720, 0x00060d07);
    fillRect(surface, 40, 40, 1200, 70, 0x00142818);
    drawString(surface, 60, 60, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - ERROR 0x404", 0x00ff4444, 0, 2);

    drawString(surface, 60, 150, "CRITICAL ERROR: NO GAME DATA FOUND", 0x00ff4444, 0, 3);
    drawString(surface, 60, 220, "Could not locate 'master.dat' in /data/fallout2, /data/* or USB drives.", 0x0018ff62, 0, 2);

    drawString(surface, 60, 280, "HOW TO INSTALL GAME FILES:", 0x0033ff77, 0, 2);

    std::string consoleIp = ps4GetConsoleIpAddress();
    char ftpLine[128];
    if (!consoleIp.empty()) {
        snprintf(ftpLine, sizeof(ftpLine), "1. Connect via FTP: ftp://%s:2121 (GoldHEN FTP)", consoleIp.c_str());
    } else {
        snprintf(ftpLine, sizeof(ftpLine), "1. Connect to PS4 via GoldHEN FTP Server (Port 2121).");
    }
    drawString(surface, 80, 320, ftpLine, 0x0018ff62, 0, 2);

    drawString(surface, 80, 360, "2. Navigate to directory: /data/fallout2/ (or USB drive /mnt/usb0/)", 0x0018ff62, 0, 2);
    drawString(surface, 80, 400, "3. Upload the required game files from your Fallout 2 copy:", 0x0018ff62, 0, 2);
    drawString(surface, 120, 435, "- master.dat", 0x000d8c36, 0, 2);
    drawString(surface, 120, 465, "- critter.dat", 0x000d8c36, 0, 2);
    drawString(surface, 120, 495, "- patch000.dat (if present)", 0x000d8c36, 0, 2);
    drawString(surface, 120, 525, "- data/ and sound/ folders", 0x000d8c36, 0, 2);
    drawString(surface, 80, 570, "4. Press [CIRCLE / O] or [CROSS / X] to exit to PS4 home screen.", 0x0033ff77, 0, 2);

    applyScanlines(surface);
    ps4VideoPresent(surface);

    SDL_GameController* controller = ps4OpenFirstController();
    int padHandle = ps4GetPadHandle();

    while (true) {
        bool confirm = false, cancel = false;
        ps4ReadInputState(controller, padHandle, nullptr, nullptr, nullptr, nullptr, &confirm, &cancel, nullptr, nullptr, nullptr);
        if (confirm || cancel) {
            ps4Log("[ps4] user requested exit from no-data error screen\n");
            if (controller != nullptr) {
                SDL_GameControllerClose(controller);
            }
            SDL_FreeSurface(surface);
            ps4VideoShutdown();
            exit(0);
        }
        SDL_Delay(50);
    }
}

// 2.5-second countdown prompt screen showing the active game with visual progress
// bar. Returns 0 = launch now, 1 = open boot menu, 2 = open settings.
static int ps4ShowBootCountdown(const std::string& activeTitle, SDL_GameController* controller, int padHandle)
{
    ps4Log("[ps4] showing boot countdown for '%s'\n", activeTitle.c_str());
    if (!ps4VideoInit(1280, 720)) {
        return 0;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return 0;
    }

    int result = 0;
    const uint32_t totalMs = 2500;
    uint32_t startTicks = SDL_GetTicks();

    // Check if Confirm was already held at start (e.g. from launching in XMB)
    bool initialConfirm = false;
    {
        bool u, d, l, r, c, ca, m, sq, tr;
        ps4ReadInputState(controller, padHandle, &u, &d, &l, &r, &c, &ca, &m, &sq, &tr);
        initialConfirm = c;
    }

    while (true) {
        uint32_t now = SDL_GetTicks();
        uint32_t elapsedMs = now - startTicks;
        if (elapsedMs >= totalMs) {
            break;
        }

        bool up = false, down = false, left = false, right = false;
        bool confirm = false, cancel = false, menu = false, square = false, triangle = false;
        ps4ReadInputState(controller, padHandle, &up, &down, &left, &right,
                          &confirm, &cancel, &menu, &square, &triangle);

        if (menu) {
            ps4Log("[ps4] boot countdown: L1/L2 pressed -> opening Boot Menu!\n");
            result = 1;
            break;
        }

        if (square) {
            ps4Log("[ps4] boot countdown: Square pressed -> opening Settings!\n");
            result = 2;
            break;
        }

        if (confirm) {
            if (!initialConfirm) {
                ps4Log("[ps4] boot countdown: Cross pressed -> launching immediately\n");
                result = 0;
                break;
            }
        } else {
            initialConfirm = false;
        }

        int remainingSeconds = (int)((totalMs - elapsedMs + 999) / 1000);

        fillRect(surface, 0, 0, 1280, 720, 0x00060d07);

        // Header
        fillRect(surface, 60, 36, 1160, 68, 0x000f2312);
        drawString(surface, 72, 50, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - BOOT LOADER", 0x0018ff62, 0, 2);
        drawString(surface, 72, 76, "FALLOUT 2 COMMUNITY EDITION - PLAYSTATION 4 PORT", 0x000d8c36, 0, 1);

        // Active Game Banner
        fillRect(surface, 60, 160, 1160, 100, 0x00142818);
        drawString(surface, 72, 180, "ACTIVE GAME:", 0x0033ff77, 0, 2);
        drawStringMaxW(surface, 72, 215, 1100, activeTitle.c_str(), 0x0018ff62, 0, 3);

        // Countdown display
        char cdBuf[128];
        snprintf(cdBuf, sizeof(cdBuf), "Starting in %d second%s...", remainingSeconds, remainingSeconds == 1 ? "" : "s");
        drawString(surface, 72, 310, cdBuf, 0x0018ff62, 0, 2);

        // Progress bar
        int barW = 1160;
        fillRect(surface, 60, 350, barW, 16, 0x000d2010);
        int filledW = barW * (int)(totalMs - elapsedMs) / (int)totalMs;
        if (filledW > 0) {
            fillRect(surface, 60, 350, filledW, 16, 0x0018ff62);
        }

        // Action instructions box (3 rows)
        fillRect(surface, 60, 430, 1160, 140, 0x000c1c0e);
        drawString(surface, 72, 452, "CONTROLLER ACTIONS:", 0x0033ff77, 0, 2);
        drawString(surface, 80, 486, "[L1 / L2]    OPEN BOOT MENU (Switch Game / Mod)", 0x0018ff62, 0, 2);
        drawString(surface, 80, 516, "[SQUARE]     OPEN SETTINGS (Resolution / Controls / QoL)", 0x0018ff62, 0, 2);
        drawString(surface, 80, 546, "[CROSS / X]  LAUNCH NOW", 0x000d8c36, 0, 2);

        // Footer divider
        fillRect(surface, 60, 580, 1160, 2, 0x0018ff62);
        drawString(surface, 60, 660, "Hint: Press L1 or L2 now to switch between Fallout 2, Sonora, Nevada, etc.", 0x000d8c36, 0, 1);

        applyScanlines(surface);
        ps4VideoPresent(surface);

        SDL_Delay(16);
    }

    SDL_FreeSurface(surface);
    // Keep sceVideoOut and direct memory buffer alive to reuse across menu and game engine.

    return result;
}

// --- Resolution presets (RobCo Configuration Terminal) ---

struct Ps4ResolutionPreset {
    const char* label;
    int width;
    int height;
    int scale;
};

static const Ps4ResolutionPreset kPs4ResolutionPresets[] = {
    { "960x540 (TV Balanced)",   960, 540, 1 },
    { "854x480 (TV Large UI)",   854, 480, 1 },
    { "1280x720 (HD 2x Big UI)", 1280, 720, 2 },
    { "1066x600 (TV Medium)",    1066, 600, 1 },
    { "1280x720 (HD 1x Small)",  1280, 720, 1 },
    { "640x480 (4:3 Classic)",   640, 480, 1 },
};
static const int kPs4ResolutionPresetCount = (int)(sizeof(kPs4ResolutionPresets) / sizeof(kPs4ResolutionPresets[0]));

static int ps4ResolutionPresetIndex(const Ps4Fallout2CfgState& cfg)
{
    for (int i = 0; i < kPs4ResolutionPresetCount; ++i) {
        if (cfg.resX == kPs4ResolutionPresets[i].width
            && cfg.resY == kPs4ResolutionPresets[i].height
            && cfg.scale == kPs4ResolutionPresets[i].scale) {
            return i;
        }
    }
    return -1;
}

// First option index that edits a button binding (see s_buttonBindings in ps4.cc).
static const int kPs4ButtonOptionBase = 16;

static const char* ps4SettingsOptionName(int opt)
{
    switch (opt) {
    case 0: return "Resolution Preset";
    case 1: return "Display Scaling";
    case 2: return "Left Stick (World)";
    case 3: return "Right Stick (World)";
    case 4: return "Combat Auto-Aim (L2)";
    case 5: return "World Auto-Aim (L2)";
    case 6: return "Melee Out-of-Range";
    case 7: return "Loot Assist (L2+L1)";
    case 8: return "Cursor Speed";
    case 9: return "Touchpad Sensitivity";
    case 10: return "PS4 On-Screen Keyboard";
    case 11: return "Circle Closes Inv/Loot";
    case 12: return "[CE QoL] Auto-Open Doors";
    case 13: return "[CE QoL] Party Loot/Barter";
    case 14: return "[CE QoL] Inventory Columns";
    case 15: return "[CE QoL] Always Run + Fast Combat";
    default: {
        int index = opt - kPs4ButtonOptionBase;
        if (index >= 0 && index < ps4ButtonBindingCount()) {
            static char label[48];
            snprintf(label, sizeof(label), "Button: %s", ps4ButtonBindingLabel(index));
            return label;
        }
        return "";
    }
    }
}

static void ps4SettingsOptionValue(int opt, const Ps4Fallout2CfgState& cfg, char* buf, size_t bufSize)
{
    switch (opt) {
    case 0: {
        int idx = ps4ResolutionPresetIndex(cfg);
        if (idx < 0) {
            snprintf(buf, bufSize, "Custom (%dx%d s%d)", cfg.resX, cfg.resY, cfg.scale);
        } else {
            snprintf(buf, bufSize, "%s", kPs4ResolutionPresets[idx].label);
        }
        break;
    }
    case 1:
        snprintf(buf, bufSize, "%s",
            g_ps4Controls.scaleMode == PS4_SCALE_FILL ? "Fill 16:9"
            : g_ps4Controls.scaleMode == PS4_SCALE_INTEGER ? "Integer"
            : "Letterbox");
        break;
    case 2:
        switch (g_ps4Controls.worldLeftStick) {
        case PS4_WRS_WALK: snprintf(buf, bufSize, "Walk Character"); break;
        case PS4_WRS_CURSOR: snprintf(buf, bufSize, "Free Cursor"); break;
        case PS4_WRS_CAMERA: snprintf(buf, bufSize, "Camera Pan"); break;
        case PS4_WRS_CENTERED: snprintf(buf, bufSize, "Centered Camera"); break;
        default: snprintf(buf, bufSize, "Off"); break;
        }
        break;
    case 3:
        switch (g_ps4Controls.worldRightStick) {
        case PS4_WRS_CAMERA: snprintf(buf, bufSize, "Camera Pan"); break;
        case PS4_WRS_CENTERED: snprintf(buf, bufSize, "Centered Camera"); break;
        case PS4_WRS_CURSOR: snprintf(buf, bufSize, "Free Cursor"); break;
        default: snprintf(buf, bufSize, "Off"); break;
        }
        break;
    case 4: snprintf(buf, bufSize, "%s", g_ps4Controls.combatAutoAim ? "On" : "Off"); break;
    case 5: snprintf(buf, bufSize, "%s", g_ps4Controls.worldAutoAim ? "On" : "Off"); break;
    case 6:
        snprintf(buf, bufSize, "%s",
            g_ps4Controls.meleeApproach == PS4_MELEE_OFF ? "Off"
            : g_ps4Controls.meleeApproach == PS4_MELEE_ATTACK ? "Run Up + Attack"
            : "Run Up Only");
        break;
    case 7: snprintf(buf, bufSize, "%s", g_ps4Controls.lootAssist ? "On" : "Off"); break;
    case 8: snprintf(buf, bufSize, "%.0f", g_ps4Controls.cursorSpeed); break;
    case 9: snprintf(buf, bufSize, "%.2f", g_ps4Controls.touchSensitivity); break;
    case 10: snprintf(buf, bufSize, "%s", g_ps4Controls.imeEnabled ? "On" : "Off"); break;
    case 11: snprintf(buf, bufSize, "%s", g_ps4Controls.circleCloseAll ? "On (Close All)" : "Off (Keep RMB)"); break;
    case 12: snprintf(buf, bufSize, "%s", cfg.autoOpenDoors ? "On" : "Off"); break;
    case 13: snprintf(buf, bufSize, "%s", cfg.partyLootAndBarter ? "On" : "Off"); break;
    case 14: snprintf(buf, bufSize, "%s", cfg.inventoryColumns >= 2 ? "2 Columns" : "1 Column"); break;
    case 15: snprintf(buf, bufSize, "%s", cfg.running ? "On" : "Off"); break;
    default: {
        int index = opt - kPs4ButtonOptionBase;
        if (index >= 0 && index < ps4ButtonBindingCount()) {
            snprintf(buf, bufSize, "%s", ps4ActionLabel(ps4ButtonBindingAction(index)));
        } else {
            buf[0] = '\0';
        }
        break;
    }
    }
}

// dir = -1 (left) or +1 (right).
static void ps4SettingsOptionAdjust(int opt, int dir, Ps4Fallout2CfgState& cfg)
{
    switch (opt) {
    case 0: {
        int idx = ps4ResolutionPresetIndex(cfg);
        int next = (idx < 0) ? (dir > 0 ? 0 : kPs4ResolutionPresetCount - 1)
                             : (idx + dir + kPs4ResolutionPresetCount) % kPs4ResolutionPresetCount;
        cfg.resX = kPs4ResolutionPresets[next].width;
        cfg.resY = kPs4ResolutionPresets[next].height;
        cfg.scale = kPs4ResolutionPresets[next].scale;
        break;
    }
    case 1: {
        int v = g_ps4Controls.scaleMode + dir;
        if (v < 0) v = 2;
        if (v > 2) v = 0;
        g_ps4Controls.scaleMode = v;
        ps4VideoSetScaleMode((Ps4ScaleMode)v);
        break;
    }
    case 2: {
        static const int roles[] = { PS4_WRS_CENTERED, PS4_WRS_WALK, PS4_WRS_CURSOR, PS4_WRS_CAMERA };
        int cur = 0;
        for (int i = 0; i < 4; ++i) if (roles[i] == g_ps4Controls.worldLeftStick) cur = i;
        cur = (cur + dir + 4) % 4;
        g_ps4Controls.worldLeftStick = roles[cur];
        break;
    }
    case 3: {
        static const int roles[] = { PS4_WRS_CURSOR, PS4_WRS_CAMERA, PS4_WRS_CENTERED };
        int cur = 0;
        for (int i = 0; i < 3; ++i) if (roles[i] == g_ps4Controls.worldRightStick) cur = i;
        cur = (cur + dir + 3) % 3;
        g_ps4Controls.worldRightStick = roles[cur];
        break;
    }
    case 4: g_ps4Controls.combatAutoAim = !g_ps4Controls.combatAutoAim; break;
    case 5: g_ps4Controls.worldAutoAim = !g_ps4Controls.worldAutoAim; break;
    case 6: {
        int v = g_ps4Controls.meleeApproach + dir;
        if (v < 0) v = 2;
        if (v > 2) v = 0;
        g_ps4Controls.meleeApproach = v;
        break;
    }
    case 7: g_ps4Controls.lootAssist = !g_ps4Controls.lootAssist; break;
    case 8: {
        float v = g_ps4Controls.cursorSpeed + dir * 50.0f;
        if (v < 250.0f) v = 250.0f;
        if (v > 900.0f) v = 900.0f;
        g_ps4Controls.cursorSpeed = v;
        g_ps4Controls.cursorSpeedBoost = v * 2.0f;
        break;
    }
    case 9: {
        float v = g_ps4Controls.touchSensitivity + dir * 0.05f;
        if (v < 0.10f) v = 0.10f;
        if (v > 0.60f) v = 0.60f;
        g_ps4Controls.touchSensitivity = v;
        break;
    }
    case 10: g_ps4Controls.imeEnabled = !g_ps4Controls.imeEnabled; break;
    case 11: g_ps4Controls.circleCloseAll = !g_ps4Controls.circleCloseAll; break;
    case 12: cfg.autoOpenDoors = !cfg.autoOpenDoors; break;
    case 13: cfg.partyLootAndBarter = !cfg.partyLootAndBarter; break;
    case 14: cfg.inventoryColumns = (cfg.inventoryColumns >= 2) ? 1 : 2; break;
    case 15: cfg.running = !cfg.running; cfg.expandBarterAndAp = cfg.running; break;
    default: {
        int index = opt - kPs4ButtonOptionBase;
        if (index >= 0 && index < ps4ButtonBindingCount()) {
            ps4ButtonBindingCycle(index, dir);
        }
        break;
    }
    }
}

// RobCo Configuration Terminal: edits display, controls and CE QoL for one game.
static void ps4RunSettingsMenu(const Ps4GameEntry& entry, SDL_GameController* controller, int padHandle)
{
    ps4Log("[ps4] opening settings menu for '%s'\n", entry.title.c_str());

    // Operate on the selected game folder's configs.
    ps4SetDataPath(entry.path.c_str());
    ps4ReadControlsConfig();
    Ps4Fallout2CfgState cfgState;
    ps4ReadFallout2CfgState(&cfgState);

    if (!ps4VideoInit(1280, 720)) {
        return;
    }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return;
    }

    const int kOptionCount = kPs4ButtonOptionBase + ps4ButtonBindingCount();
    const int maxVisible = 9;
    int selected = 0;
    int scrollOffset = 0;

    bool prevUp = false, prevDown = false, prevLeft = false, prevRight = false;
    bool prevConfirm = true, prevCancel = true, prevSquare = true, prevTriangle = true;
    {
        bool u, d, l, r, c, ca, m, sq, tr;
        ps4ReadInputState(controller, padHandle, &u, &d, &l, &r, &c, &ca, &m, &sq, &tr);
        prevUp = u; prevDown = d; prevLeft = l; prevRight = r;
        prevTriangle = tr;
        // Force the action buttons to "already pressed" so a button held from the
        // previous screen (e.g. the Square that opened this menu) can't fire here.
        prevConfirm = true;
        prevCancel = true;
        prevSquare = true;
    }

    uint32_t repeatTimer = 0;
    bool running = true;
    while (running) {
        bool upRaw = false, downRaw = false, leftRaw = false, rightRaw = false;
        bool confirmRaw = false, cancelRaw = false, menuRaw = false;
        bool squareRaw = false, triangleRaw = false;
        ps4ReadInputState(controller, padHandle, &upRaw, &downRaw, &leftRaw, &rightRaw,
                          &confirmRaw, &cancelRaw, &menuRaw, &squareRaw, &triangleRaw);

        bool goUp = upRaw && !prevUp;
        bool goDown = downRaw && !prevDown;
        bool goLeft = leftRaw && !prevLeft;
        bool goRight = rightRaw && !prevRight;

        if (upRaw || downRaw || leftRaw || rightRaw) {
            repeatTimer++;
            if (repeatTimer > 18 && (repeatTimer % 6 == 0)) {
                if (upRaw) goUp = true;
                if (downRaw) goDown = true;
                if (leftRaw) goLeft = true;
                if (rightRaw) goRight = true;
            }
        } else {
            repeatTimer = 0;
        }

        prevUp = upRaw; prevDown = downRaw; prevLeft = leftRaw; prevRight = rightRaw;

        if (goUp) selected = (selected - 1 + kOptionCount) % kOptionCount;
        if (goDown) selected = (selected + 1) % kOptionCount;
        if (goLeft) ps4SettingsOptionAdjust(selected, -1, cfgState);
        if (goRight) ps4SettingsOptionAdjust(selected, +1, cfgState);

        if (selected < scrollOffset) {
            scrollOffset = selected;
        } else if (selected >= scrollOffset + maxVisible) {
            scrollOffset = selected - maxVisible + 1;
        }

        if (confirmRaw && !prevConfirm) {
            // Cross in the settings menu acts like Right (change value +1) and never
            // closes the menu.
            ps4SettingsOptionAdjust(selected, +1, cfgState);
        }
        prevConfirm = confirmRaw;

        if (triangleRaw && !prevTriangle) {
            g_ps4Controls = Ps4ControlsConfig();
            cfgState = Ps4Fallout2CfgState();
            ps4WriteFallout2CfgState(cfgState, true);
            ps4VideoSetScaleMode((Ps4ScaleMode)g_ps4Controls.scaleMode);
            ps4Log("[ps4] settings reset to recommended PS4 defaults\n");
        }
        prevTriangle = triangleRaw;

        if (cancelRaw && !prevCancel) {
            // Circle = Save & Back: persist the current settings (fallout2.cfg
            // carries [controls] too) and return to the boot menu (never discards,
            // never launches a game).
            ps4WriteFallout2CfgState(cfgState, false);
            sync();
            ps4Log("[ps4] settings saved (Circle) for %s\n", entry.path.c_str());
            running = false;
            break;
        }
        prevCancel = cancelRaw;

        fillRect(surface, 0, 0, 1280, 720, 0x00060d07);

        // Header
        fillRect(surface, 60, 36, 1160, 68, 0x000f2312);
        drawString(surface, 72, 50, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - CONFIG TERMINAL", 0x0018ff62, 0, 2);
        char settingsFor[256];
        snprintf(settingsFor, sizeof(settingsFor), "SETTINGS FOR: %s", entry.title.c_str());
        drawStringMaxW(surface, 72, 76, 1100, settingsFor, 0x000d8c36, 0, 1);

        // Option list
        int startY = 150;
        int itemH = 44;
        for (int v = 0; v < maxVisible; ++v) {
            int i = scrollOffset + v;
            if (i >= kOptionCount) break;
            int itemY = startY + v * itemH;
            bool isSel = (i == selected);

            char val[256];
            ps4SettingsOptionValue(i, cfgState, val, sizeof(val));
            char valText[280];
            snprintf(valText, sizeof(valText), "< %s >", val);

            if (isSel) {
                fillRect(surface, 60, itemY, 1160, 38, 0x0018ff62);
                drawStringMaxW(surface, 72, itemY + 11, 500, ps4SettingsOptionName(i), 0x00060d07, 0x0018ff62, 2);
                drawStringMaxW(surface, 640, itemY + 11, 560, valText, 0x00060d07, 0x0018ff62, 2);
            } else {
                drawStringMaxW(surface, 72, itemY + 11, 500, ps4SettingsOptionName(i), 0x0018ff62, 0, 2);
                drawStringMaxW(surface, 640, itemY + 11, 560, valText, 0x000d8c36, 0, 2);
            }
        }

        if (scrollOffset > 0) {
            drawString(surface, 1080, 120, "[^ MORE]", 0x000d8c36, 0, 1);
        }
        if (scrollOffset + maxVisible < kOptionCount) {
            drawString(surface, 1080, 560, "[v MORE]", 0x000d8c36, 0, 1);
        }

        // Footer
        fillRect(surface, 60, 580, 1160, 2, 0x0018ff62);
        drawString(surface, 60, 596, "[U/D] Select   [L/R] Change Value   [CIRCLE / O] Save & Back", 0x0018ff62, 0, 2);
        drawString(surface, 60, 626, "[TRIANGLE] Reset All to Recommended PS4 & CE QoL Defaults", 0x0018ff62, 0, 2);

        applyScanlines(surface);
        ps4VideoPresent(surface);
        SDL_Delay(16);
    }

    SDL_FreeSurface(surface);
}

// Full interactive RobCo terminal boot menu
static std::string ps4RunBootMenu(const std::vector<Ps4GameEntry>& entries, int initialIndex,
                                  SDL_GameController* controller, int padHandle)
{
    ps4Log("[ps4] opening RobCo Boot Menu (%zu entries)\n", entries.size());
    if (!ps4VideoInit(1280, 720)) {
        ps4Log("[ps4] ps4VideoInit failed in ps4RunBootMenu\n");
        return entries.empty() ? "" : entries[0].path;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return entries.empty() ? "" : entries[0].path;
    }

    int selectedIndex = initialIndex;
    if (selectedIndex < 0 || selectedIndex >= (int)entries.size()) {
        selectedIndex = 0;
    }

    bool prevUp = false;
    bool prevDown = false;
    bool prevConfirm = true; // initialize true to avoid auto-triggering on enter
    bool prevCancel = true;
    bool prevSquare = true;
    {
        bool u, d, l, r, c, ca, m, sq, tr;
        ps4ReadInputState(controller, padHandle, &u, &d, &l, &r, &c, &ca, &m, &sq, &tr);
        prevUp = u;
        prevDown = d;
        prevConfirm = c;
        prevCancel = ca;
        prevSquare = sq;
    }
    uint32_t repeatTimer = 0;
    bool running = true;
    const int maxVisible = 8;
    int scrollOffset = 0;

    while (running) {
        bool upRaw = false, downRaw = false, leftRaw = false, rightRaw = false;
        bool confirmRaw = false, cancelRaw = false, menuRaw = false;
        bool squareRaw = false, triangleRaw = false;
        ps4ReadInputState(controller, padHandle, &upRaw, &downRaw, &leftRaw, &rightRaw,
                          &confirmRaw, &cancelRaw, &menuRaw, &squareRaw, &triangleRaw);

        bool goUp = upRaw && !prevUp;
        bool goDown = downRaw && !prevDown;

        if (upRaw || downRaw) {
            repeatTimer++;
            if (repeatTimer > 18 && (repeatTimer % 6 == 0)) {
                if (upRaw) goUp = true;
                if (downRaw) goDown = true;
            }
        } else {
            repeatTimer = 0;
        }

        prevUp = upRaw;
        prevDown = downRaw;

        if (goUp) {
            selectedIndex = (selectedIndex - 1 + (int)entries.size()) % (int)entries.size();
            ps4Log("[ps4] boot menu: selected -> %d (%s)\n", selectedIndex, entries[selectedIndex].title.c_str());
        }
        if (goDown) {
            selectedIndex = (selectedIndex + 1) % (int)entries.size();
            ps4Log("[ps4] boot menu: selected -> %d (%s)\n", selectedIndex, entries[selectedIndex].title.c_str());
        }

        if (selectedIndex < scrollOffset) {
            scrollOffset = selectedIndex;
        } else if (selectedIndex >= scrollOffset + maxVisible) {
            scrollOffset = selectedIndex - maxVisible + 1;
        }

        if (squareRaw && !prevSquare) {
            ps4Log("[ps4] boot menu: Square -> settings for '%s'\n", entries[selectedIndex].title.c_str());
            ps4RunSettingsMenu(entries[selectedIndex], controller, padHandle);

            // Re-sync the edge-trigger state after the settings menu returns, so a
            // button held while leaving it is NOT seen as a fresh press here (e.g.
            // Cross launching the game immediately). Action buttons are forced to
            // "already pressed".
            bool su, sd, sl, sr, sc, sca, sm, ssq, str;
            ps4ReadInputState(controller, padHandle, &su, &sd, &sl, &sr, &sc, &sca, &sm, &ssq, &str);
            prevUp = su;
            prevDown = sd;
            prevConfirm = true;
            prevCancel = true;
            prevSquare = true;
        }
        prevSquare = squareRaw;

        if (confirmRaw && !prevConfirm) {
            ps4Log("[ps4] boot menu confirmed: '%s' (%s)\n", entries[selectedIndex].title.c_str(), entries[selectedIndex].path.c_str());
            saveActiveGameConfig(entries[selectedIndex].path);
            running = false;
            break;
        }
        prevConfirm = confirmRaw;

        fillRect(surface, 0, 0, 1280, 720, 0x00060d07);

        // Header
        fillRect(surface, 60, 36, 1160, 68, 0x000f2312);
        drawString(surface, 72, 50, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - BOOT SELECTOR", 0x0018ff62, 0, 2);
        drawString(surface, 72, 76, "FALLOUT 2 COMMUNITY EDITION - PLAYSTATION 4 PORT", 0x000d8c36, 0, 1);

        // Subtitle with count and scroll status
        char subTitle[128];
        if ((int)entries.size() > maxVisible) {
            snprintf(subTitle, sizeof(subTitle), "DETECTED GAMES (%zu total, showing %d-%d):",
                     entries.size(), scrollOffset + 1, std::min((int)entries.size(), scrollOffset + maxVisible));
        } else {
            snprintf(subTitle, sizeof(subTitle), "DETECTED GAME INSTALLATIONS IN /data/:");
        }
        drawString(surface, 72, 120, subTitle, 0x0033ff77, 0, 2);

        if (scrollOffset > 0) {
            drawString(surface, 1080, 120, "[^ MORE]", 0x000d8c36, 0, 1);
        }
        if (scrollOffset + maxVisible < (int)entries.size()) {
            drawString(surface, 1080, 560, "[v MORE]", 0x000d8c36, 0, 1);
        }

        // Game list (windowed scroll)
        int startY = 160;
        int itemH = 46;
        for (int v = 0; v < maxVisible && (scrollOffset + v) < (int)entries.size(); ++v) {
            int i = scrollOffset + v;
            int itemY = startY + v * itemH;

            bool isSelected = (i == selectedIndex);
            if (isSelected) {
                fillRect(surface, 60, itemY, 1160, 38, 0x0018ff62);
                char buf[256];
                snprintf(buf, sizeof(buf), "> [%d] %s", i + 1, entries[i].title.c_str());
                drawStringMaxW(surface, 72, itemY + 11, 620, buf, 0x00060d07, 0x0018ff62, 2);
                drawStringMaxW(surface, 720, itemY + 15, 470, entries[i].path.c_str(), 0x00060d07, 0x0018ff62, 1);
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "  [%d] %s", i + 1, entries[i].title.c_str());
                drawStringMaxW(surface, 72, itemY + 11, 620, buf, 0x0018ff62, 0, 2);
                drawStringMaxW(surface, 720, itemY + 15, 470, entries[i].path.c_str(), 0x000d8c36, 0, 1);
            }
        }

        // Footer divider + controls bar (fits TV safe area)
        fillRect(surface, 60, 580, 1160, 2, 0x0018ff62);
        drawString(surface, 60, 596, "[D-PAD] Select    [CROSS] Launch    [SQUARE] Settings", 0x0018ff62, 0, 2);
        drawString(surface, 60, 660, "TIP: Hold L1 or L2 when launching the app to open this Boot Menu at any time.", 0x000d8c36, 0, 1);

        applyScanlines(surface);
        ps4VideoPresent(surface);

        SDL_Delay(16);
    }

    SDL_FreeSurface(surface);
    // Keep sceVideoOut open; engine's ps4VideoInit will resize LUTs in-place without reallocation.

    return entries[selectedIndex].path;
}

} // namespace

void ps4ShowJailbreakError()
{
    ps4Log("[ps4] showing jailbreak error screen\n");
    if (!ps4VideoInit(1280, 720)) {
        return;
    }
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return;
    }

    fillRect(surface, 0, 0, 1280, 720, 0x00060d07);
    fillRect(surface, 40, 40, 1200, 70, 0x00142818);
    drawString(surface, 60, 60, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - ERROR 0x01", 0x00ff4444, 0, 2);

    drawString(surface, 60, 150, "CRITICAL ERROR: JAILBREAK FAILED", 0x00ff4444, 0, 3);
    drawString(surface, 60, 220, "Could not escape application sandbox (libjbc set_cred failed).", 0x0018ff62, 0, 2);

    drawString(surface, 60, 290, "POSSIBLE CAUSES:", 0x0033ff77, 0, 2);
    drawString(surface, 80, 330, "1. GoldHEN is not active or kernel rw failed.", 0x0018ff62, 0, 2);
    drawString(surface, 80, 370, "2. Firmware version is unsupported.", 0x0018ff62, 0, 2);
    drawString(surface, 80, 430, "Please verify GoldHEN status in system settings and restart.", 0x0018ff62, 0, 2);
    drawString(surface, 80, 500, "Press [PS Button] to return to the home screen.", 0x0018ff62, 0, 2);

    applyScanlines(surface);
    ps4VideoPresent(surface);

    while (true) {
        SDL_Delay(100);
    }
}

std::vector<Ps4GameEntry> ps4ScanGameFolders()
{
    std::vector<Ps4GameEntry> results;
    static const char* const kRoots[] = {
        "/data",
        "/mnt/usb0",
        "/mnt/usb1"
    };

    for (size_t r = 0; r < sizeof(kRoots) / sizeof(kRoots[0]); ++r) {
        const char* root = kRoots[r];
        DIR* dir = opendir(root);
        if (dir == nullptr) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            std::string fullPath = std::string(root) + "/" + entry->d_name;
            struct stat st;
            if (stat(fullPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                continue;
            }

            if (dirHasMasterDat(fullPath)) {
                bool exists = false;
                for (const auto& existing : results) {
                    if (existing.path == fullPath || SDL_strcasecmp(existing.folderName.c_str(), entry->d_name) == 0) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    Ps4GameEntry ge;
                    ge.path = fullPath;
                    ge.folderName = entry->d_name;
                    ge.title = getFolderTitle(fullPath, entry->d_name);
                    ge.isDefault = (SDL_strcasecmp(entry->d_name, "fallout2") == 0);
                    results.push_back(ge);
                    ps4Log("[ps4] detected game folder: %s (%s)\n", fullPath.c_str(), ge.title.c_str());
                }
            }
        }
        closedir(dir);
    }

    // Fallback: check /user/data ONLY if /data was completely empty
    if (results.empty()) {
        DIR* dir = opendir("/user/data");
        if (dir != nullptr) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                std::string fullPath = std::string("/user/data/") + entry->d_name;
                struct stat st;
                if (stat(fullPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (dirHasMasterDat(fullPath)) {
                    Ps4GameEntry ge;
                    ge.path = fullPath;
                    ge.folderName = entry->d_name;
                    ge.title = getFolderTitle(fullPath, entry->d_name);
                    ge.isDefault = (SDL_strcasecmp(entry->d_name, "fallout2") == 0);
                    results.push_back(ge);
                    ps4Log("[ps4] detected fallback game folder: %s (%s)\n", fullPath.c_str(), ge.title.c_str());
                }
            }
            closedir(dir);
        }
    }

    std::sort(results.begin(), results.end(), [](const Ps4GameEntry& a, const Ps4GameEntry& b) {
        if (a.isDefault != b.isDefault) {
            return a.isDefault;
        }
        return a.title < b.title;
    });

    return results;
}

std::string ps4SelectGameFolder()
{
    ps4Log("[ps4] scanning console for Fallout game folders...\n");
    std::vector<Ps4GameEntry> folders = ps4ScanGameFolders();

    if (folders.empty()) {
        ps4Log("[ps4] no game folders found containing master.dat!\n");
        ps4ShowNoDataError();
        return "";
    }

    // Always initialize controller input and read L1/L2 held at launch.
    SDL_GameController* controller = ps4OpenFirstController();
    int padHandle = ps4GetPadHandle();
    ps4Log("[ps4] boot selection input ready: controller=%p, padHandle=%d\n", controller, padHandle);

    bool up = false, down = false, left = false, right = false;
    bool confirm = false, cancel = false, menuHeld = false, square = false, triangle = false;
    ps4ReadInputState(controller, padHandle, &up, &down, &left, &right, &confirm, &cancel, &menuHeld, &square, &triangle);
    ps4Log("[ps4] %zu game folder(s) found, L1/L2 pressed at launch=%d\n", folders.size(), menuHeld);

    std::string activeConfig = readActiveGameConfig();
    int activeIndex = 0;
    bool hasActiveConfig = false;
    if (!activeConfig.empty()) {
        for (size_t i = 0; i < folders.size(); ++i) {
            if (folders[i].path == activeConfig || folders[i].folderName == activeConfig) {
                activeIndex = (int)i;
                hasActiveConfig = true;
                break;
            }
        }
    }

    // First boot = no active-game config, or the target folder's fallout2.cfg has
    // no [controls] section yet (checked before ps4EnsureGameConfigsInitialized
    // seeds the missing pieces below).
    bool hasControlsCfg = ps4HasControlsConfig(folders[activeIndex].path.c_str());
    bool isFirstBoot = (!hasActiveConfig || !hasControlsCfg);

    // Ensure every detected folder has fallout2.cfg with a [controls] section.
    for (const Ps4GameEntry& e : folders) {
        ps4EnsureGameConfigsInitialized(e.path.c_str());
    }

    std::string chosenPath;

    if (isFirstBoot || menuHeld) {
        // First boot or L1/L2 held: always open the full boot menu (no timer),
        // even when only a single game is installed.
        chosenPath = ps4RunBootMenu(folders, activeIndex, controller, padHandle);
    } else if (folders.size() == 1) {
        // Subsequent boots with one game: launch immediately (0s).
        chosenPath = folders[0].path;
    } else {
        // Subsequent boots with multiple games: 2.5s countdown with quick actions.
        int cd = ps4ShowBootCountdown(folders[activeIndex].title, controller, padHandle);
        if (cd == 1) {
            chosenPath = ps4RunBootMenu(folders, activeIndex, controller, padHandle);
        } else if (cd == 2) {
            // Square during countdown: open settings, then land in the boot menu.
            // Both menus debounce their prev* state on entry (settings forces the
            // action buttons, boot menu re-reads), so the held Square — or whatever
            // button was used to leave settings — can't auto-launch a game.
            ps4RunSettingsMenu(folders[activeIndex], controller, padHandle);
            chosenPath = ps4RunBootMenu(folders, activeIndex, controller, padHandle);
        } else {
            chosenPath = folders[activeIndex].path;
        }
    }

    if (controller != nullptr) {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }

    return chosenPath;
}

} // namespace fallout

#endif // __PS4__
