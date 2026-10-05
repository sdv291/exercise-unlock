# Exercise Unlock

*English version: [README.en.md](README.en.md)*

Windows приложение: экран блокировки требует от ребёнка сделать несколько упражнений перед камерой прежде чем разблокировать компьютер.

Полностью native (C++20, Windows Credential Provider V2, Windows Service, Media Foundation, ONNX Runtime + MoveNet).

## Что делает

1. Ребёнок нажимает `Win+L` (или система заблокирована после auto-lock).
2. На стандартном Windows sign-in screen рядом с PIN/Password появляется плитка **Exercise Unlock**.
3. Клик — плитка показывает `Squats: N`, `Push-ups: N`, `Earned: 0:00 / 10:00`.
4. Ребёнок делает упражнения перед камерой — счётчики растут.
5. Когда `Earned` достигает `min_unlock_seconds` — «— ready to unlock». Ребёнок нажимает Submit.
6. Provider читает пароль из зашифрованного vault (DPAPI), формирует `KERB_INTERACTIVE_UNLOCK_LOGON`, отдаёт LSA. Windows разблокируется.
7. Стартует таймер на `earned` секунд. За 5 мин до конца — предупреждение. По истечении — авто-lock.
8. Во время активной сессии камера освобождена (Zoom / Camera app могут работать), детекторы упражнений замрят (нельзя набивать счётчик для следующего unlock).

## Установка (v1.0)

Собрать все три проекта:

```powershell
pwsh -File tools\redeploy.ps1     # elevated
```

Результат в `x64\Release\`:
- `ExerciseUnlockProvider.dll` — credential provider DLL
- `ExerciseUnlockService.exe` — сервис + сенсорный pipeline
- `ExerciseUnlockAdmin.exe` — админ CLI
- `onnxruntime.dll`, `movenet_lightning.onnx`, `movenet.data`, `config.ini` — рантайм

### Шаг 1 — установить сервис (от админа)

```cmd
sc create ExerciseUnlockService binPath= "D:\Documents\Projects\exersize-unlock\x64\Release\ExerciseUnlockService.exe" start= auto DisplayName= "Exercise Unlock Service"
sc start ExerciseUnlockService
```

### Шаг 2 — зарегистрировать Credential Provider

```cmd
copy /Y "D:\Documents\Projects\exersize-unlock\x64\Release\ExerciseUnlockProvider.dll" C:\Windows\System32\ExerciseUnlockProvider.dll
reg import "D:\Documents\Projects\exersize-unlock\ExerciseUnlockProvider\Register.reg"
```

### Шаг 3 — заполнить vault паролем ребёнка

```cmd
ExerciseUnlockAdmin.exe set
```

Ввести domain (обычно `.`), username, password. Файл ложится в `%ProgramData%\ExerciseUnlock\vault.dat`, зашифрован DPAPI Local Machine + ACL SYSTEM/Admins.

Проверить:
```cmd
ExerciseUnlockAdmin.exe test
```
Должно вывести `OK domain=. user=<child>` без пароля.

### Шаг 4 — заharden сервис (критично)

```cmd
ExerciseUnlockAdmin.exe harden
```

Ставит SCM DACL: только SYSTEM и Builtin\Administrators могут stop/pause/delete/start сервис. Без этого стандартный юзер (ребёнок) может `sc stop ExerciseUnlockService` и обойти всю систему.

**Проверить**: залогиниться под ребёнком, попробовать:
```cmd
sc stop ExerciseUnlockService
```
Должно вернуть `[SC] OpenService FAILED 5:  Access is denied.`

### Шаг 5 — учётка ребёнка = Standard User

**Обязательно**. Ребёнку нужен Standard User account, не Administrator, иначе он:
- Отредактирует `config.ini` (снизит `min_unlock_seconds` до 1)
- Отредактирует / удалит `vault.dat` (правда — DPAPI-зашифрован, ключ у SYSTEM; но переустановит свой)
- Отключит сервис через `sc stop` (даже с harden — админ обойдёт)
- Удалит DLL из System32, отменит регистрацию в реестре

Создать Standard User для ребёнка:

```powershell
# от админа
$pass = Read-Host -AsSecureString "New child password"
New-LocalUser -Name "child" -Password $pass -FullName "Child Account" -Description "Kid account for Exercise Unlock"
Add-LocalGroupMember -Group "Users" -Member "child"
# НЕ добавлять в Administrators!
```

Проверить: `Get-LocalGroupMember -Group Administrators` — `child` не должен быть в списке.

Vault на шаге 3 должен содержать пароль **именно этой** child-учётки — не вашей admin-учётки.

## Config

`x64\Release\config.ini`:

```ini
[reward]
pushup_seconds     = 120   ; 1 отжимание = 2 мин
squat_seconds      = 30    ; 1 присед    = 30 сек
min_unlock_seconds = 600   ; минимум 10 минут для unlock

[squat]
standing_angle   = 175     ; колено ~стоя (fully extended)
down_angle       = 100     ; колено ~глубокий присед
going_down_angle = 165
going_up_angle   = 110
min_rep_ms       = 800
max_rep_ms       = 4000

[pushup]
up_angle         = 160     ; локоть ~прямые руки
down_angle       = 120     ; локоть ~согнуты
going_down_angle = 155
going_up_angle   = 130
plank_min_angle  = 30      ; shoulder-hip-ankle минимум (низкий т.к. MoveNet искажает под наклоном камеры)
min_rep_ms       = 500
max_rep_ms       = 4000

[schedule]
active_from = 07:00        ; окно когда упражнения засчитываются
active_to   = 20:00        ; вне окна reps не считаются, unlock блокируется

[camera]
preferred = OsmoPocket     ; substring из friendly name устройства

[warnings]
lock_in_message = Computer will lock in {min} minutes

[labels]
squats        = Squats
pushups       = Push-ups
earned        = Earned
ready         = — ready to unlock
outside_hours = Outside allowed hours ({from}-{to})
```

После правки конфига: `sc stop ExerciseUnlockService && sc start ExerciseUnlockService`.

Локализация — перепишите значения в `[labels]` и `[warnings]/lock_in_message`.

### Формула вознаграждения

```
earned_seconds = pushups × pushup_seconds + squats × squat_seconds
if earned_seconds >= min_unlock_seconds → разблокировка на earned_seconds
```

С дефолтными значениями (`pushup=120`, `squat=30`, `min_unlock=600`):

| комбинация                              | earned | результат       |
|-----------------------------------------|--------|-----------------|
| 5 отжиманий                             | 10 мин | минимум unlock  |
| 20 приседов                             | 10 мин | минимум unlock  |
| 3 отжимания + 8 приседов                | 10 мин | минимум unlock  |
| 10 отжиманий                            | 20 мин | 20-мин сессия   |
| 15 отжиманий                            | 30 мин | 30-мин сессия   |
| 5 отжиманий (в 22:00 — вне окна)        | 0      | не засчитается  |

Разблокировка всегда обрезается до конца текущего дня (23:59:59 local): если сессия «должна» кончиться завтра — она принудительно кончится в полночь.

### Bank (v1.0)

Если ребёнок сделал упражнения на sign-in screen, но не нажал Submit (например ушёл, экран заблокировался снова через таймаут, или сработало `Win+L`) — заработанные секунды **не теряются**, а сохраняются в bank (`state.dat`). При следующем визите на sign-in screen bank добавляется к текущим repsm счётчикам:

```
earned = bank + (pushups × pushup_s) + (squats × squat_s)
```

При успешном unlock bank обнуляется (потрачено на текущую сессию).

## Файлы состояния

`%ProgramData%\ExerciseUnlock\`:
- `vault.dat` — DPAPI-зашифрованные creds, ACL SYSTEM+Admins
- `state.dat` — persistent счётчики + earned bank (переживает рестарт сервиса)
- `service.log` — rolling лог сервиса (rotate на 5 MB → `service.log.1`), UTF-8
- `activity.log` — audit-лог событий unlock/expire, UTF-8

`x64\Release\`:
- `debug_frame.bmp` — первый кадр после старта сервиса

`C:\Users\Public\`:
- `ExerciseUnlockWebcam.bmp` — дамп последнего кадра камеры (по запросу через pipe op)

## Диагностика

**service.log** — основной источник:
```powershell
Get-Content 'C:\ProgramData\ExerciseUnlock\service.log' -Tail 100 -Wait
```
Здесь видно всё: старт сервиса, загрузку config/pose, состояние камеры, DBG-строки детекторов (раз в секунду с текущими углами и фазой), `pushup +1` / `squat +1` при засчёте, `DROP dur=Xms` если реп не в тайминг-окно.

**test-pipe.ps1** — sanity-check pipe без экрана блокировки:
```powershell
pwsh -File tools\test-pipe.ps1
```
Печатает всё что сервис знает: webcam, pose, счётчики, reward, session. Заодно дампит текущий кадр камеры в `tools\screenshots\YYYY-MM-DD_HH-MM-SS.bmp` (полезно для калибровки порогов).

**DebugView** (Sysinternals): для realtime мониторинга включить *Capture Global Win32* + *Capture Events*. Все `LogF` события летят туда с префиксом `[ExerciseUnlockService]`.

## Удаление

```cmd
sc stop ExerciseUnlockService
sc delete ExerciseUnlockService
reg import "...\ExerciseUnlockProvider\Unregister.reg"
del C:\Windows\System32\ExerciseUnlockProvider.dll
rmdir /S /Q %ProgramData%\ExerciseUnlock
```

Если DLL заблокирована LogonUI — сначала logoff / reboot.

## Требования

- Windows 10/11 x64
- Visual Studio 2022+ (Community OK) с workload *Desktop development with C++*
- MSVC v145 toolset (или адаптируйте `PlatformToolset` в `.vcxproj`)
- Windows SDK 10/11
- ONNX Runtime CPU x64 в `third_party/onnxruntime/` (см. `README` для скачивания)
- MoveNet SinglePose Thunder ONNX в `models/movenet_lightning.onnx`
- Внешняя webcam или встроенная — в `[camera]/preferred` подстрока friendly name

## Лицензия

MIT, см. [LICENSE](LICENSE).

Сторонние компоненты:
- ONNX Runtime: MIT License, © Microsoft Corporation
- MoveNet: Apache License 2.0, © Google