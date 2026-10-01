#ifndef FALLOUT_PLATFORM_PS4_PS4_BOOT_MENU_H_
#define FALLOUT_PLATFORM_PS4_PS4_BOOT_MENU_H_

#ifdef __PS4__

#include <string>
#include <vector>

namespace fallout {

struct Ps4GameEntry {
    std::string path;       // Full absolute path, e.g. "/data/fallout2" or "/data/FalloutSonora"
    std::string title;      // User-friendly display title, e.g. "Fallout 2 (Default)" or "Fallout Sonora"
    std::string folderName; // Folder name, e.g. "fallout2" or "FalloutSonora"
    bool isDefault;
};

// Scans console data directories for game folders containing master.dat.
std::vector<Ps4GameEntry> ps4ScanGameFolders();

// Checks whether L1 or L2 is currently pressed on the controller.
bool ps4IsL1OrL2Pressed();

// Main entry point for boot folder selection:
// - If only 1 valid game folder exists: returns it immediately (skipping menu).
// - If multiple folders exist:
//   - If L1/L2 is held or pressed during early boot: opens the RobCo Boot Menu.
//   - Otherwise: loads the active/last-saved game folder from /data/f2_active_game.txt.
// - If 0 valid folders exist: displays a missing-data error screen.
std::string ps4SelectGameFolder();

} // namespace fallout

#endif // __PS4__

#endif // FALLOUT_PLATFORM_PS4_PS4_BOOT_MENU_H_
