# Launcher publish contract

Publish the unpackaged, self-contained Windows x64 launcher from this directory:

```powershell
dotnet restore .\WorldAtWarVR.Launcher.csproj -r win-x64
dotnet publish .\WorldAtWarVR.Launcher.csproj -c Release -r win-x64 --self-contained true -p:Platform=x64 --no-restore -o .\artifacts\publish\win-x64
```

The `BrandPublishedAppHost` target renames the generated native apphost to
`WorldWarVR.exe` and its WinUI resource index to `WorldWarVR.pri`. Those
basenames must match for unpackaged WinUI resource lookup. The managed entry
assembly is `WorldWarVR.Launcher.dll`, and its shared core assembly is
`WorldWarVR.Launcher.Core.dll`. The `.Launcher` suffix leaves the public
`WorldWarVR.dll` filename available for the native x86 VR mod.

The release packager must copy the complete publish directory, then add these
native files beside `WorldWarVR.exe`:

- `wawvr-launcher.exe`
- `WorldWarVR.dll`
- any required native launcher support assets

The UI resolves those files exclusively through `AppContext.BaseDirectory`.
It does not rely on the repository or a developer-machine path.

For a complete payload and installer build, use the repository packager so the
same release version is applied to the launcher assembly, Windows file
metadata, informational version, and installer:

```powershell
.\scripts\package-standalone.ps1 `
  -Configuration Release `
  -Version 0.4.0-alpha.1 `
  -InnoCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
```

`-Version` accepts `major.minor.patch` with an optional `-alpha.N`, `-beta.N`,
or `-rc.N` suffix. `-InnoCompiler` is optional; omit it when the pinned Inno
Setup 6 compiler is already discoverable through `PATH` or a standard install
location. Pass `-SkipInstaller` when only the versioned payload is required.

The verified clean external publish baseline contains 507 files totaling
220,752,095 bytes (210.53 MiB) before the native helper, mod DLL,
documentation, or installer compression are added. It uses the serviced stable
Windows App SDK 1.8.6 self-contained runtime. `EnableMsixTooling` intentionally
remains enabled so the WinUI PRI resource index is generated; the application
itself remains unpackaged because `WindowsPackageType` is `None`.
