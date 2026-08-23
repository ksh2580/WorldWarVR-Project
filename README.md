# World War VR

World War VR is an in-development, standalone OpenXR compatibility mod for the
32-bit Windows release of *Call of Duty: World at War*. The accepted gameplay
baseline is solo Zombies, with validated Nacht and Der Riese targets. The same
launcher now has a separately isolated offline multiplayer path; bot support is
optional and uses a user-supplied, exactly verified PeZBOT archive.

The intended runtime is:

```text
WorldWarVR.exe
  -> validates and launches the user's own WaW 1.7 executable at its frontend
  -> injects WorldWarVR.dll
  -> WorldWarVR.dll connects WaW rendering/input to OpenXR

WorldWarVR-Multiplayer.exe
  -> launches the separately isolated offline multiplayer frontend with the
     same VR DLL and enables an already verified PeZBOT installation
```

With `--game-dir` supplied by the managed launcher (or `WAWVR_GAME_DIR` set),
the user-facing executable opens WaW's stock frontend so Zombies maps can be
selected normally. Use `WorldWarVR.exe --launch` for direct Nacht,
`WorldWarVR.exe --launch --der-riese` for the final DLC map, or
`WorldWarVR.exe --launch --menu` for an explicit frontend launch. Double-click
`WorldWarVR-Multiplayer.exe` for the separately isolated offline multiplayer
frontend (the equivalent of `WorldWarVR.exe --launch --multiplayer`); the
stock frontend's Launch Multiplayer action uses the same runtime-only handoff.
After preparation, both branded launchers can securely recover the game root
from their paired launcher-managed runtime junctions. The branded
executable selects its adjacent VR DLL for every form; it never silently starts
an unmodded direct-map session.

Offline multiplayer bots are optional. Supply the exact supported user-owned
`PeZBOTWAW_005p.zip` with `--pezbot-archive`, place it beside
`WorldWarVR.exe`, or leave it in Downloads. The launcher accepts only the
exact supported 1,138,246-byte archive with MD5
`4defeab88624baf05d28bbebf6c86c01` and SHA-256
`b7958b96cbe3a8c316290df7148c63ca601d1de2f96f6166d2c67fe069500fdf`, imports its allowlisted files under the
isolated derived MP home (by default `%LOCALAPPDATA%\WaWVR\home-mp`) and enables
nine local bots with authentic faction weapons, perks off, and skill 0.7. It
does not download or package PeZBOT. A missing/rejected archive or an existing
custom mod folder leaves base offline multiplayer available and unchanged.
That LocalAppData path is the standalone default; an explicit SP `--homepath`
uses its sibling `home-mp` instead. The managed no-argument MP shim performs no
archive discovery or import and uses only the already prepared MP home. MP
launches pass that isolated root as both `fs_homepath` and `fs_localAppData` so
World at War loads the verified IWD and its adjacent `mod.ff` together.

The stock frontend's runtime-only `CoDWaWmp.exe` shim accepts no arguments and
does not rediscover game, source, runtime, or home paths. SP preparation writes a strict configuration
beside it inside the validated isolated runtime, carrying the resolved game
root, exact MP source, SP-derived MP stage/home paths, and source resolution.
After verifying and waiting for the exact SP parent, the shim fails closed if
that file is absent, malformed, unsafe, or belongs to another stage.
Managed SP launches can supply the exact MP executable with
`--mp-source-exe FILE`; it overrides environment/discovery while building the
trusted SP plan and is rejected for direct `--multiplayer` launches.

The default packed side-by-side source resolution is `2560x1440`
(`1280x1440` per eye), the highest resolution accepted by the exact renderer on
the current 2560x1440 desktop. Use `--resolution 1600x900` for the previously
accepted performance setting, or `--resolution 1024x768` for recovery.

The launcher does not require the Plutonium client or bootstrapper. It may
locate a compatible executable already present on the machine, but this
repository never contains or redistributes the game executable or assets.

## Current status

The exact supported WaW 1.7.1263 identities now run through the standalone
launcher with validated injection, OpenXR stereo gameplay, 6DOF head tracking,
tracked right-hand gun placement, controller-aligned bullet origin, horizontal
snap turn, gun-only first-person models, and reload-safe camera handling.
This includes untouched Steam public Build 252004 SP and MP executables in
separate identity-bound runtime stages. On 2026-08-11, a staged Steam SP launch
validated its wrapped profile, installed every VR hook, and initialized the
headset's recommended 1648x1776 OpenXR swapchain for each eye.

The current frontend candidate submits loading states, pause UI, and cinematics
only as a finite OpenXR panel anchored two metres into Local space. It never
falls through to a translation-infinite stereo projection when the panel is
unavailable. A procedural pointer is drawn directly into the captured menu
surface at the right-controller hit point, while left-stick cursor movement and
A/B remain available during capture gaps. A prior live frontend run selected
and entered Der Riese (`nazi_zombie_factory`) through native menus; the stricter
world-fixed panel and visible-pointer revision still requires headset
acceptance.

The headset-accepted MVP raises the packed source resolution to
`2560x1440`, disables the legacy ADS depth-of-field blur, maps Y to next weapon
and right grip to controller-directed frag throws, removes the redundant
desktop crosshair, scales the native gameplay HUD to 42% and pulls it well
inside a binocular-safe area in each eye, prevents
knife/reload/spawn animation from tilting the HMD camera, and makes a manual
recenter restore a level horizon. Local tracked firearm shots use the weapon's
fixed ADS-level cone without entering ADS state or slowing movement. Their
local tracer and predicted impact start at the tracked barrel and use that same
cone; the launcher enables first-person tracers and relaxes only the two
element-culling switches that could otherwise discard HMD-visible blood,
projectile trails, and explosions. Projectile weapons such as the Ray Gun also
receive the fresh tracked muzzle. On
2026-08-02, the exact supported machine/headset accepted this visual/gameplay baseline as
sharper, with the HUD far enough away and the stereo path stable.

## Building

Requirements:

- Windows 10 or later
- Visual Studio 2022 Build Tools with the x86 C++ toolchain
- CMake 3.25 or newer
- an OpenXR runtime for headset testing

Configure and build the 32-bit Debug preset:

```powershell
cmake --preset win32-debug
cmake --build --preset debug
ctest --preset debug
```

The repository also includes a PowerShell wrapper that discovers the installed
Visual Studio x86 toolchain and uses its Win32 project generator:

```powershell
./scripts/build.ps1 -Configuration Debug
```

Build and temporary trees can be kept off the system drive without changing
the source checkout:

```powershell
./scripts/build.ps1 -Configuration Debug `
  -BuildRoot E:\codex-builds\waw-vr `
  -TempRoot E:\codex-builds\waw-vr\tmp
```

The wrapper intentionally uses one non-reused MSBuild worker. This avoids the
duplicate `PATH`/`Path` environment failure observed with parallel workers and
keeps generated output deterministic enough for the two-clean-build release
gate.

Build, test, and stage the standalone Release package into the clean
`dist\WorldWarVR` folder with:

```powershell
./scripts/package.ps1
```

For an off-system-drive build and staging pass, use:

```powershell
./scripts/package.ps1 `
  -BuildRoot E:\codex-builds\waw-vr\release `
  -TempRoot E:\codex-builds\waw-vr\tmp `
  -OutputRoot E:\codex-builds\waw-vr\packages
```

`-OutputRoot` controls the final package directory as well as the default
staging parent, so a complete release pass can remain off the system drive.

`-StagingRoot` remains available when the temporary staging parent must differ
from the final output root.

The package contains only the three executable entry points, injected VR DLL, the hashed WaWVR
PeZBOT import helper (not the mod), its README and
five packaged guides, the World War VR GNU GPL version 3 license, OpenXR SDK
Apache-2.0 terms, JsonCpp Public-Domain/MIT terms, notices, and a
recursive SHA-256 manifest. It never includes game files. See
[`docs/CONTROLS.md`](docs/CONTROLS.md) inside the package for current bindings
and recovery options.

## Safety and compatibility

Every supported executable profile must pass architecture, file identity, and
hook-site byte validation before the mod installs a patch. Unsupported builds
fail closed. Runtime staging is reversible and user-owned game files are not
modified in place.

See [docs/architecture.md](docs/architecture.md),
[docs/test-plan.md](docs/test-plan.md), and
[docs/OFFLINE_MULTIPLAYER.md](docs/OFFLINE_MULTIPLAYER.md).

## License

Copyright (c) 2026 Ryan Craighead. World War VR first-party code is licensed
under the GNU General Public License version 3 only (`GPL-3.0-only`), as stated
in [LICENSE](LICENSE). Third-party components remain under their own licenses,
as recorded in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Release
packages also include the OpenXR SDK and JsonCpp license texts.
