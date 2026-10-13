#ifdef __PS4__

#include "platform/ps4/ps4_ime.h"
#include "platform/ps4/ps4.h"
#include "platform/ps4/ps4_video.h"
#include "platform/ps4/ps4_gamepad.h"
#include "svga.h"

#include <orbis/libkernel.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>

#include <SDL.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace fallout {

namespace {

static bool s_imeModuleLoaded = false;

static bool ps4ImeEnsureInitialized()
{
    if (!s_imeModuleLoaded) {
        int rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
        ps4Log("[ps4] sceSysmoduleLoadModule(IME_DIALOG) -> 0x%08x\n", rc);
        rc = sceCommonDialogInitialize();
        ps4Log("[ps4] sceCommonDialogInitialize() -> 0x%08x\n", rc);
        rc = sceUserServiceInitialize(nullptr);
        ps4Log("[ps4] sceUserServiceInitialize() -> 0x%08x\n", rc);
        s_imeModuleLoaded = true;
    }
    return true;
}

static char transliterateCodepoint(uint32_t cp)
{
    if (cp >= 32 && cp <= 126) {
        return static_cast<char>(cp);
    }

    // Polish diacritics
    switch (cp) {
    case 0x0104: return 'A'; // Ą
    case 0x0105: return 'a'; // ą
    case 0x0106: return 'C'; // Ć
    case 0x0107: return 'c'; // ć
    case 0x0118: return 'E'; // Ę
    case 0x0119: return 'e'; // ę
    case 0x0141: return 'L'; // Ł
    case 0x0142: return 'l'; // ł
    case 0x0143: return 'N'; // Ń
    case 0x0144: return 'n'; // ń
    case 0x00D3: return 'O'; // Ó
    case 0x00F3: return 'o'; // ó
    case 0x015A: return 'S'; // Ś
    case 0x015B: return 's'; // ś
    case 0x0179: return 'Z'; // Ź
    case 0x017A: return 'z'; // ź
    case 0x017B: return 'Z'; // Ż
    case 0x017C: return 'z'; // ż

    // Western European & Latin-1 accents
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return 'A';
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
    case 0x00C6: return 'A'; case 0x00E6: return 'a';
    case 0x00C7: return 'C'; case 0x00E7: return 'c';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';
    case 0x00D1: return 'N'; case 0x00F1: return 'n';
    case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: return 'O';
    case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: return 'o';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';
    case 0x00DD: return 'Y'; case 0x00FD: case 0x00FF: return 'y';
    case 0x00DF: return 's'; // German ß
    case 0x0160: return 'S'; case 0x0161: return 's';
    case 0x017D: return 'Z'; case 0x017E: return 'z';
    default: return 0;
    }
}

static bool isDosChar(char ch)
{
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        return true;
    }
    const char* punct = "#@!$`'~^&()-_=[]{}";
    return strchr(punct, ch) != nullptr;
}

static void utf8ToUtf16(const char* src, uint16_t* dst, size_t dstCapacity)
{
    if (!dst || dstCapacity == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t d = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    while (*p != '\0' && d + 1 < dstCapacity) {
        uint32_t cp = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) { p++; continue; }
            cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { p++; continue; }
            cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) { p++; continue; }
            cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            p += 4;
        } else {
            p++;
            continue;
        }

        if (cp < 0x10000) {
            dst[d++] = static_cast<uint16_t>(cp);
        } else if (cp <= 0x10FFFF) {
            if (d + 2 >= dstCapacity) break;
            cp -= 0x10000;
            dst[d++] = static_cast<uint16_t>(0xD800 + (cp >> 10));
            dst[d++] = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
        }
    }
    dst[d] = 0;
}

static void utf16ToFilteredAscii(const uint16_t* src, Ps4ImeFilterMode filter, char* out, size_t outCapacity, int maxLen)
{
    if (!out || outCapacity == 0) return;
    if (!src) {
        out[0] = '\0';
        return;
    }
    size_t maxTarget = std::min(static_cast<size_t>(maxLen), outCapacity - 1);
    size_t o = 0;
    size_t s = 0;
    while (src[s] != 0 && o < maxTarget) {
        uint32_t cp = src[s++];
        if (cp >= 0xD800 && cp <= 0xDBFF && src[s] >= 0xDC00 && src[s] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (src[s++] - 0xDC00);
        }

        char ch = transliterateCodepoint(cp);
        if (ch == 0) {
            continue;
        }

        if (filter == PS4_IME_FILTER_DOS_FILENAME) {
            if (ch >= 'a' && ch <= 'z') {
                ch = static_cast<char>(ch - 'a' + 'A');
            }
            if (!isDosChar(ch)) {
                continue;
            }
        } else { // PS4_IME_FILTER_TEXT
            if (ch < 32 || ch > 122) {
                continue;
            }
        }

        out[o++] = ch;
    }
    out[o] = '\0';
}

} // namespace

Ps4ImeResult ps4ImeEdit(const char* title, const char* initial, int maxLen,
                        Ps4ImeFilterMode filter, char* out, size_t outSize)
{
    if (!out || outSize == 0 || maxLen <= 0) {
        return PS4_IME_RESULT_SKIPPED;
    }

    if (!g_ps4Controls.imeEnabled) {
        return PS4_IME_RESULT_SKIPPED;
    }

    if (!ps4ImeEnsureInitialized()) {
        return PS4_IME_RESULT_SKIPPED;
    }

    int32_t userId = -1;
    int userRc = sceUserServiceGetInitialUser(&userId);
    ps4Log("[ps4] sceUserServiceGetInitialUser() -> 0x%08x, userId=0x%08x\n", userRc, userId);
    if (userRc != 0 || userId == -1) {
        userId = 0;
    }

    int32_t enterBtn = -1;
    int sysRc = sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN, &enterBtn);
    ps4Log("[ps4] enterButtonAssign param -> 0x%08x, val=%d (0=Circle, 1=Cross)\n", sysRc, enterBtn);

    const size_t bufUnits = static_cast<size_t>(maxLen) + 1;
    std::vector<uint16_t> utf16Input(bufUnits, 0);
    utf8ToUtf16(initial, utf16Input.data(), bufUnits);

    uint16_t utf16Title[128] = { 0 };
    utf8ToUtf16(title, utf16Title, sizeof(utf16Title) / sizeof(uint16_t));

    OrbisImeDialogSetting setting;
    memset(&setting, 0, sizeof(setting));
    setting.userId = static_cast<uint32_t>(userId);
    setting.type = ORBIS_TYPE_BASIC_LATIN;
    setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    setting.inputMethod = ORBIS__DEFAULT;
    setting.maxTextLength = static_cast<uint32_t>(maxLen);
    setting.inputTextBuffer = reinterpret_cast<wchar_t*>(utf16Input.data());
    setting.title = reinterpret_cast<const wchar_t*>(utf16Title);
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;

    int initRc = sceImeDialogInit(&setting, nullptr);
    if (initRc != 0) {
        ps4Log("[ps4] sceImeDialogInit failed: 0x%08x\n", initRc);
        return PS4_IME_RESULT_SKIPPED;
    }
    ps4Log("[ps4] sceImeDialog opened (title='%s', maxLen=%d)\n", title ? title : "", maxLen);

    while (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_RUNNING) {
        SDL_PumpEvents();
        if (gSdlTextureSurface != nullptr) {
            // ps4VideoPresent already blocks on VSync (sceKernelWaitEqueue), so a
            // further sleep would halve the IME refresh to ~30 FPS.
            ps4VideoPresent(gSdlTextureSurface);
        } else {
            sceKernelUsleep(16000);
        }
    }

    OrbisDialogResult dialogRes;
    memset(&dialogRes, 0, sizeof(dialogRes));
    sceImeDialogGetResult(&dialogRes);
    sceImeDialogTerm();

    // Flush any events accumulated while the system dialog had focus
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    ps4GamepadSwallowUntilRelease();

    ps4Log("[ps4] sceImeDialog closed, endstatus=%d\n", dialogRes.endstatus);
    if (dialogRes.endstatus != ORBIS_DIALOG_OK) {
        return PS4_IME_RESULT_CANCELED;
    }

    utf16ToFilteredAscii(utf16Input.data(), filter, out, outSize, maxLen);
    ps4Log("[ps4] ime result: '%s'\n", out);
    return PS4_IME_RESULT_CONFIRMED;
}

} // namespace fallout

#endif // __PS4__
