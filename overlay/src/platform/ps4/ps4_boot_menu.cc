#include "ps4_boot_menu.h"

#ifdef __PS4__

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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
                              bool* outUp, bool* outDown, bool* outConfirm, bool* outCancel, bool* outMenu)
{
    SDL_PumpEvents();

    bool up = false;
    bool down = false;
    bool confirm = false;
    bool cancel = false;
    bool menu = false;

    if (controller != nullptr) {
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0) up = true;
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0) down = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) < -14000) up = true;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) > 14000) down = true;

        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A) != 0) confirm = true; // Cross
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START) != 0) confirm = true; // Options
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B) != 0) cancel = true; // Circle

        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0) menu = true; // L1
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) menu = true; // L2
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0) menu = true; // L3
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X) != 0) menu = true; // Square
        if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y) != 0) menu = true; // Triangle
    }

    if (padHandle >= 0) {
        OrbisPadData pd;
        if (scePadReadState(padHandle, &pd) == 0) {
            if (pd.buttons & ORBIS_PAD_BUTTON_UP) up = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_DOWN) down = true;
            if (pd.buttons & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS)) confirm = true;
            if (pd.buttons & ORBIS_PAD_BUTTON_CIRCLE) cancel = true;

            if (pd.buttons & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_L3 |
                              ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_TRIANGLE)) {
                menu = true;
            }
        }
    }

    if (outUp) *outUp = up;
    if (outDown) *outDown = down;
    if (outConfirm) *outConfirm = confirm;
    if (outCancel) *outCancel = cancel;
    if (outMenu) *outMenu = menu;
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
    drawString(surface, 60, 220, "Could not locate 'master.dat' in /data/fallout2 or any subfolder.", 0x0018ff62, 0, 2);

    drawString(surface, 60, 290, "HOW TO INSTALL GAME FILES:", 0x0033ff77, 0, 2);
    drawString(surface, 80, 330, "1. Connect to PS4 via GoldHEN FTP Server (Port 2121).", 0x0018ff62, 0, 2);
    drawString(surface, 80, 370, "2. Navigate to directory: /data/fallout2/", 0x0018ff62, 0, 2);
    drawString(surface, 80, 410, "3. Upload the required game files from your Fallout 2 copy:", 0x0018ff62, 0, 2);
    drawString(surface, 120, 450, "- master.dat", 0x000d8c36, 0, 2);
    drawString(surface, 120, 480, "- critter.dat", 0x000d8c36, 0, 2);
    drawString(surface, 120, 510, "- patch000.dat (if present)", 0x000d8c36, 0, 2);
    drawString(surface, 120, 540, "- data/ and sound/ folders", 0x000d8c36, 0, 2);
    drawString(surface, 80, 580, "4. Press [PS Button] to exit, then restart the application.", 0x0018ff62, 0, 2);

    applyScanlines(surface);
    ps4VideoPresent(surface);

    while (true) {
        SDL_Delay(100);
    }
}

// 2.5-second countdown prompt screen showing the active game with visual progress bar
static bool ps4ShowBootCountdown(const std::string& activeTitle, SDL_GameController* controller, int padHandle)
{
    ps4Log("[ps4] showing boot countdown for '%s'\n", activeTitle.c_str());
    if (!ps4VideoInit(1280, 720)) {
        return false;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGB888);
    if (surface == nullptr) {
        ps4VideoShutdown();
        return false;
    }

    bool openMenu = false;
    const int totalMs = 2500;
    int elapsedMs = 0;

    while (elapsedMs < totalMs) {
        bool up = false, down = false, confirm = false, cancel = false, menu = false;
        ps4ReadInputState(controller, padHandle, &up, &down, &confirm, &cancel, &menu);

        if (menu) {
            ps4Log("[ps4] boot countdown: L1/L2 pressed -> opening Boot Menu!\n");
            openMenu = true;
            break;
        }

        if (confirm) {
            ps4Log("[ps4] boot countdown: Cross pressed -> launching immediately\n");
            openMenu = false;
            break;
        }

        int remainingSeconds = (totalMs - elapsedMs + 999) / 1000;

        fillRect(surface, 0, 0, 1280, 720, 0x00060d07);

        // Header
        fillRect(surface, 40, 40, 1200, 68, 0x000f2312);
        drawString(surface, 56, 54, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - BOOT LOADER", 0x0018ff62, 0, 2);
        drawString(surface, 56, 82, "FALLOUT 2 COMMUNITY EDITION - PLAYSTATION 4 PORT", 0x000d8c36, 0, 1);

        // Active Game Banner
        fillRect(surface, 40, 160, 1200, 100, 0x00142818);
        drawString(surface, 60, 180, "ACTIVE GAME:", 0x0033ff77, 0, 2);
        drawString(surface, 60, 215, activeTitle.c_str(), 0x0018ff62, 0, 3);

        // Countdown display
        char cdBuf[128];
        snprintf(cdBuf, sizeof(cdBuf), "Starting in %d second%s...", remainingSeconds, remainingSeconds == 1 ? "" : "s");
        drawString(surface, 60, 310, cdBuf, 0x0018ff62, 0, 2);

        // Progress bar
        int barW = 1160;
        fillRect(surface, 60, 350, barW, 16, 0x000d2010);
        int filledW = barW * (totalMs - elapsedMs) / totalMs;
        if (filledW > 0) {
            fillRect(surface, 60, 350, filledW, 16, 0x0018ff62);
        }

        // Action instructions box
        fillRect(surface, 40, 430, 1200, 190, 0x000c1c0e);
        drawString(surface, 60, 455, "CONTROLLER ACTIONS:", 0x0033ff77, 0, 2);
        drawString(surface, 80, 500, "[L1 / L2]   OPEN BOOT MENU (Select a different game / mod)", 0x0018ff62, 0, 2);
        drawString(surface, 80, 545, "[CROSS / X] LAUNCH NOW", 0x000d8c36, 0, 2);

        // Footer divider
        fillRect(surface, 40, 650, 1200, 2, 0x0018ff62);
        drawString(surface, 60, 665, "Hint: Press L1 or L2 now to switch between Fallout 2, Sonora, Nevada, etc.", 0x000d8c36, 0, 1);

        applyScanlines(surface);
        ps4VideoPresent(surface);

        SDL_Delay(20);
        elapsedMs += 20;
    }

    SDL_FreeSurface(surface);
    ps4VideoShutdown();

    return openMenu;
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
    bool prevConfirm = false;
    bool prevCancel = false;
    uint32_t repeatTimer = 0;
    bool running = true;

    while (running) {
        bool upRaw = false, downRaw = false, confirmRaw = false, cancelRaw = false, menuRaw = false;
        ps4ReadInputState(controller, padHandle, &upRaw, &downRaw, &confirmRaw, &cancelRaw, &menuRaw);

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

        if (confirmRaw && !prevConfirm) {
            ps4Log("[ps4] boot menu confirmed: '%s' (%s)\n", entries[selectedIndex].title.c_str(), entries[selectedIndex].path.c_str());
            saveActiveGameConfig(entries[selectedIndex].path);
            running = false;
            break;
        }
        prevConfirm = confirmRaw;

        if (cancelRaw && !prevCancel) {
            ps4Log("[ps4] boot menu cancelled with Circle (using default)\n");
            selectedIndex = 0;
            running = false;
            break;
        }
        prevCancel = cancelRaw;

        fillRect(surface, 0, 0, 1280, 720, 0x00060d07);

        // Header
        fillRect(surface, 40, 30, 1200, 68, 0x000f2312);
        drawString(surface, 56, 44, "ROBCO INDUSTRIES (TM) TERMLINK PROTOCOL - BOOT SELECTOR", 0x0018ff62, 0, 2);
        drawString(surface, 56, 72, "FALLOUT 2 COMMUNITY EDITION - PLAYSTATION 4 PORT", 0x000d8c36, 0, 1);

        // Subtitle
        drawString(surface, 56, 120, "DETECTED GAME INSTALLATIONS IN /data/:", 0x0033ff77, 0, 2);

        // Game list
        int startY = 160;
        int itemH = 46;
        for (size_t i = 0; i < entries.size(); ++i) {
            int itemY = startY + (int)i * itemH;
            if (itemY + itemH > 560) break;

            bool isSelected = ((int)i == selectedIndex);
            if (isSelected) {
                fillRect(surface, 56, itemY, 1168, 38, 0x0018ff62);
                char buf[256];
                snprintf(buf, sizeof(buf), "> [%zu] %s", i + 1, entries[i].title.c_str());
                drawString(surface, 68, itemY + 11, buf, 0x00060d07, 0x0018ff62, 2);
                drawString(surface, 800, itemY + 15, entries[i].path.c_str(), 0x00060d07, 0x0018ff62, 1);
            } else {
                char buf[256];
                snprintf(buf, sizeof(buf), "  [%zu] %s", i + 1, entries[i].title.c_str());
                drawString(surface, 68, itemY + 11, buf, 0x0018ff62, 0, 2);
                drawString(surface, 800, itemY + 15, entries[i].path.c_str(), 0x000d8c36, 0, 1);
            }
        }

        // Footer divider
        fillRect(surface, 40, 580, 1200, 2, 0x0018ff62);

        // Controls bar
        drawString(surface, 56, 600, "CONTROLS:", 0x0033ff77, 0, 2);
        drawString(surface, 56, 630, "[D-PAD UP / DOWN / STICK] Select     [CROSS / X] Launch Game     [CIRCLE / O] Default", 0x0018ff62, 0, 2);
        drawString(surface, 56, 665, "TIP: Hold L1 or L2 right after launching the app to open this menu again.", 0x000d8c36, 0, 1);

        applyScanlines(surface);
        ps4VideoPresent(surface);

        SDL_Delay(16);
    }

    SDL_FreeSurface(surface);
    ps4VideoShutdown();

    return entries[selectedIndex].path;
}

} // namespace

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

bool ps4IsL1OrL2Pressed()
{
    SDL_PumpEvents();

    int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* pad = SDL_GameControllerOpen(i);
            if (pad != nullptr) {
                bool l1 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
                bool l2 = (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000)
                       || (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0);
                SDL_GameControllerClose(pad);
                if (l1 || l2) return true;
            }
        }
    }

    scePadInit();
    int32_t userId = -1;
    if (sceUserServiceGetInitialUser(&userId) == 0 && userId != -1) {
        int handle = scePadGetHandle(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        if (handle >= 0) {
            OrbisPadData pd;
            memset(&pd, 0, sizeof(pd));
            if (scePadReadState(handle, &pd) == 0) {
                if (pd.buttons & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_L3)) {
                    return true;
                }
            }
        }
    }

    return false;
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

    // RULE 1: If only 1 game folder exists, ALWAYS skip boot menu!
    if (folders.size() == 1) {
        ps4Log("[ps4] single game folder found: %s — skipping boot menu\n", folders[0].path.c_str());
        return folders[0].path;
    }

    // Initialize controller input once
    SDL_GameController* controller = ps4OpenFirstController();
    int padHandle = ps4GetPadHandle();
    ps4Log("[ps4] boot selection input ready: controller=%p, padHandle=%d\n", controller, padHandle);

    // Check if L1 or L2 is held right now at launch
    bool up = false, down = false, confirm = false, cancel = false, menuHeld = false;
    ps4ReadInputState(controller, padHandle, &up, &down, &confirm, &cancel, &menuHeld);
    ps4Log("[ps4] multiple game folders (%zu found), L1/L2 pressed at launch=%d\n", folders.size(), menuHeld);

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

    std::string chosenPath;

    if (menuHeld) {
        // L1 or L2 held at launch -> open menu immediately
        chosenPath = ps4RunBootMenu(folders, activeIndex, controller, padHandle);
    } else if (hasActiveConfig) {
        // Show 2.5s countdown with visual progress bar and quick actions
        bool wantMenu = ps4ShowBootCountdown(folders[activeIndex].title, controller, padHandle);
        if (wantMenu) {
            chosenPath = ps4RunBootMenu(folders, activeIndex, controller, padHandle);
        } else {
            chosenPath = folders[activeIndex].path;
        }
    } else {
        // First run with multiple folders -> open menu directly
        chosenPath = ps4RunBootMenu(folders, 0, controller, padHandle);
    }

    if (controller != nullptr) {
        SDL_GameControllerClose(controller);
        controller = nullptr;
    }

    return chosenPath;
}

} // namespace fallout

#endif // __PS4__
