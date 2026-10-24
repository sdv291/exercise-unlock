; Exercise Unlock — Windows installer (Inno Setup 6, English + Ukrainian).
;
; Build with:  pwsh -File installer\build-installer.ps1
; which compiles the solution, stages everything under installer\build\
; and runs ISCC with AppVersion / StageDir defined.
;
; What setup does besides copying files:
;   * installs the Visual C++ 2015-2022 x64 runtime when it is missing or
;     older than the bundled vc_redist.x64.exe (staged from Visual Studio):
;     every binary and onnxruntime.dll link the CRT dynamically
;   * if a version is already installed, opens on a page with "Update"
;     (keeps the account, password and calibration when Windows still
;     accepts the stored password - nothing else is asked) and "Remove
;     everything" (runs the installed uninstaller, then cleans up)
;   * asks which local account the exercises unlock (writes users.allow)
;   * asks for that account's password, checks it with Windows and stores
;     it in the DPAPI vault (via ExerciseUnlockAdmin, password passed in an
;     environment variable — never on a command line or on disk)
;   * registers the credential provider, installs + hardens the service
;   * optionally disables PIN / Windows Hello sign-in (tools\lock-user.ps1)
;   * optionally runs the angle calibration (ExerciseUnlockCalibrate.exe):
;     live camera + 4 photos -> thresholds written to config.ini
; All runtime data (vault, state, logs, users.allow, sounds) lives in
; {app}\data, locked to SYSTEM + Administrators; components find it via
; HKLM\SOFTWARE\ExerciseUnlock\InstallDir (ExerciseUnlockCommon\Paths.h).
; Uninstall reverses all of it (the shared VC++ runtime stays).

#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#ifndef StageDir
  #define StageDir "build"
#endif

#define AppName       "Exercise Unlock"
#define ProviderClsid "{{8FDE7B3A-3E5A-4F1C-9C88-6B5F1B7D9E20}"

; The bundled redistributable's file version is the minimum VC++ runtime
; version setup accepts (it matches the runtime's registry Major.Minor.Bld).
#define VCRedistVersion GetVersionNumbersString(AddBackslash(SourcePath) + StageDir + "\vc_redist.x64.exe")
#if VCRedistVersion == ""
  #error vc_redist.x64.exe is missing from the stage dir - build with build-installer.ps1
#endif

[Setup]
AppId={{6E1F4C52-8B0A-4C7E-9A61-2E5B8D3F7A10}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Exercise Unlock
AppPublisherURL=https://github.com/sdv291/exercise-unlock
AppSupportURL=https://github.com/sdv291/exercise-unlock
DefaultDirName={autopf}\ExerciseUnlock
DisableDirPage=yes
DisableProgramGroupPage=yes
DefaultGroupName=Exercise Unlock
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile=..\LICENSE
SetupIconFile={#StageDir}\app.ico
UninstallDisplayIcon={app}\app.ico
; The app icon in the wizard's top corner; Setup picks the size for the DPI.
WizardSmallImageFile=..\branding\png\wizard-55.png,..\branding\png\wizard-69.png,..\branding\png\wizard-83.png,..\branding\png\wizard-110.png,..\branding\png\wizard-138.png
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}
OutputDir=out
OutputBaseFilename=ExerciseUnlock-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=yes
LanguageDetectionMethod=uilanguage
CloseApplications=no
; %TEMP%\Setup Log <date>.txt - lists every helper command and its exit code
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Files]
Source: "{#StageDir}\bin\ExerciseUnlockService.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\bin\ExerciseUnlockAdmin.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\bin\ExerciseUnlockCalibrate.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\bin\onnxruntime.dll";            DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\bin\movenet_lightning.onnx";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\config.ini";                     DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "{#StageDir}\app.ico";                        DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\tools\*.ps1";                    DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#StageDir}\vc_redist.x64.exe";              DestDir: "{tmp}"; Flags: deleteafterinstall; Check: VCRedistNeeded
; LogonUI loads the provider from System32; if it's in use, replace on reboot.
Source: "{#StageDir}\bin\ExerciseUnlockProvider.dll"; DestDir: "{sys}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "..\sounds\*.wav";                            DestDir: "{app}\data\sounds"; Flags: onlyifdoesntexist
Source: "sounds.ini";                                 DestDir: "{app}\data"; Flags: onlyifdoesntexist

[Registry]
; Credential provider: COM server + registration so LogonUI enumerates it.
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#ProviderClsid}"; ValueType: string; ValueName: ""; ValueData: "ExerciseUnlockProvider"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#ProviderClsid}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{sys}\ExerciseUnlockProvider.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#ProviderClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{#ProviderClsid}"; ValueType: string; ValueName: ""; ValueData: "ExerciseUnlockProvider"; Flags: uninsdeletekey
; Setup's own memory (chosen account, lockdown applied) for upgrades / uninstall.
Root: HKLM; Subkey: "SOFTWARE\ExerciseUnlock"; Flags: uninsdeletekey
; Tells the service, the provider and the admin tool where {app}\data is.
Root: HKLM; Subkey: "SOFTWARE\ExerciseUnlock"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"

[Tasks]
Name: "calibrate"; Description: "{cm:TaskCalibrate}"
Name: "lockdown"; Description: "{cm:TaskLockdown}"

[Icons]
Name: "{group}\{cm:CalibrateShortcut}"; Filename: "{app}\ExerciseUnlockCalibrate.exe"; Parameters: "/lang {language}"; WorkingDir: "{app}"; IconFilename: "{app}\app.ico"

[InstallDelete]
; renamed to lock-user.ps1 in 1.1.1
Type: files; Name: "{app}\tools\lock-actor.ps1"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\data"
; written by the calibration (config.ini is only installed when missing,
; so an upgraded install may not have it in the uninstall log)
Type: files; Name: "{app}\config.ini"
Type: files; Name: "{app}\config.ini.bak"
Type: files; Name: "{app}\tools\dashboard.html"
Type: files; Name: "{app}\debug_frame.bmp"

[CustomMessages]
en.ExistingCaption=Exercise Unlock is already installed
uk.ExistingCaption=Exercise Unlock уже встановлено
en.ExistingDescription=Update it or remove it from this computer.
uk.ExistingDescription=Оновіть програму або видаліть її з цього комп'ютера.
en.ExistingInfo=Installed: version %1, child's account "%2".
uk.ExistingInfo=Встановлено: версія %1, обліковий запис дитини «%2».
en.ExistingInfoNoUser=Installed: version %1.
uk.ExistingInfoNoUser=Встановлено: версія %1.
en.UpdateButton=Update
uk.UpdateButton=Оновити
en.UpdateKeep=Install version %1. The child's account, its password and the calibration stay as they are - setup asks nothing more.
uk.UpdateKeep=Встановити версію %1. Обліковий запис дитини, його пароль і калібрування залишаться без змін - більше нічого не запитуватиметься.
en.UpdateAsk=Install version %1. The saved password is missing or no longer accepted by Windows, so setup will ask for the account and the password again.
uk.UpdateAsk=Встановити версію %1. Збережений пароль відсутній або Windows його більше не приймає, тож інсталятор ще раз запитає обліковий запис і пароль.
en.ReconfigureCheck=Change the account and password too
uk.ReconfigureCheck=Змінити обліковий запис і пароль
en.RemoveAllButton=Remove everything
uk.RemoveAllButton=Видалити все
en.RemoveAllText=Completely remove Exercise Unlock: the service, the sign-in tile, the saved password, settings, calibration and logs.
uk.RemoveAllText=Повністю видалити Exercise Unlock: службу, плитку входу, збережений пароль, налаштування, калібрування та журнали.
en.RemoveAllConfirm=Remove Exercise Unlock and all its data from this computer?
uk.RemoveAllConfirm=Видалити Exercise Unlock і всі його дані з цього комп'ютера?
en.RemoveAllDone=Exercise Unlock has been removed.
uk.RemoveAllDone=Exercise Unlock видалено.
en.RemoveAllReboot=Restart the computer to finish removing the sign-in tile.
uk.RemoveAllReboot=Перезавантажте комп'ютер, щоб завершити видалення плитки входу.
en.RemoveAllFailed=Removal did not complete (code %1). Try again, or use Settings - Apps - Installed apps.
uk.RemoveAllFailed=Видалення не завершилося (код %1). Спробуйте ще раз або скористайтеся «Параметри - Програми - Встановлені програми».
en.ReadyKept=(unchanged)
uk.ReadyKept=(без змін)
en.UserPageCaption=Child's account
uk.UserPageCaption=Обліковий запис дитини
en.UserPageDescription=Choose the Windows account that will be unlocked with exercises.
uk.UserPageDescription=Оберіть обліковий запис Windows, який розблоковуватиметься вправами.
en.UserPageHint=The Exercise Unlock tile will appear on the lock screen only for this account. All other accounts, including yours, keep signing in as usual.
uk.UserPageHint=Плитка Exercise Unlock з'явиться на екрані блокування лише для цього облікового запису. Усі інші облікові записи, зокрема ваш, входитимуть як звичайно.
en.UserMarkAdmin=administrator
uk.UserMarkAdmin=адміністратор
en.UserMarkYou=you
uk.UserMarkYou=це ви
en.UserManualPrompt=Could not list the local accounts. Type the account name:
uk.UserManualPrompt=Не вдалося отримати список облікових записів. Введіть ім'я облікового запису:
en.UserAdminWarning=This account is an administrator. A child with administrator rights can turn Exercise Unlock off - a standard account is recommended.
uk.UserAdminWarning=Цей обліковий запис має права адміністратора. Дитина з такими правами може вимкнути Exercise Unlock - рекомендуємо звичайний обліковий запис.
en.UserSelfWarning=This is your own account: you would also have to exercise to unlock it.
uk.UserSelfWarning=Це ваш власний обліковий запис: вам теж доведеться виконувати вправи, щоб його розблокувати.
en.UserNoneHint=No separate account for the child yet? Create a standard one in Windows Settings (Accounts - Other users), then run setup again.
uk.UserNoneHint=Ще немає окремого облікового запису для дитини? Створіть звичайний у налаштуваннях Windows (Облікові записи - Інші користувачі) і запустіть інсталяцію знову.
en.UserSelectRequired=Please choose an account.
uk.UserSelectRequired=Будь ласка, оберіть обліковий запис.
en.UserAdminConfirm=The account "%1" is an administrator, so the child could switch Exercise Unlock off.%n%nContinue with this account anyway?
uk.UserAdminConfirm=Обліковий запис «%1» має права адміністратора, тож дитина зможе вимкнути Exercise Unlock.%n%nУсе одно продовжити з цим обліковим записом?
en.UserSelfConfirm=You chose your own account "%1". Unlocking it will then require exercises too.%n%nContinue?
uk.UserSelfConfirm=Ви обрали власний обліковий запис «%1». Тоді для його розблокування теж знадобляться вправи.%n%nПродовжити?
en.PwdCaption=Account password
uk.PwdCaption=Пароль облікового запису
en.PwdDescription=Enter the current Windows password of the account "%1".
uk.PwdDescription=Введіть поточний пароль Windows облікового запису «%1».
en.PwdSubCaption=Exercise Unlock keeps this password encrypted (only the system can read it) and uses it to unlock the account once the exercises are done. Do not tell this password to the child.
uk.PwdSubCaption=Exercise Unlock зберігає цей пароль у зашифрованому вигляді (прочитати його може лише система) і використовує, щоб розблокувати обліковий запис після вправ. Не повідомляйте цей пароль дитині.
en.PwdPrompt=Password:
uk.PwdPrompt=Пароль:
en.PwdConfirmPrompt=Confirm password:
uk.PwdConfirmPrompt=Підтвердження пароля:
en.PwdEmpty=Enter the password of "%1".%n%nIf the account has no password, set one first in Windows Settings (Accounts) - otherwise the child can unlock the computer without any exercises.
uk.PwdEmpty=Введіть пароль облікового запису «%1».%n%nЯкщо пароля немає, спершу встановіть його в налаштуваннях Windows (Облікові записи) - інакше дитина зможе розблокувати комп'ютер без жодних вправ.
en.PwdMismatch=The passwords do not match.
uk.PwdMismatch=Паролі не збігаються.
en.PwdWrong=The password is not correct for the account "%1".
uk.PwdWrong=Неправильний пароль для облікового запису «%1».
en.PwdUnverified=Windows could not check the password (error %1). Make sure it is correct.%n%nContinue anyway?
uk.PwdUnverified=Windows не вдалося перевірити пароль (помилка %1). Переконайтеся, що він правильний.%n%nУсе одно продовжити?
en.TaskLockdown=Disable PIN, Windows Hello and picture password sign-in on this PC (for all accounts) and forbid the child to change the password. Recommended - otherwise the exercises can be bypassed.
uk.TaskLockdown=Вимкнути вхід через PIN-код, Windows Hello та графічний пароль на цьому ПК (для всіх облікових записів) і заборонити дитині змінювати пароль. Рекомендовано - інакше вправи можна обійти.
en.TaskCalibrate=Calibrate the exercise angles for the child now: live camera view and 4 photos, about 3 minutes. Needs the camera and the child.
uk.TaskCalibrate=Відкалібрувати кути вправ під дитину зараз: відео з камери й 4 фото, близько 3 хвилин. Потрібні камера й дитина.
en.StatusCalibrate=Calibrating the exercise angles (see the calibration window)...
uk.StatusCalibrate=Калібрування кутів вправ (див. вікно калібрування)...
en.CalibrateShortcut=Exercise Unlock calibration
uk.CalibrateShortcut=Калібрування Exercise Unlock
en.ReadyChildAccount=Child's account:
uk.ReadyChildAccount=Обліковий запис дитини:
en.StatusAccount=Saving the child's account settings...
uk.StatusAccount=Збереження налаштувань облікового запису дитини...
en.StatusVCRedist=Installing the Microsoft Visual C++ runtime...
uk.StatusVCRedist=Встановлення середовища виконання Microsoft Visual C++...
en.StatusService=Installing the Exercise Unlock service...
uk.StatusService=Встановлення служби Exercise Unlock...
en.StatusLockdown=Restricting other sign-in methods...
uk.StatusLockdown=Обмеження інших способів входу...
en.ServiceDescription=Counts exercises in front of the camera and unlocks the child's account (Exercise Unlock).
uk.ServiceDescription=Рахує вправи перед камерою та розблоковує обліковий запис дитини (Exercise Unlock).
en.ErrVault=Could not save the password (code %1). Please run setup again.%n%nDetails: %2
uk.ErrVault=Не вдалося зберегти пароль (код %1). Будь ласка, запустіть інсталяцію ще раз.%n%nПодробиці: %2
en.ErrHarden=Could not protect the service from being stopped (code %1).%n%nDetails: %2
uk.ErrHarden=Не вдалося захистити службу від зупинки (код %1).%n%nПодробиці: %2
en.ErrService=The service could not be started (code %1). Restart the computer; if it still fails, see service.log in the data folder of the installation directory.
uk.ErrService=Не вдалося запустити службу (код %1). Перезавантажте комп'ютер; якщо помилка повториться, перегляньте service.log у папці data каталогу встановлення.
en.ErrVCRedist=The Microsoft Visual C++ runtime could not be installed (code %1). Exercise Unlock will not work without it.%n%nInstall it from https://aka.ms/vc14/vc_redist.x64.exe and restart the computer.
uk.ErrVCRedist=Не вдалося встановити середовище виконання Microsoft Visual C++ (код %1). Без нього Exercise Unlock не працюватиме.%n%nВстановіть його з https://aka.ms/vc14/vc_redist.x64.exe і перезавантажте комп'ютер.
en.FinishedNote=To try it, lock the computer (Win+L) and choose the account "%1".%n%nSettings (minutes per exercise, allowed hours, camera): %2%n%nTo recalibrate the angles later: Start menu - Exercise Unlock calibration.
uk.FinishedNote=Щоб спробувати, заблокуйте комп'ютер (Win+L) і оберіть обліковий запис «%1».%n%nНалаштування (хвилини за вправу, дозволені години, камера): %2%n%nЩоб пізніше перекалібрувати кути: меню «Пуск» - Калібрування Exercise Unlock.
en.NoSilent=Exercise Unlock setup must be run interactively: it asks which account to configure and for that account's password.
uk.NoSilent=Інсталяцію Exercise Unlock потрібно запускати у звичайному режимі: вона запитує, який обліковий запис налаштувати, і його пароль.

[Code]
const
  SVC        = 'ExerciseUnlockService';
  REG_KEY    = 'SOFTWARE\ExerciseUnlock';
  PW_ENV     = 'EU_SETUP_VAULT_PASSWORD';
  LOGON32_LOGON_NETWORK    = 3;
  LOGON32_PROVIDER_DEFAULT = 0;
  ERROR_LOGON_FAILURE      = 1326;
  ERROR_SERVICE_DOES_NOT_EXIST = 1060;
  ERROR_SERVICE_ALREADY_RUNNING = 1056;
  ERROR_PRODUCT_VERSION        = 1638;   { vc_redist: a newer runtime is already installed }
  ERROR_SUCCESS_REBOOT_REQUIRED = 3010;
  VC_RUNTIME_KEY    = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  VC_REDIST_VERSION = '{#VCRedistVersion}';
  // Inno's own uninstall registration: <AppId>_is1
  UNINST_KEY = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E1F4C52-8B0A-4C7E-9A61-2E5B8D3F7A10}_is1';

var
  ExistingPage: TWizardPage;
  ReconfigureCheck: TNewCheckBox;
  Installed: Boolean;               { an earlier version is registered }
  Verified: Boolean;                { ...and its stored password still works }
  RemovedAll: Boolean;              { "Remove everything" ran: close without asking }
  InstalledUninstaller, InstalledVersion, InstalledDir: String;
  UserPage: TWizardPage;
  UserList: TNewListBox;
  UserManual: TNewEdit;
  UserWarn: TNewStaticText;
  PwdPage: TInputQueryWizardPage;
  UserNames: TArrayOfString;      { account names, index-aligned with UserList }
  UserAdmin: array of Boolean;
  ChildUser: String;
  ServiceStoppedBySetup: Boolean;   { PrepareToInstall stopped a running service }
  SetupConfigured: Boolean;         { ConfigureExerciseUnlock ran (it restarts the service) }
  VCRedistRestart: Boolean;         { vc_redist.x64.exe asked for a reboot }

function LogonUser(lpszUsername, lpszDomain, lpszPassword: String;
  dwLogonType, dwLogonProvider: Cardinal; var phToken: THandle): Boolean;
  external 'LogonUserW@advapi32.dll stdcall';
function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function SetEnvironmentVariable(lpName, lpValue: String): Boolean;
  external 'SetEnvironmentVariableW@kernel32.dll stdcall';
function ClearEnvironmentVariable(lpName: String; lpValue: Cardinal): Boolean;
  external 'SetEnvironmentVariableW@kernel32.dll stdcall';

{ ------------------------------------------------------------ helpers }

function SysExe(const Name: String): String;
begin
  Result := ExpandConstant('{sysnative}\') + Name;
end;

{ Runs a command hidden, waits, logs and returns its exit code (-1 if it
  could not start). Never pass secrets in Params: they end up in the log. }
function RunHidden(const Exe, Params: String): Integer;
var
  Code: Integer;
begin
  if not Exec(Exe, Params, '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  Log(Format('run: "%s" %s -> %d', [Exe, Params, Code]));
  Result := Code;
end;

{ Stop the service so its files can be replaced / removed. Builds before
  the camera watchdog could hang in a camera call and never finish
  stopping, which left the exe locked ("DeleteFile failed; code 5").
  Disable autostart first so the SCM doesn't restart it, ask it to stop,
  then kill whatever is left. }
procedure StopServiceHard;
begin
  RunHidden(SysExe('sc.exe'), 'config ' + SVC + ' start= disabled');
  RunHidden(SysExe('net.exe'), 'stop ' + SVC);
  RunHidden(SysExe('taskkill.exe'), '/F /IM ExerciseUnlockService.exe');
  Sleep(1000);
end;

{ The admin tool of the package being installed, for use before its files
  are copied: 1.1.0's own copy can't repair the service or verify the vault. }
function NewAdminTool: String;
begin
  ExtractTemporaryFile('ExerciseUnlockAdmin.exe');
  Result := ExpandConstant('{tmp}\ExerciseUnlockAdmin.exe');
end;

function RidOf(const Sid: String): String;
var
  P: Integer;
begin
  P := Length(Sid);
  while (P > 0) and (Sid[P] <> '-') do
    P := P - 1;
  Result := Copy(Sid, P + 1, MaxInt);
end;

function IsUserAdmin(Index: Integer): Boolean;
begin
  Result := (Index >= 0) and (Index < GetArrayLength(UserAdmin)) and UserAdmin[Index];
end;

{ ------------------------------------------------------------ account page }

procedure UpdateUserWarning;
var
  Idx: Integer;
begin
  Idx := UserList.ItemIndex;
  if IsUserAdmin(Idx) then
    UserWarn.Caption := CustomMessage('UserAdminWarning')
  else if (Idx >= 0) and (CompareText(UserNames[Idx], GetUserNameString) = 0) then
    UserWarn.Caption := CustomMessage('UserSelfWarning')
  else
    UserWarn.Caption := '';
end;

procedure UserListClick(Sender: TObject);
begin
  UpdateUserWarning;
end;

{ Local, enabled accounts via WMI, skipping the built-in Administrator /
  Guest / DefaultAccount / WDAGUtilityAccount (RIDs 500/501/503/504).
  Administrators are found through the BUILTIN\Administrators SID because
  the group name is localized. }
procedure LoadAccounts;
var
  Locator, Wmi, Coll, Item, Members, Member: Variant;
  I, N, Pick, Prev: Integer;
  Admins, Name, Full, Sid, Me, Line, PrevUser: String;
  Disabled: Boolean;
begin
  Admins := '|';
  N := 0;
  Me := GetUserNameString;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\CIMV2');

    Coll := Wmi.ExecQuery('SELECT * FROM Win32_Group WHERE LocalAccount = TRUE AND SID = ''S-1-5-32-544''');
    if Coll.Count > 0 then
    begin
      Item := Coll.ItemIndex(0);
      Members := Item.Associators_('Win32_GroupUser', 'Win32_UserAccount');
      for I := 0 to Members.Count - 1 do
      begin
        Member := Members.ItemIndex(I);
        Name := Member.Name;
        Admins := Admins + LowerCase(Name) + '|';
      end;
    end;

    Coll := Wmi.ExecQuery('SELECT Name, FullName, SID, Disabled FROM Win32_UserAccount WHERE LocalAccount = TRUE');
    for I := 0 to Coll.Count - 1 do
    begin
      Item := Coll.ItemIndex(I);
      Disabled := Item.Disabled;
      if Disabled then
        Continue;
      Sid := Item.SID;
      Sid := RidOf(Sid);
      if (Sid = '500') or (Sid = '501') or (Sid = '503') or (Sid = '504') then
        Continue;
      Name := Item.Name;
      Full := '';
      if not VarIsNull(Item.FullName) then
        Full := Item.FullName;

      SetArrayLength(UserNames, N + 1);
      SetArrayLength(UserAdmin, N + 1);
      UserNames[N] := Name;
      UserAdmin[N] := Pos('|' + LowerCase(Name) + '|', Admins) > 0;

      Line := Name;
      if (Full <> '') and (CompareText(Full, Name) <> 0) then
        Line := Line + '  -  ' + Full;
      if UserAdmin[N] then
        Line := Line + '   [' + CustomMessage('UserMarkAdmin') + ']';
      if CompareText(Name, Me) = 0 then
        Line := Line + '   (' + CustomMessage('UserMarkYou') + ')';
      UserList.Items.Add(Line);
      N := N + 1;
    end;
  except
    Log('account enumeration failed: ' + GetExceptionMessage);
  end;

  if N = 0 then
  begin
    { WMI unavailable: fall back to typing the name. }
    UserList.Visible := False;
    UserManual.Visible := True;
    UserWarn.Caption := CustomMessage('UserManualPrompt');
    Exit;
  end;

  { Preselect: the account chosen last time, else the first standard
    account that isn't the person running setup. }
  Pick := -1;
  Prev := -1;
  if RegQueryStringValue(HKLM, REG_KEY, 'ChildUser', PrevUser) then
    for I := 0 to N - 1 do
      if CompareText(UserNames[I], PrevUser) = 0 then
        Prev := I;
  if Prev >= 0 then
    Pick := Prev
  else
    for I := 0 to N - 1 do
      if (Pick < 0) and (not UserAdmin[I]) and (CompareText(UserNames[I], Me) <> 0) then
        Pick := I;
  UserList.ItemIndex := Pick;
  UpdateUserWarning;
end;

{ ------------------------------------------------------------ existing installation }

{ An earlier installation? Setup then opens on a page offering to update
  it or to remove everything. The update keeps the child's account, its
  password and the calibration as long as Windows still accepts the
  stored password - checked by the admin tool, so the password never
  reaches setup. }
procedure DetectInstalled;
begin
  Installed := RegQueryStringValue(HKLM, UNINST_KEY, 'UninstallString', InstalledUninstaller) and
               (InstalledUninstaller <> '');
  if not Installed then
    Exit;
  if not RegQueryStringValue(HKLM, UNINST_KEY, 'DisplayVersion', InstalledVersion) then
    InstalledVersion := '?';
  if RegQueryStringValue(HKLM, UNINST_KEY, 'InstallLocation', InstalledDir) then
    InstalledDir := RemoveBackslashUnlessRoot(InstalledDir);
  if RegQueryStringValue(HKLM, REG_KEY, 'ChildUser', ChildUser) and (ChildUser <> '') then
    Verified := RunHidden(NewAdminTool, 'verify --user "' + ChildUser + '"') = 0;
end;

{ Update without asking for the account, the password and the calibration. }
function KeepSettings: Boolean;
begin
  Result := Verified and not ReconfigureCheck.Checked;
end;

procedure UpdateClick(Sender: TObject);
begin
  WizardForm.NextButton.OnClick(WizardForm.NextButton);
end;

procedure SetExistingPageEnabled(Enabled: Boolean);
begin
  ExistingPage.Surface.Enabled := Enabled;
  WizardForm.NextButton.Enabled := Enabled;
  WizardForm.CancelButton.Enabled := Enabled;
end;

procedure RemoveAllClick(Sender: TObject);
var
  Code, Waited: Integer;
  Msg: String;
begin
  if MsgBox(CustomMessage('RemoveAllConfirm'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
    Exit;
  SetExistingPageEnabled(False);

  { 1.1.0's uninstaller can't delete its over-hardened service and may
    find the exe still running: repair and stop it first. }
  RunHidden(NewAdminTool, 'harden');
  StopServiceHard;
  Code := RunHidden(RemoveQuotes(InstalledUninstaller), '/SILENT /SUPPRESSMSGBOXES /NORESTART');
  { The uninstaller finishes in a copy of itself: wait (up to a minute)
    until it has removed its own registration. }
  Waited := 0;
  while RegKeyExists(HKLM, UNINST_KEY) and (Waited < 120) do
  begin
    Sleep(500);
    Waited := Waited + 1;
  end;

  if RegKeyExists(HKLM, UNINST_KEY) then
  begin
    { Not removed: put the service back and let the parent choose again. }
    RunHidden(SysExe('sc.exe'), 'config ' + SVC + ' start= auto');
    RunHidden(SysExe('sc.exe'), 'start ' + SVC);
    MsgBox(FmtMessage(CustomMessage('RemoveAllFailed'), [IntToStr(Code)]), mbError, MB_OK);
    SetExistingPageEnabled(True);
    Exit;
  end;

  if RunHidden(SysExe('sc.exe'), 'query ' + SVC) <> ERROR_SERVICE_DOES_NOT_EXIST then
    RunHidden(SysExe('sc.exe'), 'delete ' + SVC);
  { Older uninstallers leave the calibration's config files behind. }
  if InstalledDir <> '' then
  begin
    DeleteFile(InstalledDir + '\config.ini');
    DeleteFile(InstalledDir + '\config.ini.bak');
    DelTree(InstalledDir + '\data', True, True, True);
    RemoveDir(InstalledDir + '\tools');
    RemoveDir(InstalledDir);
  end;
  Msg := CustomMessage('RemoveAllDone');
  if FileExists(ExpandConstant('{sys}\ExerciseUnlockProvider.dll')) then
    Msg := Msg + #13#10#13#10 + CustomMessage('RemoveAllReboot');
  MsgBox(Msg, mbInformation, MB_OK);
  RemovedAll := True;
  { The wizard refuses to close while its Cancel button is disabled. }
  WizardForm.CancelButton.Enabled := True;
  WizardForm.Close;
end;

{ First page when an earlier version is installed: an "Update" button
  (what is kept, plus a "change the account too" box) and a "Remove
  everything" button (what gets deleted). }
procedure CreateExistingPage;
var
  Info, UpdateText, RemoveText: TNewStaticText;
  UpdateBtn, RemoveBtn: TNewButton;
  TextLeft, TextWidth: Integer;
begin
  ExistingPage := CreateCustomPage(wpWelcome, CustomMessage('ExistingCaption'), CustomMessage('ExistingDescription'));

  Info := TNewStaticText.Create(ExistingPage);
  Info.Parent := ExistingPage.Surface;
  Info.AutoSize := False;
  Info.WordWrap := True;
  Info.Width := ExistingPage.SurfaceWidth;
  Info.Height := ScaleY(30);
  if ChildUser <> '' then
    Info.Caption := FmtMessage(CustomMessage('ExistingInfo'), [InstalledVersion, ChildUser])
  else
    Info.Caption := FmtMessage(CustomMessage('ExistingInfoNoUser'), [InstalledVersion]);

  UpdateBtn := TNewButton.Create(ExistingPage);
  UpdateBtn.Parent := ExistingPage.Surface;
  UpdateBtn.Top := Info.Top + Info.Height + ScaleY(6);
  UpdateBtn.Width := ScaleX(140);
  UpdateBtn.Height := ScaleY(30);
  UpdateBtn.Caption := CustomMessage('UpdateButton');
  UpdateBtn.OnClick := @UpdateClick;

  TextLeft := UpdateBtn.Width + ScaleX(14);
  TextWidth := ExistingPage.SurfaceWidth - TextLeft;

  UpdateText := TNewStaticText.Create(ExistingPage);
  UpdateText.Parent := ExistingPage.Surface;
  UpdateText.AutoSize := False;
  UpdateText.WordWrap := True;
  UpdateText.Left := TextLeft;
  UpdateText.Top := UpdateBtn.Top;
  UpdateText.Width := TextWidth;
  UpdateText.Height := ScaleY(48);
  if Verified then
    UpdateText.Caption := FmtMessage(CustomMessage('UpdateKeep'), ['{#AppVersion}'])
  else
    UpdateText.Caption := FmtMessage(CustomMessage('UpdateAsk'), ['{#AppVersion}']);

  ReconfigureCheck := TNewCheckBox.Create(ExistingPage);
  ReconfigureCheck.Parent := ExistingPage.Surface;
  ReconfigureCheck.Left := TextLeft;
  ReconfigureCheck.Top := UpdateText.Top + UpdateText.Height + ScaleY(2);
  ReconfigureCheck.Width := TextWidth;
  ReconfigureCheck.Height := ScaleY(17);
  ReconfigureCheck.Caption := CustomMessage('ReconfigureCheck');
  ReconfigureCheck.Visible := Verified;   { otherwise the account is asked for anyway }

  RemoveBtn := TNewButton.Create(ExistingPage);
  RemoveBtn.Parent := ExistingPage.Surface;
  RemoveBtn.Top := ReconfigureCheck.Top + ReconfigureCheck.Height + ScaleY(22);
  RemoveBtn.Width := UpdateBtn.Width;
  RemoveBtn.Height := UpdateBtn.Height;
  RemoveBtn.Caption := CustomMessage('RemoveAllButton');
  RemoveBtn.OnClick := @RemoveAllClick;

  RemoveText := TNewStaticText.Create(ExistingPage);
  RemoveText.Parent := ExistingPage.Surface;
  RemoveText.AutoSize := False;
  RemoveText.WordWrap := True;
  RemoveText.Left := TextLeft;
  RemoveText.Top := RemoveBtn.Top;
  RemoveText.Width := TextWidth;
  RemoveText.Height := ScaleY(50);
  RemoveText.Caption := CustomMessage('RemoveAllText');
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  DetectInstalled;
  { A silent run can only update: a first install asks for the account
    and its password. }
  if WizardSilent and not Verified then
  begin
    SuppressibleMsgBox(CustomMessage('NoSilent'), mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
var
  Hint, NoneHint: TNewStaticText;
begin
  CreateExistingPage;
  UserPage := CreateCustomPage(wpLicense, CustomMessage('UserPageCaption'), CustomMessage('UserPageDescription'));

  Hint := TNewStaticText.Create(UserPage);
  Hint.Parent := UserPage.Surface;
  Hint.AutoSize := False;
  Hint.WordWrap := True;
  Hint.Width := UserPage.SurfaceWidth;
  Hint.Height := ScaleY(40);
  Hint.Caption := CustomMessage('UserPageHint');

  UserList := TNewListBox.Create(UserPage);
  UserList.Parent := UserPage.Surface;
  UserList.Top := Hint.Top + Hint.Height + ScaleY(4);
  UserList.Width := UserPage.SurfaceWidth;
  UserList.Height := ScaleY(96);
  UserList.OnClick := @UserListClick;

  UserManual := TNewEdit.Create(UserPage);
  UserManual.Parent := UserPage.Surface;
  UserManual.Top := UserList.Top + ScaleY(24);
  UserManual.Width := UserPage.SurfaceWidth div 2;
  UserManual.Visible := False;

  UserWarn := TNewStaticText.Create(UserPage);
  UserWarn.Parent := UserPage.Surface;
  UserWarn.AutoSize := False;
  UserWarn.WordWrap := True;
  UserWarn.Top := UserList.Top + UserList.Height + ScaleY(6);
  UserWarn.Width := UserPage.SurfaceWidth;
  UserWarn.Height := ScaleY(30);
  UserWarn.Font.Color := $0000B0;

  NoneHint := TNewStaticText.Create(UserPage);
  NoneHint.Parent := UserPage.Surface;
  NoneHint.AutoSize := False;
  NoneHint.WordWrap := True;
  NoneHint.Top := UserWarn.Top + UserWarn.Height + ScaleY(2);
  NoneHint.Width := UserPage.SurfaceWidth;
  NoneHint.Height := ScaleY(30);
  NoneHint.Caption := CustomMessage('UserNoneHint');
  NoneHint.Font.Color := $707070;

  LoadAccounts;
  if UserManual.Visible then
    UserWarn.Top := UserList.Top;   { prompt sits above the edit box }

  PwdPage := CreateInputQueryPage(UserPage.ID, CustomMessage('PwdCaption'), '', CustomMessage('PwdSubCaption'));
  PwdPage.Add(CustomMessage('PwdPrompt'), True);
  PwdPage.Add(CustomMessage('PwdConfirmPrompt'), True);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Idx, Err: Integer;
  Token: THandle;
  Pw: String;
begin
  Result := True;
  if CurPageID = UserPage.ID then
  begin
    if UserList.Visible then
    begin
      Idx := UserList.ItemIndex;
      if Idx < 0 then
      begin
        MsgBox(CustomMessage('UserSelectRequired'), mbError, MB_OK);
        Result := False;
        Exit;
      end;
      ChildUser := UserNames[Idx];
      if IsUserAdmin(Idx) and
         (MsgBox(FmtMessage(CustomMessage('UserAdminConfirm'), [ChildUser]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES) then
      begin
        Result := False;
        Exit;
      end;
    end
    else
    begin
      ChildUser := Trim(UserManual.Text);
      if ChildUser = '' then
      begin
        MsgBox(CustomMessage('UserSelectRequired'), mbError, MB_OK);
        Result := False;
        Exit;
      end;
    end;
    if (CompareText(ChildUser, GetUserNameString) = 0) and
       (MsgBox(FmtMessage(CustomMessage('UserSelfConfirm'), [ChildUser]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES) then
    begin
      Result := False;
      Exit;
    end;
    PwdPage.Description := FmtMessage(CustomMessage('PwdDescription'), [ChildUser]);
  end
  else if CurPageID = PwdPage.ID then
  begin
    Pw := PwdPage.Values[0];
    if Pw = '' then
    begin
      MsgBox(FmtMessage(CustomMessage('PwdEmpty'), [ChildUser]), mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if Pw <> PwdPage.Values[1] then
    begin
      MsgBox(CustomMessage('PwdMismatch'), mbError, MB_OK);
      Result := False;
      Exit;
    end;
    { Check the password with Windows now, so a typo can't leave the
      child with a tile that never unlocks. }
    if LogonUser(ChildUser, '.', Pw, LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, Token) then
      CloseHandle(Token)
    else
    begin
      Err := DLLGetLastError;
      Log(Format('LogonUser check for %s failed: %d', [ChildUser, Err]));
      if Err = ERROR_LOGON_FAILURE then
      begin
        MsgBox(FmtMessage(CustomMessage('PwdWrong'), [ChildUser]), mbError, MB_OK);
        Result := False;
        Exit;
      end;
      if MsgBox(FmtMessage(CustomMessage('PwdUnverified'), [IntToStr(Err)]), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) <> IDYES then
      begin
        Result := False;
        Exit;
      end;
    end;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  if PageID = ExistingPage.ID then
    Result := not Installed
  else
    Result := KeepSettings and
              ((PageID = UserPage.ID) or (PageID = PwdPage.ID) or (PageID = wpSelectTasks));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := CustomMessage('ReadyChildAccount') + NewLine + Space + ChildUser;
  if KeepSettings then
    Result := Result + ' ' + CustomMessage('ReadyKept');
  Result := Result + NewLine + NewLine + MemoDirInfo;
  if KeepSettings then
  begin
    { MemoTasksInfo still lists the remembered calibration task, which
      an update skips (see ConfigureExerciseUnlock). }
    if WizardIsTaskSelected('lockdown') then
      Result := Result + NewLine + NewLine + SetupMessage(msgReadyMemoTasks) + NewLine + Space + CustomMessage('TaskLockdown');
  end
  else if MemoTasksInfo <> '' then
    Result := Result + NewLine + NewLine + MemoTasksInfo;
end;

{ ------------------------------------------------------------ install }

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if RunHidden(SysExe('sc.exe'), 'query ' + SVC) <> ERROR_SERVICE_DOES_NOT_EXIST then
  begin
    { 1.1.0 hardened the service so tightly that Administrators could no
      longer reconfigure or delete it ("sc config" -> access denied).
      The new admin tool takes ownership and puts full control back. }
    RunHidden(NewAdminTool, 'harden');
    ServiceStoppedBySetup := True;
    StopServiceHard;
  end;
  Result := '';
end;

procedure DeinitializeSetup;
begin
  { Setup was cancelled after it stopped the service: put it back. }
  if ServiceStoppedBySetup and not SetupConfigured then
  begin
    RunHidden(SysExe('sc.exe'), 'config ' + SVC + ' start= auto');
    RunHidden(SysExe('sc.exe'), 'start ' + SVC);
  end;
end;

procedure SetStatus(const Msg: String);
begin
  WizardForm.StatusLabel.Caption := Msg;
end;

{ VC++ 2015-2022 x64 runtime version from its registry key in one
  registry view; 0 if it isn't installed there. }
function InstalledVCRuntime(RootKey: Integer): Int64;
var
  Installed, Major, Minor, Bld, Rbld: Cardinal;
begin
  Result := 0;
  if RegQueryDWordValue(RootKey, VC_RUNTIME_KEY, 'Installed', Installed) and (Installed = 1) and
     RegQueryDWordValue(RootKey, VC_RUNTIME_KEY, 'Major', Major) and
     RegQueryDWordValue(RootKey, VC_RUNTIME_KEY, 'Minor', Minor) and
     RegQueryDWordValue(RootKey, VC_RUNTIME_KEY, 'Bld', Bld) then
  begin
    if not RegQueryDWordValue(RootKey, VC_RUNTIME_KEY, 'Rbld', Rbld) then
      Rbld := 0;
    Result := PackVersionComponents(Major, Minor, Bld, Rbld);
  end;
end;

{ The runtime is missing or older than the bundled redistributable. Also
  the Check of vc_redist.x64.exe in the Files section, so it is only
  extracted when needed. The 64-bit registry view is authoritative for the
  x64 runtime; the 32-bit (WOW6432Node) copy is the fallback. }
function VCRedistNeeded: Boolean;
var
  Have, Need: Int64;
begin
  Have := InstalledVCRuntime(HKLM64);
  if Have = 0 then
    Have := InstalledVCRuntime(HKLM32);
  Result := not StrToVersion(VC_REDIST_VERSION, Need) or (ComparePackedVersion(Have, Need) < 0);
end;

{ 1638: a newer runtime is already there; 3010: installed, reboot needed. }
procedure InstallVCRedist;
var
  Code: Integer;
begin
  SetStatus(CustomMessage('StatusVCRedist'));
  Code := RunHidden(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart');
  if Code = ERROR_SUCCESS_REBOOT_REQUIRED then
    VCRedistRestart := True
  else if (Code <> 0) and (Code <> ERROR_PRODUCT_VERSION) then
  begin
    Log('VC++ runtime install failed, see %TEMP%\dd_vcredist_amd64_*.log');
    MsgBox(FmtMessage(CustomMessage('ErrVCRedist'), [IntToStr(Code)]), mbError, MB_OK);
  end;
end;

function NeedRestart: Boolean;
begin
  Result := VCRedistRestart;
end;

procedure RunLockUser(const User: String; Undo: Boolean);
var
  Params: String;
begin
  { -NonInteractive: -User is mandatory, never let a prompt hang setup. }
  Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\tools\lock-user.ps1') + '" -User "' + User + '"';
  if Undo then
    Params := Params + ' -Undo';
  RunHidden(SysExe('WindowsPowerShell\v1.0\powershell.exe'), Params);
end;

{ Earlier builds kept their data in C:\ProgramData\ExerciseUnlock and
  debug files in C:\Users\Public. Carry over the counters and the unlock
  history, then remove the old folder - including its copy of the vault. }
procedure MigrateLegacyData(const Data: String);
var
  Old: String;
begin
  Old := ExpandConstant('{commonappdata}\ExerciseUnlock');
  if DirExists(Old) then
  begin
    if FileExists(Old + '\state.dat') and not FileExists(Data + '\state.dat') then
      CopyFile(Old + '\state.dat', Data + '\state.dat', True);
    if FileExists(Old + '\activity.log') and not FileExists(Data + '\activity.log') then
      CopyFile(Old + '\activity.log', Data + '\activity.log', True);
    DelTree(Old, True, True, True);
    Log('migrated and removed ' + Old);
  end;
  DeleteFile('C:\Users\Public\exerciseunlock-provider.log');
  DeleteFile('C:\Users\Public\eu-service-trace.log');
  DeleteFile('C:\Users\Public\ExerciseUnlockWebcam.bmp');
end;

procedure ConfigureExerciseUnlock;
var
  Code: Integer;
  Data, Admin, SvcExe, PrevUser, BinPath: String;
  PrevLock: Cardinal;
  Lines: TArrayOfString;
  WantLock: Boolean;
begin
  SetupConfigured := True;   { from here on setup re-enables + starts the service itself }
  Data := ExpandConstant('{app}\data');
  Admin := ExpandConstant('{app}\ExerciseUnlockAdmin.exe');
  SvcExe := ExpandConstant('{app}\ExerciseUnlockService.exe');
  if not RegQueryStringValue(HKLM, REG_KEY, 'ChildUser', PrevUser) then
    PrevUser := '';
  if not RegQueryDWordValue(HKLM, REG_KEY, 'Lockdown', PrevLock) then
    PrevLock := 0;

  { 0. VC++ runtime first: the admin tool, the calibration, the service
       and the provider (loaded by LogonUI) all need it. }
  if VCRedistNeeded then
    InstallVCRedist
  else
    Log('VC++ runtime ' + VC_REDIST_VERSION + ' or newer already installed');

  { 1. Child account: tile filter + password vault. The data folder is
       locked to SYSTEM + Administrators: the vault is machine-scope DPAPI,
       so anyone who can read it could decrypt the password. }
  SetStatus(CustomMessage('StatusAccount'));
  ForceDirectories(Data);
  { Not icacls ... /T: on files it leaves an empty ACL that nobody can
    write (1.1.0 bug). secure-data also repairs files damaged by that. }
  RunHidden(Admin, 'secure-data');
  MigrateLegacyData(Data);
  SetArrayLength(Lines, 1);
  Lines[0] := ChildUser;
  if not SaveStringsToUTF8File(Data + '\users.allow', Lines, False) then
    Log('could not write ' + Data + '\users.allow');

  { An update keeps the stored password (verified at startup). }
  if not KeepSettings then
  begin
    if (PrevUser <> '') and (CompareText(PrevUser, ChildUser) <> 0) then
      RunHidden(Admin, 'remove --user "' + PrevUser + '"');
    SetEnvironmentVariable(PW_ENV, PwdPage.Values[0]);
    Code := RunHidden(Admin, 'set --domain . --user "' + ChildUser + '" --password-env ' + PW_ENV);
    ClearEnvironmentVariable(PW_ENV, 0);
    if Code <> 0 then
      MsgBox(FmtMessage(CustomMessage('ErrVault'), [IntToStr(Code), Data + '\admin.log']), mbError, MB_OK);
  end;

  { 2. Angle calibration while the camera is still free (the service
       isn't running yet). It writes the thresholds into config.ini,
       which the service reads when it starts below. }
  if WizardIsTaskSelected('calibrate') and not KeepSettings then
  begin
    SetStatus(CustomMessage('StatusCalibrate'));
    if not Exec(ExpandConstant('{app}\ExerciseUnlockCalibrate.exe'), '/setup /lang ' + ActiveLanguage,
                ExpandConstant('{app}'), SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
      Log('calibration could not start');
    Log(Format('calibration exit code %d', [Code]));
  end;

  { 3. Service: create or repoint, auto-restart on crash, lock it so
       non-admins can't stop it, start. }
  SetStatus(CustomMessage('StatusService'));
  BinPath := ' binPath= "\"' + SvcExe + '\"" start= auto DisplayName= "Exercise Unlock"';
  if RunHidden(SysExe('sc.exe'), 'query ' + SVC) = ERROR_SERVICE_DOES_NOT_EXIST then
    RunHidden(SysExe('sc.exe'), 'create ' + SVC + BinPath)
  else
    RunHidden(SysExe('sc.exe'), 'config ' + SVC + BinPath);
  RunHidden(SysExe('sc.exe'), 'description ' + SVC + ' "' + CustomMessage('ServiceDescription') + '"');
  RunHidden(SysExe('sc.exe'), 'failure ' + SVC + ' reset= 86400 actions= restart/5000/restart/5000/restart/30000');
  Code := RunHidden(Admin, 'harden');
  if Code <> 0 then
    MsgBox(FmtMessage(CustomMessage('ErrHarden'), [IntToStr(Code), Data + '\admin.log']), mbError, MB_OK);
  Code := RunHidden(SysExe('sc.exe'), 'start ' + SVC);
  if (Code <> 0) and (Code <> ERROR_SERVICE_ALREADY_RUNNING) then
    MsgBox(FmtMessage(CustomMessage('ErrService'), [IntToStr(Code)]), mbError, MB_OK);

  { 4. Optional lockdown of PIN / Hello / password change. Undo it for a
       previously configured account when it changes or is unticked. }
  WantLock := WizardIsTaskSelected('lockdown');
  if (PrevLock = 1) and ((not WantLock) or (CompareText(PrevUser, ChildUser) <> 0)) and (PrevUser <> '') then
    RunLockUser(PrevUser, True);
  if WantLock then
  begin
    SetStatus(CustomMessage('StatusLockdown'));
    RunLockUser(ChildUser, False);
  end;

  RegWriteStringValue(HKLM, REG_KEY, 'ChildUser', ChildUser);
  if WantLock then
    RegWriteDWordValue(HKLM, REG_KEY, 'Lockdown', 1)
  else
    RegWriteDWordValue(HKLM, REG_KEY, 'Lockdown', 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ConfigureExerciseUnlock;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if RemovedAll then
    Confirm := False;   { nothing left to cancel }
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = ExistingPage.ID then
    WizardForm.NextButton.Caption := CustomMessage('UpdateButton')
  else if CurPageID = wpFinished then
  begin
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      FmtMessage(CustomMessage('FinishedNote'), [ChildUser, ExpandConstant('{app}\config.ini')]);
    WizardForm.AdjustLabelHeight(WizardForm.FinishedLabel);
  end;
end;

{ ------------------------------------------------------------ uninstall }

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  User: String;
  Lock: Cardinal;
begin
  if CurUninstallStep = usUninstall then
  begin
    { Gives Administrators back delete rights on a 1.1.0 install. }
    RunHidden(ExpandConstant('{app}\ExerciseUnlockAdmin.exe'), 'harden');
    StopServiceHard;
    RunHidden(SysExe('sc.exe'), 'delete ' + SVC);
    if RegQueryDWordValue(HKLM, REG_KEY, 'Lockdown', Lock) and (Lock = 1) and
       RegQueryStringValue(HKLM, REG_KEY, 'ChildUser', User) then
      RunLockUser(User, True);
    { Wipe the stored password before the folder goes away. }
    RunHidden(ExpandConstant('{app}\ExerciseUnlockAdmin.exe'), 'clear');
  end;
end;
