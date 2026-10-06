; Instalador de AppToMic (Inno Setup 6).
; Compilar desde la raíz del repo, después de build.bat:
;   ISCC installer\AppToMic.iss
; La versión sale del archivo VERSION (se puede pisar con /DAppVersion=x.y.z).
; Idioma: inglés por defecto; español si Windows está en español.

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
ShowLanguageDialog=auto

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"

[CustomMessages]
en.StartWithWindows=Start AppToMic with Windows
en.OpenCableDownload=Open the VB-CABLE download page (required)
en.CablePageTitle=VB-CABLE is missing
en.CablePageSubtitle=AppToMic needs the VB-CABLE virtual microphone to work.
en.CablePageText=VB-CABLE was not found on this PC. To install it:%n%n  1. Open its official page with the button below and download the Windows package.%n  2. Unzip it and run VBCABLE_Setup_x64.exe as administrator.%n  3. Click "Install Driver" and restart your PC.%n%nYou can continue installing AppToMic in the meantime.%nVB-CABLE is donationware by VB-Audio (www.vb-cable.com).
en.OpenCableButton=Open VB-CABLE page

es.StartWithWindows=Iniciar AppToMic con Windows
es.OpenCableDownload=Abrir la página de descarga de VB-CABLE (necesario)
es.CablePageTitle=Falta VB-CABLE
es.CablePageSubtitle=AppToMic necesita el micrófono virtual VB-CABLE para funcionar.
es.CablePageText=No se encontró VB-CABLE en esta PC. Para instalarlo:%n%n  1. Abrí su página oficial con el botón de abajo y descargá el paquete para Windows.%n  2. Descomprimilo y ejecutá VBCABLE_Setup_x64.exe como administrador.%n  3. Hacé clic en "Install Driver" y reiniciá la PC.%n%nPodés seguir con la instalación de AppToMic mientras tanto.%nVB-CABLE es donationware de VB-Audio (www.vb-cable.com).
es.OpenCableButton=Abrir página de VB-CABLE

[Tasks]
Name: "startup"; Description: "{cm:StartWithWindows}"
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
Filename: "https://vb-audio.com/Cable/"; Description: "{cm:OpenCableDownload}"; Flags: shellexec postinstall nowait skipifsilent; Check: not IsCableInstalled
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
  CablePage := CreateOutputMsgPage(wpSelectTasks, CustomMessage('CablePageTitle'),
    CustomMessage('CablePageSubtitle'), CustomMessage('CablePageText'));

  Button := TNewButton.Create(CablePage);
  Button.Parent := CablePage.Surface;
  Button.Caption := CustomMessage('OpenCableButton');
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
