; LodeStar installer (Inno Setup 6).  Built by System/Engine/Tools/installer/make_release.py:
;   ISCC /DStage=<release folder> /DImages=<bmp folder> /DAppVer=0.1.0 /O<output folder> LodeStar.iss
; Installs the launcher only (no game files, nothing from the game): per user, no admin rights, one folder.
; After the files: the "Build game" page - Build now (the setup wizard: controller, settings, game files, build)
; or Build in the launcher.  The uninstaller removes the whole folder after asking about the saves.

#ifndef AppVer
  #define AppVer "0.1.0"
#endif
#ifndef Stage
  #error Stage (the release folder) must be given: /DStage=...
#endif
#ifndef Images
  #define Images "."
#endif

[Setup]
AppId={{6B1F3E2A-8C4D-4E7A-9F21-3D5C7A9B0E14}
AppName=LodeStar
AppVersion={#AppVer}
AppVerName=LodeStar {#AppVer}
AppPublisher=Kimo
AppPublisherURL=https://github.com/marcelkimo778-afk/LodeStar-Recomp
DefaultDirName={localappdata}\Programs\LodeStar
DefaultGroupName=LodeStar
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=LodeStar-Setup-{#AppVer}
SetupIconFile={#Stage}\System\launcher\assets\launcher.ico
UninstallDisplayIcon={app}\LodeStar.exe
UninstallDisplayName=LodeStar
WizardStyle=modern
WizardImageFile={#Images}\wizard_large_1x.bmp,{#Images}\wizard_large_2x.bmp
WizardSmallImageFile={#Images}\wizard_small_1x.bmp,{#Images}\wizard_small_2x.bmp
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
RestartApplications=no

[Messages]
WelcomeLabel1=Welcome to LodeStar
WelcomeLabel2=This installs the LodeStar launcher on your PC.%n%nNo game files are included: you build Pokémon Legends: Arceus (1.0.0) yourself from your own dump, right after the installation or later in the launcher.
FinishedHeadingLabel=LodeStar is installed
FinishedLabelNoIcons=Have fun!
FinishedLabel=LodeStar is installed. Start it any time from the Start menu.

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Dirs]
Name: "{app}\Game"
Name: "{app}\Mods"
Name: "{app}\Settings"

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\LodeStar"; Filename: "{app}\LodeStar.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\LodeStar"; Filename: "{app}\LodeStar.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; the launcher is Python: installed for this user through Windows' winget when it is missing
Filename: "winget"; Parameters: "install -e --id Python.Python.3.14 --scope user --silent --accept-source-agreements --accept-package-agreements --disable-interactivity"; StatusMsg: "Installing Python 3 (the launcher needs it)..."; Check: NeedPython; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  BuildPage: TInputOptionWizardPage;

function FindPyw(): String;
var
  Cands: array of String;
  I: Integer;
begin
  Result := '';
  SetArrayLength(Cands, 5);
  Cands[0] := ExpandConstant('{win}\pyw.exe');
  Cands[1] := ExpandConstant('{localappdata}\Programs\Python\Launcher\pyw.exe');
  Cands[2] := ExpandConstant('{localappdata}\Programs\Python\Python314\pythonw.exe');
  Cands[3] := ExpandConstant('{autopf}\Python314\pythonw.exe');
  Cands[4] := ExpandConstant('{localappdata}\Programs\Python\Python313\pythonw.exe');
  for I := 0 to GetArrayLength(Cands) - 1 do
    if FileExists(Cands[I]) then
    begin
      Result := Cands[I];
      Exit;
    end;
end;

function NeedPython(): Boolean;
begin
  Result := FindPyw() = '';
end;

procedure InitializeWizard();
begin
  BuildPage := CreateInputOptionPage(wpInfoAfter, 'Build the game',
    'LodeStar turns your own dump of Pokémon Legends: Arceus 1.0.0 into a PC game - once, on your PC.',
    'Build now walks you through your controller, the best settings for this PC and importing your game files, ' +
    'then builds the game (10 to 30 minutes, with a progress bar and the time left). You can also do it later: ' +
    'the launcher has the same steps.', True, False);
  BuildPage.Add('Build now');
  BuildPage.Add('Build later, in the launcher');
  BuildPage.SelectedValueIndex := 0;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = BuildPage.ID) and WizardSilent();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Code: Integer;
  Pyw: String;
begin
  Result := True;
  if CurPageID = BuildPage.ID then
  begin
    Pyw := FindPyw();
    if BuildPage.SelectedValueIndex = 0 then
    begin
      if Pyw = '' then
        MsgBox('Python was not found, so the build wizard cannot start. Start LodeStar from the Start menu and ' +
               'build from there.', mbError, MB_OK)
      else
        ExecAsOriginalUser(Pyw, '"' + ExpandConstant('{app}\System\launcher\setup_wizard.py') + '"',
                           ExpandConstant('{app}'), SW_SHOW, ewNoWait, Code);
    end
    else
      ExecAsOriginalUser(ExpandConstant('{app}\LodeStar.exe'), '', ExpandConstant('{app}'), SW_SHOW, ewNoWait, Code);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Saves, Dest: String;
  Code: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    Saves := ExpandConstant('{app}\System\build_work\out');
    if DirExists(Saves + '\save') or DirExists(Saves + '\save_slots') then
      if UninstallSilent() or (MsgBox('Keep a copy of your saves?' + #13#10#13#10 +
         'They are copied to Documents\LodeStar saves before LodeStar''s folder is removed.',
         mbConfirmation, MB_YESNO) = IDYES) then
      begin
        Dest := ExpandConstant('{userdocs}\LodeStar saves\') + GetDateTimeString('yyyy-mm-dd_hhnnss', '-', '-');
        ForceDirectories(Dest);
        if DirExists(Saves + '\save') then
          Exec(ExpandConstant('{sys}\robocopy.exe'), '"' + Saves + '\save" "' + Dest + '\save" /E /NFL /NDL /NJH /NJS',
               '', SW_HIDE, ewWaitUntilTerminated, Code);
        if DirExists(Saves + '\save_slots') then
          Exec(ExpandConstant('{sys}\robocopy.exe'), '"' + Saves + '\save_slots" "' + Dest + '\save_slots" /E /NFL /NDL /NJH /NJS',
               '', SW_HIDE, ewWaitUntilTerminated, Code);
      end;
  end;
end;
