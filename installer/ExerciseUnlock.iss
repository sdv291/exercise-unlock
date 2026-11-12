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
;   * optionally disables PIN / Windows Hello sign-in and time zone changes
;     by standard accounts (tools\lock-user.ps1)
;   * starts the calibration (ExerciseUnlockCalibrate.exe) when "Finish" is
;     clicked, if its box on the last page is ticked: live camera, 6 photos
;     and test reps -> thresholds written to config.ini
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
Source: "{#StageDir}\bin\ExerciseUnlockUpdate.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\bin\onnxruntime.dll";           DestDir: "{app}"; Flags: ignoreversion
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
; The language of the update window (ExerciseUnlockUpdate.exe).
Root: HKLM; Subkey: "SOFTWARE\ExerciseUnlock"; ValueType: string; ValueName: "Language"; ValueData: "{language}"
; Windows Error Reporting keeps the last 5 minidumps of a service crash in
; {app}\data\dumps (the service restarts itself: sc failure).
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\ExerciseUnlockService.exe"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\ExerciseUnlockService.exe"; ValueType: expandsz; ValueName: "DumpFolder"; ValueData: "{app}\data\dumps"
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\ExerciseUnlockService.exe"; ValueType: dword; ValueName: "DumpCount"; ValueData: "5"
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\ExerciseUnlockService.exe"; ValueType: dword; ValueName: "DumpType"; ValueData: "1"

[Tasks]
Name: "lockdown"; Description: "{cm:TaskLockdown}"

[Icons]
Name: "{group}\{cm:CalibrateShortcut}"; Filename: "{app}\ExerciseUnlockCalibrate.exe"; Parameters: "/lang {language}"; WorkingDir: "{app}"; IconFilename: "{app}\app.ico"
; The same wizard with the phone camera picked (its QR code); no webcam is opened.
Name: "{group}\{cm:CalibratePhoneShortcut}"; Filename: "{app}\ExerciseUnlockCalibrate.exe"; Parameters: "/phone /lang {language}"; WorkingDir: "{app}"; IconFilename: "{app}\app.ico"

[Run]
; The calibration starts once "Finish" is clicked (ticked on a new install,
; offered unticked on an update). The service is running by then: on the
; desktop it leaves the camera alone ([camera] lock_screen_only), and
; without /setup the calibrator restarts it after saving.
; runascurrentuser: a postinstall entry otherwise runs as the user who
; started setup, not elevated, and the calibrator needs administrator
; rights (its manifest) - CreateProcess failed with 740 (1.4.1 and older).
Filename: "{app}\ExerciseUnlockCalibrate.exe"; Parameters: "/lang {language}"; WorkingDir: "{app}"; Description: "{cm:RunCalibrate}"; Flags: postinstall nowait skipifsilent runascurrentuser; Check: not KeepSettings
Filename: "{app}\ExerciseUnlockCalibrate.exe"; Parameters: "/lang {language}"; WorkingDir: "{app}"; Description: "{cm:RunCalibrate}"; Flags: postinstall nowait skipifsilent unchecked runascurrentuser; Check: KeepSettings

[InstallDelete]
; renamed to lock-user.ps1 in 1.1.1
Type: files; Name: "{app}\tools\lock-actor.ps1"
; no longer part of the sound set since 1.3.3
Type: files; Name: "{app}\data\sounds\amazing.wav"
Type: files; Name: "{app}\data\sounds\great.wav"
Type: files; Name: "{app}\data\sounds\insane.wav"
Type: files; Name: "{app}\data\sounds\legendary.wav"
Type: files; Name: "{app}\data\sounds\perfect.wav"

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
en.ExistingInfo=Installed: version %1, user's account "%2".
uk.ExistingInfo=Встановлено: версія %1, обліковий запис користувача «%2».
en.ExistingInfoNoUser=Installed: version %1.
uk.ExistingInfoNoUser=Встановлено: версія %1.
en.UpdateButton=Update
uk.UpdateButton=Оновити
en.UpdateKeep=Install version %1. The user's account, its password and the calibration stay as they are - setup asks nothing more.
uk.UpdateKeep=Встановити версію %1. Обліковий запис користувача, його пароль і калібрування залишаться без змін - більше нічого не запитуватиметься.
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
en.UserPageCaption=User's account
uk.UserPageCaption=Обліковий запис користувача
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
en.UserAdminWarning=This account is an administrator. A user with administrator rights can turn Exercise Unlock off - a standard account is recommended.
uk.UserAdminWarning=Цей обліковий запис має права адміністратора. Користувач із такими правами може вимкнути Exercise Unlock - рекомендуємо звичайний обліковий запис.
en.UserAdminOnly=To remove these rights, the PC needs another administrator account.
uk.UserAdminOnly=Щоб прибрати ці права, на ПК має бути ще один обліковий запис адміністратора.
en.MakeStandardCheck=Remove administrator rights from this account (recommended)
uk.MakeStandardCheck=Прибрати в цього облікового запису права адміністратора (рекомендовано)
en.ReadyMakeStandard=administrator rights will be removed
uk.ReadyMakeStandard=права адміністратора буде прибрано
en.ErrMakeStandard=Could not remove administrator rights from the account "%1" (code %2). Do it in Windows Settings: Accounts - Other users - Change account type - Standard User.
uk.ErrMakeStandard=Не вдалося прибрати права адміністратора в облікового запису «%1» (код %2). Зробіть це в Параметрах Windows: Облікові записи - Інші користувачі - Змінити тип облікового запису - Звичайний користувач.
en.ErrMakeStandardOnly=The account "%1" kept its administrator rights: there is no other enabled administrator on this PC.
uk.ErrMakeStandardOnly=Обліковий запис «%1» залишився адміністратором: інших увімкнених адміністраторів на цьому ПК немає.
en.UserSelfWarning=This is your own account: you would also have to exercise to unlock it.
uk.UserSelfWarning=Це ваш власний обліковий запис: вам теж доведеться виконувати вправи, щоб його розблокувати.
en.UserNoneHint=No separate account for the user yet? Create a standard one in Windows Settings (Accounts - Other users), then run setup again.
uk.UserNoneHint=Ще немає окремого облікового запису для користувача? Створіть звичайний у налаштуваннях Windows (Облікові записи - Інші користувачі) і запустіть інсталяцію знову.
en.CreateUserHint=This PC has only one account. If the user has none of their own, create a standard account for them - setup will then start again with it chosen.
uk.CreateUserHint=На цьому ПК лише один обліковий запис. Якщо в користувача немає власного, створіть для нього звичайний обліковий запис - інсталяція запуститься знову, і його буде вибрано.
en.CreateUserButton=Create an account
uk.CreateUserButton=Створити обліковий запис
en.CreateUserCaption=New user account
uk.CreateUserCaption=Новий обліковий запис користувача
en.CreateUserNote=A standard Windows account (not an administrator) whose password never expires. Exercise Unlock signs in with this password after the exercises - do not tell it to the user.
uk.CreateUserNote=Звичайний обліковий запис Windows (не адміністратор), пароль якого не має терміну дії. Exercise Unlock входить із цим паролем після вправ - не повідомляйте його користувачеві.
en.CreateUserName=Account name:
uk.CreateUserName=Ім'я облікового запису:
en.CreateUserOK=Create
uk.CreateUserOK=Створити
en.CreateUserBadName=Enter a name of up to 20 characters, without " / \ [ ] : ; | = , + * ? < > @
uk.CreateUserBadName=Введіть ім'я до 20 символів, без " / \ [ ] : ; | = , + * ? < > @
en.CreateUserNoPwd=Enter a password: without one the user could sign in without any exercises.
uk.CreateUserNoPwd=Введіть пароль: без нього користувач зможе входити без жодних вправ.
en.CreateUserExists=An account named "%1" already exists. Choose another name.
uk.CreateUserExists=Обліковий запис «%1» уже існує. Оберіть інше ім'я.
en.CreateUserPolicy=Windows did not accept this password: it is too short or too simple for this computer's password rules.
uk.CreateUserPolicy=Windows не прийняла цей пароль: він закороткий або надто простий для правил паролів цього комп'ютера.
en.CreateUserFailed=The account could not be created (code %1).
uk.CreateUserFailed=Не вдалося створити обліковий запис (код %1).
en.CreateUserDone=The account "%1" has been created. Setup will now start again so you can choose it.
uk.CreateUserDone=Обліковий запис «%1» створено. Зараз інсталяція запуститься знову, щоб ви могли його обрати.
en.CreateUserNoRestart=The account "%1" has been created, but setup could not start again (code %2). Close setup and run it once more.
uk.CreateUserNoRestart=Обліковий запис «%1» створено, але інсталяцію не вдалося запустити знову (код %2). Закрийте інсталятор і запустіть його ще раз.
en.UserSelectRequired=Please choose an account.
uk.UserSelectRequired=Будь ласка, оберіть обліковий запис.
en.UserAdminConfirm=The account "%1" is an administrator, so the user could switch Exercise Unlock off.%n%nContinue with this account anyway?
uk.UserAdminConfirm=Обліковий запис «%1» має права адміністратора, тож користувач зможе вимкнути Exercise Unlock.%n%nУсе одно продовжити з цим обліковим записом?
en.UserSelfConfirm=You chose your own account "%1". Unlocking it will then require exercises too.%n%nContinue?
uk.UserSelfConfirm=Ви обрали власний обліковий запис «%1». Тоді для його розблокування теж знадобляться вправи.%n%nПродовжити?
en.PwdCaption=Account password
uk.PwdCaption=Пароль облікового запису
en.PwdDescription=Enter the current Windows password of the account "%1".
uk.PwdDescription=Введіть поточний пароль Windows облікового запису «%1».
en.PwdSubCaption=Exercise Unlock keeps this password encrypted (only the system can read it) and uses it to unlock the account once the exercises are done. Do not tell this password to the user.
uk.PwdSubCaption=Exercise Unlock зберігає цей пароль у зашифрованому вигляді (прочитати його може лише система) і використовує, щоб розблокувати обліковий запис після вправ. Не повідомляйте цей пароль користувачеві.
en.PwdPrompt=Password:
uk.PwdPrompt=Пароль:
en.PwdConfirmPrompt=Confirm password:
uk.PwdConfirmPrompt=Підтвердження пароля:
en.PwdEmpty=Enter the password of "%1".%n%nIf the account has no password, set one first in Windows Settings (Accounts) - otherwise the user can unlock the computer without any exercises.
uk.PwdEmpty=Введіть пароль облікового запису «%1».%n%nЯкщо пароля немає, спершу встановіть його в налаштуваннях Windows (Облікові записи) - інакше користувач зможе розблокувати комп'ютер без жодних вправ.
en.PwdMismatch=The passwords do not match.
uk.PwdMismatch=Паролі не збігаються.
en.PwdWrong=The password is not correct for the account "%1".
uk.PwdWrong=Неправильний пароль для облікового запису «%1».
en.PwdUnverified=Windows could not check the password (error %1). Make sure it is correct.%n%nContinue anyway?
uk.PwdUnverified=Windows не вдалося перевірити пароль (помилка %1). Переконайтеся, що він правильний.%n%nУсе одно продовжити?
en.TaskLockdown=Disable PIN, Windows Hello and picture password sign-in on this PC (for all accounts), forbid the user to change the password and standard accounts to change the time zone (it would shift the allowed hours). Recommended - otherwise the exercises can be bypassed.
uk.TaskLockdown=Вимкнути вхід через PIN-код, Windows Hello та графічний пароль на цьому ПК (для всіх облікових записів), заборонити користувачеві змінювати пароль, а звичайним обліковим записам - часовий пояс (він зсуває дозволені години). Рекомендовано - інакше вправи можна обійти.
en.RunCalibrate=Start the calibration: tune the exercises to the user with the camera (about 5 minutes, the user should be there)
uk.RunCalibrate=Запустити калібрування: налаштувати вправи під користувача з камерою (близько 5 хвилин, користувач має бути поруч)
en.CalibrateShortcut=Exercise Unlock calibration
uk.CalibrateShortcut=Калібрування Exercise Unlock
en.CalibratePhoneShortcut=Exercise Unlock phone calibration
uk.CalibratePhoneShortcut=Калібрування телефона Exercise Unlock
en.ReadyChildAccount=User's account:
uk.ReadyChildAccount=Обліковий запис користувача:
en.StatusAccount=Saving the user's account settings...
uk.StatusAccount=Збереження налаштувань облікового запису користувача...
en.StatusVCRedist=Installing the Microsoft Visual C++ runtime...
uk.StatusVCRedist=Встановлення середовища виконання Microsoft Visual C++...
en.StatusService=Installing the Exercise Unlock service...
uk.StatusService=Встановлення служби Exercise Unlock...
en.StatusLockdown=Restricting other sign-in methods...
uk.StatusLockdown=Обмеження інших способів входу...
en.ServiceDescription=Counts exercises in front of the camera and unlocks the user's account (Exercise Unlock).
uk.ServiceDescription=Рахує вправи перед камерою та розблоковує обліковий запис користувача (Exercise Unlock).
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
  NEW_PW_ENV = 'EU_SETUP_NEW_ACCOUNT_PASSWORD';
  NERR_USER_EXISTS        = 2224;
  NERR_PASSWORD_TOO_SHORT = 2245;   { also: too simple, too recent }
  MANIFEST_URL = 'https://exercise-unlock.sdv291.com/download/version.txt';
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
  NoneHint: TNewStaticText;
  CreateUserBtn: TNewButton;
  MakeStandardCheck: TNewCheckBox;  { take the chosen administrator out of Administrators }
  MakeStandard: Boolean;            { ...ticked when "Next" was pressed }
  WarnIdx: Integer;                 { the pick UpdateUserWarning last saw }
  RestartingSetup: Boolean;         { "Create an account" started setup again: close without asking }
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

{ Another listed (enabled) account is an administrator too. }
function OtherAdminListed(Index: Integer): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 0 to GetArrayLength(UserAdmin) - 1 do
    if (I <> Index) and UserAdmin[I] then
      Result := True;
end;

{ An administrator picked: the warning, and the box that takes the rights
  away - only while another administrator stays, so the PC keeps one.
  A new pick gets the box ticked, unless it is your own account. }
procedure UpdateUserWarning;
var
  Idx: Integer;
  IsSelf: Boolean;
begin
  Idx := UserList.ItemIndex;
  IsSelf := (Idx >= 0) and (CompareText(UserNames[Idx], GetUserNameString) = 0);
  MakeStandardCheck.Visible := IsUserAdmin(Idx) and OtherAdminListed(Idx);
  if Idx <> WarnIdx then
    MakeStandardCheck.Checked := not IsSelf;
  WarnIdx := Idx;
  if IsUserAdmin(Idx) and not MakeStandardCheck.Visible then
    UserWarn.Caption := CustomMessage('UserAdminWarning') + ' ' + CustomMessage('UserAdminOnly')
  else if IsUserAdmin(Idx) then
    UserWarn.Caption := CustomMessage('UserAdminWarning')
  else if IsSelf then
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

  { Preselect: the account just made with "Create an account" (/CHILD=),
    else the one chosen last time, else the first standard account that
    isn't the person running setup. }
  Pick := -1;
  Prev := -1;
  PrevUser := ExpandConstant('{param:CHILD|}');
  if PrevUser = '' then
    if not RegQueryStringValue(HKLM, REG_KEY, 'ChildUser', PrevUser) then
      PrevUser := '';
  if PrevUser <> '' then
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

{ A name Windows takes for a local account: up to 20 characters, none of
  these, not ending with a dot. }
function GoodAccountName(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := (Name <> '') and (Length(Name) <= 20) and (Name[Length(Name)] <> '.');
  for I := 1 to Length(Name) do
    if Pos(Name[I], '"/\[]:;|=,+*?<>@') > 0 then
      Result := False;
end;

function AddFormLabel(Form: TSetupForm; const Caption: String; Top, Height: Integer): TNewStaticText;
begin
  Result := TNewStaticText.Create(Form);
  Result.Parent := Form;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Left := ScaleX(12);
  Result.Top := Top;
  Result.Width := Form.ClientWidth - ScaleX(24);
  Result.Height := Height;
  Result.Caption := Caption;
end;

{ A yes/no question with Setup's own "Yes" / "No" (ButtonYes / ButtonNo of
  the chosen language). MsgBox is Windows' message box, whose buttons
  follow the language of Windows: "Yes" / "No" on an English Windows under
  a Ukrainian setup. "No" is the default; Esc and the window's X say no. }
function AskYesNo(const Title, Text: String): Boolean;
var
  Form: TSetupForm;
  Icon: TBitmapImage;
  Msg: TNewStaticText;
  YesButton, NoButton: TNewButton;
  W, Bottom: Integer;
begin
  Form := CreateCustomForm(ScaleX(420), ScaleY(150), True, True);
  try
    Form.ClientWidth := ScaleX(420);   { see CreateUserClick }
    Form.Caption := Title;

    Icon := TBitmapImage.Create(Form);
    Icon.Parent := Form;
    Icon.Left := ScaleX(16);
    Icon.Top := ScaleY(16);
    Icon.Width := ScaleX(32);
    Icon.Height := ScaleY(32);
    InitializeBitmapImageFromStockIcon(Icon, SIID_HELP, clNone, []);

    Msg := TNewStaticText.Create(Form);
    Msg.Parent := Form;
    Msg.Left := Icon.Left + Icon.Width + ScaleX(14);
    Msg.Top := ScaleY(18);
    Msg.Width := Form.ClientWidth - Msg.Left - ScaleX(16);
    Msg.WordWrap := True;
    Msg.AutoSize := True;
    Msg.Caption := Text;

    Bottom := Msg.Top + Msg.Height;
    if Icon.Top + Icon.Height > Bottom then
      Bottom := Icon.Top + Icon.Height;
    Form.ClientHeight := Bottom + ScaleY(20 + 23 + 14);

    YesButton := TNewButton.Create(Form);
    YesButton.Parent := Form;
    YesButton.Caption := SetupMessage(msgButtonYes);
    YesButton.ModalResult := mrYes;
    NoButton := TNewButton.Create(Form);
    NoButton.Parent := Form;
    NoButton.Caption := SetupMessage(msgButtonNo);
    NoButton.ModalResult := mrNo;
    NoButton.Cancel := True;
    NoButton.Default := True;

    W := Form.CalculateButtonWidth([YesButton.Caption, NoButton.Caption]);
    YesButton.Width := W;
    NoButton.Width := W;
    YesButton.Height := ScaleY(23);
    NoButton.Height := ScaleY(23);
    NoButton.Top := Form.ClientHeight - ScaleY(23 + 14);
    YesButton.Top := NoButton.Top;
    NoButton.Left := Form.ClientWidth - ScaleX(14) - W;
    YesButton.Left := NoButton.Left - ScaleX(8) - W;

    Form.ActiveControl := NoButton;
    Form.FlipAndCenterIfNeeded(True, WizardForm, False);
    Result := Form.ShowModal = mrYes;
  finally
    Form.Free;
  end;
end;

{ "Create an account", offered when the PC lists a single account (the
  parent's own, typically): a standard local account for the child, made
  by the admin tool with a password that never expires. The password goes
  to it in the environment - never on the command line or into the setup
  log. Then setup starts again with the new account picked (/CHILD=), and
  this one closes. }
procedure CreateUserClick(Sender: TObject);
var
  Form: TSetupForm;
  NameEdit: TNewEdit;
  PwdEdit, ConfirmEdit: TPasswordEdit;
  OKButton, CancelButton: TNewButton;
  Name, Problem: String;
  Y, W, Code: Integer;
  Created: Boolean;
begin
  Form := CreateCustomForm(ScaleX(440), ScaleY(286), True, True);
  try
    { CreateCustomForm scales the size once more for the screen; the
      controls below are placed with ScaleX/ScaleY only. }
    Form.ClientWidth := ScaleX(440);
    Form.ClientHeight := ScaleY(286);
    Form.Caption := CustomMessage('CreateUserCaption');
    Y := ScaleY(12);
    AddFormLabel(Form, CustomMessage('CreateUserNote'), Y, ScaleY(60));
    Y := Y + ScaleY(68);

    AddFormLabel(Form, CustomMessage('CreateUserName'), Y, ScaleY(16));
    NameEdit := TNewEdit.Create(Form);
    NameEdit.Parent := Form;
    NameEdit.Left := ScaleX(12);
    NameEdit.Top := Y + ScaleY(18);
    NameEdit.Width := Form.ClientWidth - ScaleX(24);
    NameEdit.MaxLength := 20;
    Y := Y + ScaleY(50);

    AddFormLabel(Form, CustomMessage('PwdPrompt'), Y, ScaleY(16));
    PwdEdit := TPasswordEdit.Create(Form);
    PwdEdit.Parent := Form;
    PwdEdit.Left := ScaleX(12);
    PwdEdit.Top := Y + ScaleY(18);
    PwdEdit.Width := Form.ClientWidth - ScaleX(24);
    Y := Y + ScaleY(50);

    AddFormLabel(Form, CustomMessage('PwdConfirmPrompt'), Y, ScaleY(16));
    ConfirmEdit := TPasswordEdit.Create(Form);
    ConfirmEdit.Parent := Form;
    ConfirmEdit.Left := ScaleX(12);
    ConfirmEdit.Top := Y + ScaleY(18);
    ConfirmEdit.Width := Form.ClientWidth - ScaleX(24);

    OKButton := TNewButton.Create(Form);
    OKButton.Parent := Form;
    OKButton.Caption := CustomMessage('CreateUserOK');
    OKButton.Top := Form.ClientHeight - ScaleY(23 + 12);
    OKButton.Height := ScaleY(23);
    OKButton.ModalResult := mrOk;
    OKButton.Default := True;

    CancelButton := TNewButton.Create(Form);
    CancelButton.Parent := Form;
    CancelButton.Caption := SetupMessage(msgButtonCancel);
    CancelButton.Top := OKButton.Top;
    CancelButton.Height := OKButton.Height;
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;

    W := Form.CalculateButtonWidth([OKButton.Caption, CancelButton.Caption]);
    OKButton.Width := W;
    CancelButton.Width := W;
    CancelButton.Left := Form.ClientWidth - ScaleX(12) - W;
    OKButton.Left := CancelButton.Left - ScaleX(6) - W;

    Form.ActiveControl := NameEdit;
    Form.FlipAndCenterIfNeeded(True, WizardForm, False);

    { Asked again until Windows creates the account or "Cancel". }
    Created := False;
    while (not Created) and (Form.ShowModal = mrOk) do
    begin
      Name := Trim(NameEdit.Text);
      Problem := '';
      if not GoodAccountName(Name) then
        Problem := CustomMessage('CreateUserBadName')
      else if PwdEdit.Text = '' then
        Problem := CustomMessage('CreateUserNoPwd')
      else if PwdEdit.Text <> ConfirmEdit.Text then
        Problem := CustomMessage('PwdMismatch');
      if Problem = '' then
      begin
        SetEnvironmentVariable(NEW_PW_ENV, PwdEdit.Text);
        Code := RunHidden(NewAdminTool, 'create-user --user "' + Name + '" --password-env ' + NEW_PW_ENV);
        ClearEnvironmentVariable(NEW_PW_ENV, 0);
        if Code = 0 then
          Created := True
        else if Code = NERR_USER_EXISTS then
          Problem := FmtMessage(CustomMessage('CreateUserExists'), [Name])
        else if Code = NERR_PASSWORD_TOO_SHORT then
          Problem := CustomMessage('CreateUserPolicy')
        else
          Problem := FmtMessage(CustomMessage('CreateUserFailed'), [IntToStr(Code)]);
      end;
      if not Created then
        MsgBox(Problem, mbError, MB_OK);
    end;
  finally
    Form.Free;
  end;

  if Created then
  begin
    MsgBox(FmtMessage(CustomMessage('CreateUserDone'), [Name]), mbInformation, MB_OK);
    if ShellExec('', ExpandConstant('{srcexe}'), '/LANG=' + ActiveLanguage + ' "/CHILD=' + Name + '"',
                 '', SW_SHOWNORMAL, ewNoWait, Code) then
    begin
      RestartingSetup := True;
      { The wizard refuses to close while its Cancel button is disabled. }
      WizardForm.CancelButton.Enabled := True;
      WizardForm.Close;
    end
    else
      MsgBox(FmtMessage(CustomMessage('CreateUserNoRestart'), [Name, IntToStr(Code)]), mbError, MB_OK);
  end;
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
  if not AskYesNo(SetupMessage(msgSetupAppTitle), CustomMessage('RemoveAllConfirm')) then
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
var
  Code: Integer;
begin
  Result := True;
  DetectInstalled;
  { A silent run can only update: a first install asks for the account
    and its password. The app's update window (/FROMAPP=1) gets the
    normal wizard instead, already elevated. }
  if WizardSilent and not Verified then
  begin
    if Installed and (ExpandConstant('{param:FROMAPP|0}') = '1') then
      ShellExec('', ExpandConstant('{srcexe}'), '/LANG=' + ActiveLanguage, '', SW_SHOWNORMAL, ewNoWait, Code)
    else
      SuppressibleMsgBox(CustomMessage('NoSilent'), mbError, MB_OK, IDOK);
    Result := False;
  end;
end;

procedure InitializeWizard;
var
  Hint: TNewStaticText;
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
  UserWarn.Height := ScaleY(44);
  UserWarn.Font.Color := $0000B0;

  MakeStandardCheck := TNewCheckBox.Create(UserPage);
  MakeStandardCheck.Parent := UserPage.Surface;
  MakeStandardCheck.Top := UserWarn.Top + UserWarn.Height + ScaleY(2);
  MakeStandardCheck.Width := UserPage.SurfaceWidth;
  MakeStandardCheck.Height := ScaleY(17);
  MakeStandardCheck.Caption := CustomMessage('MakeStandardCheck');
  MakeStandardCheck.Visible := False;
  WarnIdx := -2;

  NoneHint := TNewStaticText.Create(UserPage);
  NoneHint.Parent := UserPage.Surface;
  NoneHint.AutoSize := False;
  NoneHint.WordWrap := True;
  NoneHint.Top := MakeStandardCheck.Top + MakeStandardCheck.Height + ScaleY(8);
  NoneHint.Width := UserPage.SurfaceWidth;
  NoneHint.Height := ScaleY(30);
  NoneHint.Caption := CustomMessage('UserNoneHint');
  NoneHint.Font.Color := $707070;

  CreateUserBtn := TNewButton.Create(UserPage);
  CreateUserBtn.Parent := UserPage.Surface;
  CreateUserBtn.Caption := CustomMessage('CreateUserButton');
  CreateUserBtn.Width := WizardForm.CalculateButtonWidth([CreateUserBtn.Caption]);
  CreateUserBtn.Height := ScaleY(23);
  CreateUserBtn.OnClick := @CreateUserClick;
  CreateUserBtn.Visible := False;

  LoadAccounts;
  if UserManual.Visible then
    UserWarn.Top := UserList.Top;   { prompt sits above the edit box }

  { A single account - most likely the parent's own: offer to create one
    for the child right here. A one-line list leaves room for the button. }
  if UserList.Visible and (UserList.Items.Count = 1) then
  begin
    UserList.Height := ScaleY(40);
    UserWarn.Top := UserList.Top + UserList.Height + ScaleY(6);
    NoneHint.Top := UserWarn.Top + UserWarn.Height + ScaleY(4);
    NoneHint.Height := ScaleY(44);
    NoneHint.Caption := CustomMessage('CreateUserHint');
    NoneHint.Font.Color := clWindowText;
    CreateUserBtn.Top := NoneHint.Top + NoneHint.Height + ScaleY(4);
    CreateUserBtn.Visible := True;
  end;

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
      MakeStandard := MakeStandardCheck.Visible and MakeStandardCheck.Checked;
      if IsUserAdmin(Idx) and not MakeStandard and
         not AskYesNo(SetupMessage(msgSetupAppTitle), FmtMessage(CustomMessage('UserAdminConfirm'), [ChildUser])) then
      begin
        Result := False;
        Exit;
      end;
    end
    else
    begin
      ChildUser := Trim(UserManual.Text);
      MakeStandard := False;
      if ChildUser = '' then
      begin
        MsgBox(CustomMessage('UserSelectRequired'), mbError, MB_OK);
        Result := False;
        Exit;
      end;
    end;
    if (CompareText(ChildUser, GetUserNameString) = 0) and
       not AskYesNo(SetupMessage(msgSetupAppTitle), FmtMessage(CustomMessage('UserSelfConfirm'), [ChildUser])) then
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
      if not AskYesNo(SetupMessage(msgSetupAppTitle), FmtMessage(CustomMessage('PwdUnverified'), [IntToStr(Err)])) then
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
  else if PageID = wpLicense then
    { Started again by "Create an account": the licence was accepted then. }
    Result := ExpandConstant('{param:CHILD|}') <> ''
  else
    Result := KeepSettings and
              ((PageID = UserPage.ID) or (PageID = PwdPage.ID) or (PageID = wpSelectTasks));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := CustomMessage('ReadyChildAccount') + NewLine + Space + ChildUser;
  if KeepSettings then
    Result := Result + ' ' + CustomMessage('ReadyKept')
  else if MakeStandard then
    Result := Result + ' (' + CustomMessage('ReadyMakeStandard') + ')';
  Result := Result + NewLine + NewLine + MemoDirInfo;
  if KeepSettings then
  begin
    { The tasks page is skipped on an update: list what it remembered. }
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

{ Windows' default policy for the "Sign-in options" page. The lockdown sets
  it to 0, and lock-user.ps1 -Undo of 1.4.0 and older deleted it instead of
  putting 1 back: the page stayed "managed by your organization" for every
  account after the uninstall. Put back on install and uninstall. }
procedure RepairSignInOptions;
var
  Key: String;
begin
  Key := 'SOFTWARE\Microsoft\PolicyManager\default\Settings\AllowSignInOptions';
  if RegKeyExists(HKLM64, Key) and not RegValueExists(HKLM64, Key, 'value') then
  begin
    Log('AllowSignInOptions had no value - set back to 1');
    RegWriteDWordValue(HKLM64, Key, 'value', 1);
  end;
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

{ An update keeps config.ini and sounds.ini, so defaults that came later
  reach an existing install only here. Only what older versions shipped is
  replaced: a value somebody chose stays. }
procedure MigrateDefaults(const Cfg, Snd: String);
var
  Cur: String;
begin
  if (GetIniInt('update', 'enabled', 0, 0, 1, Cfg) = 0) and (GetIniString('update', 'manifest_url', '', Cfg) = '') then
  begin
    SetIniString('update', 'enabled', '1', Cfg);
    SetIniString('update', 'manifest_url', MANIFEST_URL, Cfg);
    Log('update check switched on');
  end;
  Cur := GetIniString('sounds', 'situp_ready', '', Snd);
  if (Cur = '') or (Cur = 'pushup_ready.wav') then
    SetIniString('sounds', 'situp_ready', 'situp_ready.wav', Snd);
  Cur := GetIniString('sounds', 'situp_counted', '', Snd);
  if (Cur = '') or (Cur = 'pushup_counted.wav') then
    SetIniString('sounds', 'situp_counted', 'situp_counted.wav', Snd);
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
  { Crash dumps of the service ([Registry] LocalDumps): created here so
    they inherit the data folder's ACL - a dump holds pieces of the
    service's memory. }
  ForceDirectories(Data + '\dumps');
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
    { "Remove administrator rights" on the account page. The admin tool
      checks again that another administrator stays (exit 3 if not). }
    if MakeStandard then
    begin
      Code := RunHidden(Admin, 'make-standard --user "' + ChildUser + '"');
      if Code = 3 then
        MsgBox(FmtMessage(CustomMessage('ErrMakeStandardOnly'), [ChildUser]), mbError, MB_OK)
      else if Code <> 0 then
        MsgBox(FmtMessage(CustomMessage('ErrMakeStandard'), [ChildUser, IntToStr(Code)]), mbError, MB_OK);
    end;
  end;

  MigrateDefaults(ExpandConstant('{app}\config.ini'), Data + '\sounds.ini');

  { 2. Service: create or repoint, auto-restart on crash, lock it so
       non-admins can't stop it, start. The calibration comes after
       setup closes ([Run]). }
  SetStatus(CustomMessage('StatusService'));
  BinPath := ' binPath= "\"' + SvcExe + '\"" start= auto DisplayName= "Exercise Unlock"';
  if RunHidden(SysExe('sc.exe'), 'query ' + SVC) = ERROR_SERVICE_DOES_NOT_EXIST then
    RunHidden(SysExe('sc.exe'), 'create ' + SVC + BinPath)
  else
    RunHidden(SysExe('sc.exe'), 'config ' + SVC + BinPath);
  RunHidden(SysExe('sc.exe'), 'description ' + SVC + ' "' + CustomMessage('ServiceDescription') + '"');
  RunHidden(SysExe('sc.exe'), 'failure ' + SVC + ' reset= 86400 actions= restart/5000/restart/5000/restart/30000');
  { ...and when it stops itself with an error (no pipe to serve), not
    only when it crashes. }
  RunHidden(SysExe('sc.exe'), 'failureflag ' + SVC + ' 1');
  Code := RunHidden(Admin, 'harden');
  if Code <> 0 then
    MsgBox(FmtMessage(CustomMessage('ErrHarden'), [IntToStr(Code), Data + '\admin.log']), mbError, MB_OK);
  Code := RunHidden(SysExe('sc.exe'), 'start ' + SVC);
  if (Code <> 0) and (Code <> ERROR_SERVICE_ALREADY_RUNNING) then
    MsgBox(FmtMessage(CustomMessage('ErrService'), [IntToStr(Code)]), mbError, MB_OK);

  { 3. Optional lockdown of PIN / Hello / password change. Undo it for a
       previously configured account when it changes or is unticked. }
  WantLock := WizardIsTaskSelected('lockdown');
  RepairSignInOptions;
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
  if RemovedAll or RestartingSetup then
    Confirm := False    { nothing left to cancel / setup runs again }
  else if Confirm then
  begin
    { "Exit Setup?" with the buttons in Setup's language (AskYesNo). }
    Confirm := False;
    Cancel := AskYesNo(SetupMessage(msgExitSetupTitle), SetupMessage(msgExitSetupMessage));
  end;
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
    { the calibration box goes under the longer text }
    WizardForm.RunList.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(12);
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
    RepairSignInOptions;
    { Wipe the stored password before the folder goes away. }
    RunHidden(ExpandConstant('{app}\ExerciseUnlockAdmin.exe'), 'clear');
  end;
end;
