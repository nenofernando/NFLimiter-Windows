#ifndef BuildRoot
  #error BuildRoot must point to build-vst3-windows-x64
#endif

#ifndef OutputDir
  #define OutputDir ".\dist"
#endif

#define PluginName "NF Limiter"
#define PluginVersion "1.0.0"
#define InstallerVersion "1.0.0"
#define Publisher "NF Audio Tools"
#define Copyright "NF Audio Tools - By Neno Fernando. All rights reserved."
#define AuthorLine "NF Audio Tools - By Neno Fernando"

[Setup]
AppId={{6C6E664C-696D-4954-4552-4E464C494D31}
AppName={#PluginName}
AppVersion={#PluginVersion}
AppVerName={#PluginName} {#PluginVersion}
AppPublisher={#Publisher}
AppCopyright={#Copyright}
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=no
DefaultGroupName={#PluginName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=NF-Limiter-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#PluginName} {#PluginVersion}
VersionInfoVersion={#InstallerVersion}
VersionInfoCompany={#Publisher}
VersionInfoCopyright={#Copyright}
VersionInfoDescription={#PluginName} {#PluginVersion} Windows x64 installer ({#AuthorLine}) - True Peak Mastering Limiter
VersionInfoProductName={#PluginName}
VersionInfoProductVersion={#PluginVersion}
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "manualicon"; Description: "Create Start Menu shortcuts to the English and Portuguese manuals"; GroupDescription: "Shortcuts:"; Flags: checkedonce

[Files]
Source: "{#BuildRoot}\NFLimiter_artefacts\Release\VST3\NF Limiter.vst3\*"; DestDir: "{commoncf64}\VST3\NF Limiter.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Manuals\*"; DestDir: "{commonappdata}\NF Audio Tools\NF Limiter\Manuals"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Manuals\*"; DestDir: "{commoncf64}\VST3\NF Limiter.vst3\Contents\Resources\Manuals"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#PluginName} Manual (English)"; Filename: "{commonappdata}\NF Audio Tools\NF Limiter\Manuals\NF_Limiter_Manual_EN.pdf"; Tasks: manualicon
Name: "{group}\{#PluginName} Manual (Portuguese)"; Filename: "{commonappdata}\NF Audio Tools\NF Limiter\Manuals\NF_Limiter_Manual_PT.pdf"; Tasks: manualicon
Name: "{group}\Uninstall {#PluginName}"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\NF Limiter.vst3"
Type: filesandordirs; Name: "{commonappdata}\NF Audio Tools\NF Limiter"
