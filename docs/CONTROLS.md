# World War VR controls and recovery notes

## Gameplay

- Headset: 6DOF view and position.
- Right controller pose: weapon aim.
- Right trigger: fire.
- Hip-fired local VR bullets use the current weapon's fixed ADS-level accuracy;
  the native expanding desktop hip-fire cone is not applied.
- Left trigger: reload.
- Left grip: hold/cook the secondary tactical grenade; release to throw.
- Left stick: move relative to the headset's horizontal heading. Physical HMD
  turning automatically brings the hidden native player body into the same
  heading, so sprint and interaction traces do not retain an invisible old
  facing direction.
- Left stick click: start sprint. One normal click stays latched while the
  movement stick remains outside its deadzone; returning the stick to neutral
  clears sprint.
- Right stick left/right: horizontal snap turn.
- X: use/interact, including doors, wall weapons, the mystery box, and revive.
- A: jump.
- B: crouch.
- Y/left-secondary: a plain tap switches to the next weapon when released.
- Campaign support selector: hold Y/left-secondary, then push the left stick
  left once to send WaW's native `6` key. In Little Resistance this equips
  the rocket-barrage designator. Point it with the right controller and use
  the normal right trigger. Locomotion remains active while the chord is held;
  completing it consumes Y's release so it cannot also switch weapons. The
  same hidden mission D-pad provides up=`5`, down=`N`, and right=`7` for
  matching native campaign prompts; the path cannot run in multiplayer.
  Passive Touch thumbrest contact is never treated as a modifier.
- Right stick click or a fast outward right-controller knife swing: melee.
  The knife action and damage remain native, its comfortable trace range is
  extended to 96 game units, and target-assisted camera rotation/lunge is
  suppressed for VR comfort.
- Right grip: hold/cook a frag grenade, point the right controller, and
  release to throw. The native offhand model is attached to the controller
  and shows the launch direction; turning only your head does not redirect it.
- Left Menu short tap: Escape/open or close the native menu.
- Left Menu held for one second: recenter position and facing direction while
  always restoring a gravity-level horizon.

The desktop firearm crosshair is disabled during VR gameplay. Aim with the
visible barrel; use prompts, grenade warnings, ammo, points, and round HUD
remain available. Gameplay HUD elements are reduced and pulled into the
central binocular field so ammo, points, round, and minimap elements can be
read without turning the eyes toward the uncomfortable edges of the lenses.

Local firearm tracers and predicted impacts originate at the visible barrel
and use the same fixed ADS-level cone as the authoritative shot. Blood,
projectile trails, and explosions remain native game effects; the launcher
disables the two stock camera-dependent element discard checks that are unsafe
for the late HMD camera.

Prone and previous-weapon inputs are intentionally unbound in this MVP. Native
keyboard input—including desktop ADS—remains available on the desktop.

## Native menus and cinematics

- Point the right controller at the finite menu panel to move WaW's native
  cursor. An orange ring/dot is drawn into the menu itself at the hit point;
  pull the right trigger once to click the pointed item.
- Left stick moves the native menu cursor.
- A confirms/selects.
- B goes back.
- The frontend, loading screens, console, and cinematics are presented as a
  gravity-level panel fixed two metres into OpenXR Local space. Moving your
  head left/right/up/down produces real parallax; gameplay switches to stereo
  VR. Controller pointing is enabled only while WaW's native UI owns input.

Launch the configured `WorldWarVR.exe` for the stock Zombies frontend. The
managed launcher supplies the game directory; standalone use requires
`--game-dir`, `WAWVR_GAME_DIR`, or an already prepared launcher-managed runtime.
That same
executable boots Nacht with `--launch`, Der Riese with
`--launch --der-riese`, or the frontend with `--launch --menu`; every form
automatically validates and injects the adjacent `WorldWarVR.dll`.

## Offline multiplayer and bots

Choose **Launch Multiplayer** from the single-player frontend or double-click:

```text
WorldWarVR-Multiplayer.exe
```

The command-line equivalent is `WorldWarVR.exe --launch --multiplayer`.

The single-player process exits before the isolated multiplayer runtime starts,
so OpenXR is handed off rather than shared by two game processes. From the MP
frontend, create a local game and choose a stock map/mode. Gameplay uses the
same headset, controller, weapon, and menu bindings above.

World War VR does not bundle bots. To enable PeZBOT, place the exact
user-owned `PeZBOTWAW_005p.zip` beside the launcher or in Downloads before
starting MP. A missing or rejected archive leaves ordinary offline multiplayer
available. See `docs\OFFLINE_MULTIPLAYER.md` for the accepted archive identity,
safe import behavior, and stage-only commands.

## Resolution and recovery

The default packed side-by-side source is `2560x1440` (`1280x1440` per eye
before OpenXR remapping). This is 2.56 times the source pixels of the accepted
`1600x900` build while preserving the same per-eye aspect ratio. If performance
regresses, use the previously accepted setting:

```text
WorldWarVR.exe --launch --resolution 1600x900
```

For minimum-cost device-loss recovery, use:

```text
wawvr-launcher.exe --launch --resolution 1024x768 --mod-dll WorldWarVR.dll
```

`WAWVR_SOURCE_RESOLUTION` provides the same override without changing a
shortcut; an explicit `--resolution` value takes precedence.

On this exact T4 executable's windowed path, `r_customMode` is parsed before
the stock `r_mode` resolution enum. The launcher therefore sets
`r_customMode` directly and intentionally does not use the older IW3-style
`r_mode -1` override.

The launcher disables stock depth-of-field with `r_dof_enable 0` for all
launch targets. This removes the legacy full-screen focus blur when aiming
down sights; it does not disable ADS itself.

For fail-closed diagnosis, set one of these environment variables to `1`
before starting the launcher:

- `WAWVR_DISABLE_XR`: leave WaW on its stock desktop renderer.
- `WAWVR_DISABLE_INPUT`: leave native game input untouched.
- `WAWVR_DISABLE_WEAPON`: disable tracked weapon placement and ballistics.
- `WAWVR_DISABLE_WEAPON_CAMERA_PATCH`: restore stock `tag_camera` movement.
- `WAWVR_DISABLE_MELEE_CAMERA_PATCH`: restore stock auto-melee target aiming.
- `WAWVR_DISABLE_DIRECT_BOOT`: restore the stock startup cinematic command.

These are recovery controls, not normal play settings. Remove the variable or
set it to `0` to restore the corresponding VR feature on the next launch.

For a targeted renderer investigation only,
`WAWVR_FX_STEREO_DIAGNOSTICS=1` enables changing backend emissive/light-state
telemetry. Leave it unset during ordinary play; the opt-in path writes from
the render thread and is intentionally disabled by default.

World War VR does not include the game. The launcher validates and stages a
user-owned compatible World at War 1.7 executable and links to its existing
data without modifying the installation.
