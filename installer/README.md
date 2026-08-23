# World War VR installer

This directory builds the single per-user setup program distributed to players:

```text
WorldWarVR-Setup.exe
```

The installer consumes a clean, already-built payload directory. It never reads a game installation and must not contain game files or third-party bot assets.

## Required payload layout

The payload root must contain:

```text
WorldWarVR.exe
WorldWarVR.Launcher.dll
WorldWarVR.Launcher.Core.dll
WorldWarVR.ico
wawvr-launcher.exe
WorldWarVR.dll
WaWVR-PeZBOT-Import.ps1
LICENSE
licenses\OpenXR-SDK\LICENSE.txt
licenses\JsonCpp\LICENSE.txt
licenses\nuget\...vendor license and notice files...
licenses\dotnet\...runtime license and notice files...
...self-contained WinUI and .NET runtime files...
```

Only `WorldWarVR.exe` receives a Start Menu shortcut. The native launcher,
VR DLL, support script, runtime dependencies, and genuine dependency terms are
installed beside it as private application support files. Research-reference
or source-provenance documents are not part of the player package.

`WorldWarVR.Launcher.dll` and `WorldWarVR.Launcher.Core.dll` are internal
managed support files installed beside the application.

## Build

Install the pinned Inno Setup 6.7.3 compiler, then run:

```powershell
.\installer\build-installer.ps1 `
  -PayloadDir .\artifacts\standalone-payload `
  -OutputDir .\artifacts\installer `
  -Version 0.4.0-alpha.1 `
  -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

`-InnoCompiler` is optional when the pinned compiler is available through
`PATH` or a standard install location. The script validates the payload,
rejects known game/mod/debug artifacts, checks the pinned compiler SHA-256,
builds the setup program, and prints its SHA-256 and Authenticode status. Pass
`-RequireSignature` for a final-release gate after code signing is wired into
the release pipeline.

The installed 6.7.3 compiler identifies itself as **Non-commercial use only**. It is suitable for today's private test installer and a genuinely non-commercial free release. Before any public release that enables Nexus Donation Points, paid distribution, sponsorship, or other monetization, confirm that this compiler license permits the intended use, obtain the appropriate license, or move the deterministic setup definition to a suitable alternative toolchain.

The setup is per-user and installs to `%LOCALAPPDATA%\Programs\World War VR` by default, so it does not require administrator access. The stable installer identity lets an existing private-alpha installation upgrade in place at its previous location; the upgrade also removes the old executable, VR DLL, icon, PRI file, and Start Menu shortcuts. It creates a Start Menu shortcut and a standard Windows uninstall entry. It does not copy or modify files in the selected game installation.
