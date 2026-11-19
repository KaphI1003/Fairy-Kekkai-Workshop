; 脚本由 Inno Setup 脚本向导生成。
; 有关创建 Inno Setup 脚本文件的详细信息，请参阅帮助文档！

#ifdef MyFork
  #define MyAppName "Fairy Kekkai Workshop ASS 适配功能增强版"
  #define MyAppDirName "Fairy Kekkai Workshop Fork"
  #define MyAppPublisher "Fairy Kekkai Workshop Fork contributors"
  #define MyAppId "{{C84B6577-FD18-4D39-99A4-E824A4826425}"
#else
  #define MyAppName "Fairy Kekkai Workshop"
  #define MyAppDirName MyAppName
  #define MyAppPublisher "Fairy Oracle Sanctuary"
  #define MyAppId "{{0C3A7120-57EF-4142-B7E3-C94C8C0FDE46}"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "3.0.0"
#endif
#ifndef MyDisplayVersion
  #define MyDisplayVersion MyAppVersion
#endif
#ifndef MyVariant
  #define MyVariant "CPU-v3.7.0"
#endif
#ifndef MyAppURL
  #ifdef MyFork
    #define MyAppURL ""
  #else
    #define MyAppURL "https://github.com/Fairy-Oracle-Sanctuary/Fairy-Kekkai-Workshop"
  #endif
#endif
; 主程序名与待打包目录允许打包脚本用 /D 覆盖，同一个脚本同时服务两代程序：
;   Python 版（默认）：Fairy-Kekkai-Workshop.exe + dist\Fairy-Kekkai-Workshop.dist
;   C++ 版：           Fairy-Kekkai-Workshop-Cpp.exe + dist-cpp\Fairy-Kekkai-Workshop-Cpp.dist
#ifndef MyAppExeName
  #define MyAppExeName "Fairy-Kekkai-Workshop.exe"
#endif
#ifndef MySourceDir
  #define MySourceDir "dist\Fairy-Kekkai-Workshop.dist"
#endif
#define MyAppAssocName MyAppName + "文件"
#define MyAppAssocExt ".myp"
#define MyAppAssocKey StringChange(MyAppDirName + "文件", " ", "") + MyAppAssocExt
#define MyPayloadExcludes ".fkw-package-stage,.fkw-ocr-files.iss,vc_redist.x64.exe,\AppData\*,\config\*,\config.json,\project.json,\project,\project\*,\Projects,\Projects\*,\Fairy-Kekkai-Workshop-Projects,\Fairy-Kekkai-Workshop-Projects\*"

[Setup]
; 注意：AppId 的值唯一标识此应用程序。不要在其他应用程序的安装程序中使用相同的 AppId 值。
; (若要生成新的 GUID，请在 IDE 中单击 "工具|生成 GUID"。)
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyDisplayVersion}
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductTextVersion={#MyDisplayVersion}
VersionInfoProductName={#MyAppName}
;AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppDirName}
UninstallDisplayIcon={app}\{#MyAppExeName}
; "ArchitecturesAllowed=x64compatible" 指定安装程序无法运行
; 除 Arm 上的 x64 和 Windows 11 之外的任何平台上。
ArchitecturesAllowed=x64compatible
; "ArchitecturesInstallIn64BitMode=x64compatible" 要求
; 安装可以在 x64 或 Arm 上的 Windows 11 上以“64 位模式”完成，
; 这意味着它应该使用本机 64 位 Program Files 目录和
; 注册表的 64 位视图。
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
DisableProgramGroupPage=yes
LicenseFile=LICENSE
; 取消注释以下行以在非管理员安装模式下运行 (仅为当前用户安装)。
PrivilegesRequired=admin
OutputBaseFilename=Fairy-Kekkai-Workshop-v{#MyAppVersion}-{#MyVariant}-Windows-x86_64-Setup
OutputDir=Output
SolidCompression=yes
Compression=lzma2/max
LZMANumBlockThreads=4
WizardStyle=modern
SetupIconFile=app\resource\images\logo.ico

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; 安装前删除旧版本的所有文件和子目录
;Type: filesandordirs; Name: "{app}\*"
; C++ 版沿用同一 AppId 覆盖安装 Python 版时两代主程序文件名不同，旧 exe 不会被自动替换，
; 因此在复制文件前显式删除（仅当打包脚本传入 /DMyRemoveLegacyExe=1 时生效）。
#ifdef MyRemoveLegacyExe
Type: files; Name: "{app}\Fairy-Kekkai-Workshop.exe"
#endif

[Files]
#ifdef MyOcrFiles
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Excludes: "{#MyPayloadExcludes},\tools\PaddleOCR-GPU-*"; Flags: ignoreversion recursesubdirs createallsubdirs
  #include MyOcrFiles
#else
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Excludes: "{#MyPayloadExcludes}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
#if FileExists(MySourceDir + "\vc_redist.x64.exe")
Source: "{#MySourceDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif
; 注意：不要在任何共享系统文件上使用 "Flags: ignoreversion" 

[Registry]
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocExt}\OpenWithProgids"; ValueType: string; ValueName: "{#MyAppAssocKey}"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}"; ValueType: string; ValueName: ""; ValueData: "{#MyAppAssocName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKA; Subkey: "Software\Classes\{#MyAppAssocKey}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

#if FileExists(MySourceDir + "\vc_redist.x64.exe")
[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated runhidden
#endif

[CustomMessages]
english.CleanupIncomplete=Some Workshop files could not be cleaned up. Your project folders have been preserved.
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
chinesesimp.CleanupIncomplete=部分软件文件未能清理。视频项目文件夹已保留。
#endif

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Script, Filename, Params, OtherEdition: String;
  ResultCode: Integer;
  Success: Boolean;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  { Embed the cleanup code so it survives removal of the installed files. }
  Script := '';
#define CleanupHandle
#sub AddCleanupLine
  #emit "  Script := Script + '" + StringChange(FileRead(CleanupHandle), "'", "''") + "' + #13#10;"
#endsub
#for {CleanupHandle = FileOpen(AddBackslash(SourcePath) + "scripts\uninstall-cleanup.ps1"); CleanupHandle && !FileEof(CleanupHandle); ""} AddCleanupLine
#if !CleanupHandle
  #error Unable to read uninstall-cleanup.ps1
#endif
#call FileClose(CleanupHandle)
  Filename := ExpandConstant('{tmp}\fkw-uninstall-cleanup.ps1');
  Success := SaveStringToFile(Filename, Utf8Encode(#$FEFF + Script), False);
  if Success then begin
    Params := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Filename +
      '" -InstallDir "' + AddBackslash(ExpandConstant('{app}')) + '." -UserDataDir "' +
      ExpandConstant('{localappdata}\Fairy-Oracle-Sanctuary\Fairy-Kekkai-Workshop') + '"';
#ifdef MyFork
    OtherEdition := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{0C3A7120-57EF-4142-B7E3-C94C8C0FDE46}_is1';
#else
    OtherEdition := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{C84B6577-FD18-4D39-99A4-E824A4826425}_is1';
#endif
    if RegKeyExists(HKLM64, OtherEdition) or RegKeyExists(HKCU64, OtherEdition) or
       RegKeyExists(HKLM32, OtherEdition) or RegKeyExists(HKCU32, OtherEdition) then
      Params := Params + ' -KeepSharedData';
    Success := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
      Params, ExpandConstant('{tmp}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Success := Success and (ResultCode = 0);
    DeleteFile(Filename);
  end;
  if not Success then begin
    Log(CustomMessage('CleanupIncomplete'));
    SuppressibleMsgBox(CustomMessage('CleanupIncomplete'), mbInformation, MB_OK, IDOK);
  end;
end;

