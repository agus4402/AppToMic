; Instalador de AppToMic (Inno Setup 6).
; Compilar desde la raíz del repo, después de build.bat:
;   ISCC installer\AppToMic.iss
; La versión sale del archivo VERSION (se puede pisar con /DAppVersion=x.y.z).

#ifndef AppVersion
  #define VersionFile FileOpen(AddBackslash(SourcePath) + "..\VERSION")
  #define AppVersion Trim(FileRead(VersionFile))
  #expr FileClose(VersionFile)
#endif

[Setup]
AppId={{9F792399-BBE4-4F18-97CF-E5C52CFDCBEF}
AppName=AppToMic
AppVersion={#AppVersion}
AppPublisher=agus4402
AppPublisherURL=https://github.com/agus4402/AppToMic
DefaultDirName={autopf}\AppToMic
DisableProgramGroupPage=yes
PrivilegesRequired=admin
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; La captura de audio por proceso existe desde Windows 10 build 20348 / Windows 11.
MinVersion=10.0.20348
OutputDir=..\dist
OutputBaseFilename=AppToMic-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\AppToMic.exe
UninstallDisplayName=AppToMic

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[Tasks]
Name: "startup"; Description: "Iniciar AppToMic con Windows"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\AppToMic.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\AppToMic"; Filename: "{app}\AppToMic.exe"
Name: "{autodesktop}\AppToMic"; Filename: "{app}\AppToMic.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "AppToMic"; ValueData: """{app}\AppToMic.exe"" /tray"; Tasks: startup

[Run]
; La licencia de VB-CABLE no permite integrarlo en otro instalador, así que solo guiamos al usuario.
Filename: "https://vb-audio.com/Cable/"; Description: "Abrir la página de descarga de VB-CABLE (necesario)"; Flags: shellexec postinstall nowait skipifsilent; Check: not IsCableInstalled
Filename: "{app}\AppToMic.exe"; Description: "{cm:LaunchProgram,AppToMic}"; Flags: nowait postinstall skipifsilent

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  CableUrl = 'https://vb-audio.com/Cable/';
  // PKEY_Device_DeviceDesc
  DeviceDescKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},2';

var
  CablePage: TOutputMsgWizardPage;

function IsCableInstalled: Boolean;
var
  Base, Desc: String;
  Keys: TArrayOfString;
  I: Integer;
begin
  Result := False;
  Base := 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render';
  if not RegGetSubkeyNames(HKLM64, Base, Keys) then Exit;
  for I := 0 to GetArrayLength(Keys) - 1 do
    if RegQueryStringValue(HKLM64, Base + '\' + Keys[I] + '\Properties', DeviceDescKey, Desc) and
       (Pos('CABLE Input', Desc) > 0) then
    begin
      Result := True;
      Exit;
    end;
end;

procedure StopRunningApp;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM AppToMic.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

procedure OpenCablePage(Sender: TObject);
var
  Code: Integer;
begin
  ShellExec('open', CableUrl, '', '', SW_SHOWNORMAL, ewNoWait, Code);
end;

procedure InitializeWizard;
var
  Button: TNewButton;
begin
  CablePage := CreateOutputMsgPage(wpSelectTasks, 'Falta VB-CABLE',
    'AppToMic necesita el micrófono virtual VB-CABLE para funcionar.',
    'No se encontró VB-CABLE en esta PC. Para instalarlo:' + #13#10#13#10 +
    '  1. Abrí su página oficial con el botón de abajo y descargá el paquete para Windows.' + #13#10 +
    '  2. Descomprimilo y ejecutá VBCABLE_Setup_x64.exe como administrador.' + #13#10 +
    '  3. Hacé clic en "Install Driver" y reiniciá la PC.' + #13#10#13#10 +
    'Podés seguir con la instalación de AppToMic mientras tanto.' + #13#10 +
    'VB-CABLE es donationware de VB-Audio (www.vb-cable.com).');

  Button := TNewButton.Create(CablePage);
  Button.Parent := CablePage.Surface;
  Button.Caption := 'Abrir página de VB-CABLE';
  Button.Width := ScaleX(180);
  Button.Height := WizardForm.NextButton.Height;
  Button.Left := 0;
  Button.Top := CablePage.SurfaceHeight - Button.Height;
  Button.OnClick := @OpenCablePage;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = CablePage.ID) and IsCableInstalled;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopRunningApp;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // La app también puede crear este valor desde "Iniciar con Windows".
  if CurUninstallStep = usUninstall then RegDeleteValue(HKCU, RunKey, 'AppToMic');
end;
