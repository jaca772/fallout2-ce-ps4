# DualShock 4 Controller Configuration Guide

This port features a highly customizable gamepad subsystem designed specifically for PlayStation 4. All settings are loaded from **`/data/fallout2/ps4_controls.cfg`** on startup.

The configuration file is **automatically generated on first launch** with commented defaults. You can edit this file over FTP (e.g. GoldHEN FTP server on port 2121) without rebuilding the package. To restore default settings at any time, simply delete `ps4_controls.cfg` and relaunch the game.

---

## 1. Remapping Buttons

Assign any of the supported actions to the button keys.

### Available Actions

| Action | Description |
|---|---|
| `lmb` | Left mouse click / Confirm / Attack |
| `rmb` | Right mouse click (cycles cursor mode) |
| `esc` | Escape (Game menu / Cancel / Skip cutscenes) |
| `inventory` | Open inventory (`i`) |
| `charsheet` | Open character sheet (`c`) |
| `skilldex` | Open Skilldex (`s`) |
| `pipboy` | Open Pipboy (`p`) |
| `reload` | Reload equipped weapon (spends AP in combat) |
| `weaponmode` | Cycle weapon attack mode (single / aimed / burst / reload) |
| `swaphands` | Switch to the secondary equipped weapon |
| `combat` | Initiate combat / End turn (or end combat if safe) |
| `endcombat` | Force-leave combat |
| `boost` | Hold to accelerate stick cursor speed |
| `none` | Unbound (no action) |

### Button Configuration Keys

```ini
btn_cross    = lmb          btn_l1      = inventory
btn_circle   = rmb          btn_r1      = reload
btn_square   = skilldex     btn_l3      = swaphands
btn_triangle = charsheet    btn_r3      = combat
btn_options  = esc          btn_share   = pipboy
```

> **Note on `btn_share`:**  
> The physical Share button on the DualShock 4 is reserved by the PlayStation 4 operating system and cannot be intercepted by homebrew. In this configuration, **`btn_share` controls the physical click (press down) of the Touchpad**. By default, clicking the touchpad opens the Pipboy (`btn_share = pipboy`). Finger gestures and taps on the touchpad surface remain mapped to cursor motion and left-click independently.

> **Note on D-pad and Triggers:**  
> The D-pad directional buttons and L2/R2 triggers have dedicated combat/movement logic and are not remappable.

---

## 2. Analog Stick Roles

You can configure the behavior of each analog stick independently using `world_left_stick` and `world_right_stick`:

| Value | Behavior |
|---|---|
| `centered` | Pan the camera with the cursor locked to screen center (frees at map edges) *(Default Left)* |
| `cursor` | Free trackpad-style cursor pointer *(Default Right)* |
| `camera` | Pan the view while leaving the cursor position stationary |
| `walk` | Direct character movement (walk or run depending on deflection; Left stick only) |
| `off` | Disabled |

```ini
world_left_stick  = centered
world_right_stick = cursor
```

---

## 3. Aim & Interaction Assist

```ini
combat_auto_aim        = 1    # 1 = Snap onto nearest hostile in combat via L2 (0 = off)
world_auto_aim         = 1    # 1 = Snap onto NPCs/interactables out of combat (0 = off)
auto_aim_whole_map     = 0    # 0 = Only targets on-screen, 1 = Search entire map
auto_aim_center_camera = 0    # 1 = Pan camera to keep aimed target centered
melee_approach         = 1    # Behavior when pressing ✕ on out-of-reach melee/unarmed target:
                              #   0 = Disabled (engine reports "out of range")
                              #   1 = Run up to the target without swinging (Default)
                              #   2 = Run up and attack immediately
loot_assist            = 1    # 1 = Enable L2 + L1 snapping to containers/corpses/doors
loot_snap_distance     = 20   # Search radius for loot assist (in hex tiles)
loot_skip_empty        = 1    # 1 = Skip empty containers/corpses when cycling targets
circle_close_all       = 0    # Circle behavior in inventory / barter / loot screens:
                              #   0 = Acts as right-click (inspect item; close via Options/Esc or Done)
                              #   1 = Closes the current screen immediately
```

---

## 4. Cursor & Touchpad Sensitivity

Fine-tune the responsiveness of both the stick cursor and the DualShock 4 touchpad:

```ini
cursor_speed        = 450     # Stick cursor speed in pixels/sec
cursor_speed_boost  = 900     # Stick cursor speed when holding the 'boost' button
touch_sensitivity   = 0.25    # Base touchpad sensitivity when moving slowly (lower = more precise)
touch_accel         = 0.001   # Acceleration gain when moving finger quickly (0 = linear)
touch_smoothing     = 0.40    # Smoothing factor (0.0 to 0.9 — higher = smoother, slight latency)
touch_deadzone      = 2.0     # Deadzone threshold to eliminate cursor drift from resting fingers
touch_max_gain      = 4.0     # Maximum multiplier for acceleration
touch_tap_ms        = 200     # Maximum touch duration (in ms) registered as a tap click
touch_tap_move      = 40      # Maximum finger movement allowed during a tap click
```

---

## 5. Camera & Advanced Settings

```ini
camera_deadzone_x    = 100    # Horizontal deadzone before camera starts tracking (px)
camera_deadzone_y    = 75     # Vertical deadzone before camera starts tracking (px)
camera_pan_threshold = 0.40   # Analog stick deflection threshold (0.0..1.0) needed to pan
dpad_repeat_ms       = 150    # Repeat delay for D-pad directional inputs (in ms)

# Settings applicable only when world_left_stick = walk:
walk_deadzone        = 0.30
run_threshold        = 0.50
walk_project_tiles   = 2
run_settle_ms        = 50
```
