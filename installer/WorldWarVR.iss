#define ProductName "World War VR"
#define ProductExe "WorldWarVR.exe"
#define ProductPublisher "Ryan Craighead"
#define ProductUrl "https://github.com/RyanCraighead/WorldWarVR-Releases"

#ifndef PayloadDir
  #error PayloadDir must point to the validated standalone payload directory.
#endif

#ifndef InstallerOutputDir
  #error InstallerOutputDir must point to the installer output directory.
#endif

#ifndef ProductVersion
  #define ProductVersion "0.4.0-alpha.1"
#endif

#ifndef ProductFileVersion
  #define ProductFileVersion "0.4.0.0"
#endif

[Setup]
AppId={{0BB6E20A-13C4-4E3F-A649-C9320325E367}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppVerName={#ProductName} {#ProductVersion}
AppPublisher={#ProductPublisher}
AppPublisherURL={#ProductUrl}
AppSupportURL={#ProductUrl}/issues
AppUpdatesURL={#ProductUrl}/releases
VersionInfoVersion={#ProductFileVersion}
VersionInfoDescription={#ProductName} installer
VersionInfoProductName={#ProductName}
VersionInfoProductVersion={#ProductFileVersion}
SetupIconFile={#PayloadDir}\WorldWarVR.ico
DefaultDirName={localappdata}\Programs\World War VR
DisableDirPage=yes
DefaultGroupName={#ProductName}
DisableProgramGroupPage=yes
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={app}\{#ProductExe}
OutputDir={#InstallerOutputDir}
OutputBaseFilename=WorldWarVR-Setup
LicenseFile={#PayloadDir}\LICENSE
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
UsePreviousAppDir=yes
UsePreviousGroup=no
DirExistsWarning=no
DisableWelcomePage=no
DisableReadyPage=no
AllowNoIcons=yes
ChangesAssociations=no
ChangesEnvironment=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\WorldAtWarVR.exe"
Type: files; Name: "{app}\WorldAtWarVR.dll"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.exe"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.dll"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.deps.json"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.runtimeconfig.json"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.pdb"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.pri"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.Core.dll"
Type: files; Name: "{app}\WorldAtWarVR.Launcher.Core.pdb"
Type: files; Name: "{app}\WorldAtWarVR.ico"
Type: files; Name: "{app}\WorldAtWarVR.pri"
Type: files; Name: "{userprograms}\WorldAtWarVR\WorldAtWarVR.lnk"
Type: files; Name: "{userprograms}\WorldAtWarVR\Uninstall WorldAtWarVR.lnk"
Type: dirifempty; Name: "{userprograms}\WorldAtWarVR"

[Icons]
Name: "{group}\World War VR"; Filename: "{app}\{#ProductExe}"; WorkingDir: "{app}"; Comment: "Launch Call of Duty: World at War in VR"
Name: "{group}\Uninstall World War VR"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#ProductExe}"; Description: "Launch World War VR"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
