# Fallout II Community Edition — PS4 Port

PlayStation 4 homebrew port of [fallout2-ce](https://github.com/fallout2-ce/fallout2-ce) (Fallout 2 Community Edition) built with the OpenOrbis toolchain.

- **100% Native Video:** Renders via native `sceVideoOut` flip queues — **zero proprietary Sony modules needed**.
- **Engine-only package:** No copyrighted game assets are bundled. You provide your own Fallout 2 PC data files.
- **DualShock 4 Gamepad Support:** Smooth stick movement, trackpad cursor via DS4 Touchpad, combat aim-assist (L2) and loot/door assist (L2 + L1).
- **Case-insensitive FS:** Automatically handles mixed-case filenames on PS4's case-sensitive filesystem.

---

## Installation & Setup

### 1. Requirements
- A jailbroken PS4 running **GoldHEN** (tested on 9.00; compatible with 5.05–11.00).
- Your own copy of Fallout 2 for PC (GOG, Steam, or original CD).

### 2. Copy Game Data
Transfer your Fallout 2 game files via FTP (e.g. GoldHEN FTP server on port 2121) to **`/data/fallout2/`** on the console:

```
/data/fallout2/
├── master.dat
├── critter.dat
├── patch000.dat      (if present in your install)
├── data/
├── sound/
└── fallout2.cfg      (optional: configure resolution)
```

*Note: File names and folders can be in any casing (lowercase, uppercase, mixed).*

### 3. Install the PKG
Download the latest `IV0001-FALL20002_00-FALL200020100000.pkg` from [Releases](../../releases) and install it via **GoldHEN Debug Settings → Package Installer**.

Saves are stored in `/data/fallout2/data/SAVEGAME/` and persist across PKG updates.

---

## Controls (DualShock 4)

### Default Mapping

| Input | Action |
|---|---|
| **Left Stick** | Camera pan with cursor centered (frees at map edge) |
| **Right Stick** | Free cursor pointer |
| **Touchpad (Slide / Tap)** | Trackpad cursor / Left click |
| **Touchpad (Physical Click)** | Pipboy |
| **Cross ✕** | Left click / Confirm / Attack target |
| **Circle ○** | Right click (cycle cursor mode) / Back / Cancel |
| **Square ◻** | Skilldex |
| **Triangle △** | Character Sheet |
| **L1** | Inventory |
| **R1** | Reload weapon (in combat: costs AP) |
| **L2 (Hold)** | **Aim Assist:** Snap reticle to nearest target (D-pad to cycle) |
| **L2 + L1 (Hold)** | **Loot Assist:** Snap to nearest container, corpse or door |
| **L3 (Click Left Stick)** | Swap hands (switch equipped weapon) |
| **R3 (Click Right Stick)** | Toggle combat / End turn (or exit combat if safe) |
| **Options** | Escape (Menu / Skip cutscene) |
| **D-Pad** | Arrow keys / Navigate action menus |

### Aim & Interaction Assist
- **Combat Target Snap (Hold L2):** Automatically locks the reticle onto the nearest hostile/target critter and displays hit chance and AP costs. Use **D-pad ← / →** to cycle through visible targets. Press **✕** to fire.
- **Loot / Interact Snap (Hold L2 + L1):** Snaps to nearest containers, corpses, or doors. Press **✕** to loot or open/close. If a skill (e.g. Lockpick) is active in Skilldex, pressing **✕** applies it directly to the door/container.

### Customizing Controls
All gamepad bindings and sensitivity settings can be customized in **`/data/fallout2/ps4_controls.cfg`** (created automatically on first launch). You can edit this file over FTP.

---

## Building

### GitHub Actions (Cloud Build)
1. Go to **Actions → build-ps4 → Run workflow**.
2. Leave `upstream_ref` blank (or enter `main` to track the latest upstream commit).
3. The workflow builds the PKG and publishes it automatically to [Releases](../../releases).

### Building Locally (Linux / WSL)
Requires the OpenOrbis toolchain and portlibs:
```sh
source /opt/pacbrew/ps4/openorbis/ps4vars.sh
scripts/build.sh
```
The output package will be created in the repository root as `IV0001-FALL20002_00-FALL200020100000.pkg`.

*(Optional)*: To use a custom icon, place a 512×512 PNG at `icon0.png` in the repository root before building.
