# Exercise Unlock

Windows app: the sign-in screen requires the child to do a few exercises
in front of the webcam before the computer unlocks.

Fully native (C++20, Windows Credential Provider V2, Windows Service,
Media Foundation, ONNX Runtime + MoveNet).

## What it does

1. Child presses `Win+L` (or the system auto-locks after the session
   expires).
2. On the regular Windows sign-in screen, an **Exercise Unlock** tile
   appears next to Password / PIN.
3. Click it — the tile shows `Squats: N`, `Push-ups: N`,
   `Earned: 0:00 / 10:00`.
4. Child does exercises in front of the camera — counters go up.
5. When `Earned` reaches `min_unlock_seconds`, the tile shows
   "— ready to unlock". Child clicks Submit.
6. The provider reads the password from the DPAPI vault, packs a
   `KERB_INTERACTIVE_UNLOCK_LOGON`, hands it to LSA. Windows unlocks.
7. A timer starts for `earned` seconds. Five minutes before expiry,
   a toast warns the child. When time is up — auto-lock.
8. During an active session the camera is released (Zoom / Camera app
   can use it) and the exercise detectors freeze (reps done inside
   the session cannot be banked for the next unlock).

## Install (v1.1)

Build all three projects:

```powershell
pwsh -File tools\redeploy.ps1    # elevated
```

Output lands in `x64\Release\`:
- `ExerciseUnlockProvider.dll` — credential provider DLL
- `ExerciseUnlockService.exe` — service + sensor pipeline
- `ExerciseUnlockAdmin.exe` — admin CLI
- `onnxruntime.dll`, `movenet_lightning.onnx`, `movenet.data`,
  `config.ini` — runtime

### Step 1 — install the service (as admin)

```cmd
sc create ExerciseUnlockService binPath= "D:\...\x64\Release\ExerciseUnlockService.exe" start= auto DisplayName= "Exercise Unlock Service"
sc start ExerciseUnlockService
```

### Step 2 — register the credential provider

```cmd
copy /Y "D:\...\x64\Release\ExerciseUnlockProvider.dll" C:\Windows\System32\ExerciseUnlockProvider.dll
reg import "D:\...\ExerciseUnlockProvider\Register.reg"
```

### Step 3 — seed the vault with the child's password

```cmd
ExerciseUnlockAdmin.exe set --user child --domain .
```

Interactive prompt for password. The file lands in
`%ProgramData%\ExerciseUnlock\vault.dat`, encrypted with DPAPI
Local-Machine scope + an ACL that only SYSTEM and the local
Administrators group can read.

Multiple children on the same machine? Repeat step 3 with different
`--user` values; the vault stores them side by side:

```cmd
ExerciseUnlockAdmin.exe list
ExerciseUnlockAdmin.exe test --user child2
```

Verify:
```cmd
ExerciseUnlockAdmin.exe test
```
should print `OK  domain=.  user=<child>  (password N chars, hidden)`.

### Step 4 — harden the service (critical)

```cmd
ExerciseUnlockAdmin.exe harden
```

Sets an SCM DACL that only lets SYSTEM and Builtin\Administrators
stop / pause / delete / start the service. Without this, a standard
user (the child) can `sc stop ExerciseUnlockService` and bypass the
whole thing.

**Verify**: sign in as the child and run:
```cmd
sc stop ExerciseUnlockService
```
Should return `[SC] OpenService FAILED 5:  Access is denied.`

### Step 5 — child account must be Standard User

**Required.** The child needs a Standard User account, not
Administrator, otherwise they can:
- Edit `config.ini` (lower `min_unlock_seconds` to 1).
- Edit / delete `vault.dat` (it's DPAPI-encrypted so contents are safe,
  but they could replace it with their own).
- Stop the service via `sc stop` (even with harden — admins bypass).
- Delete the DLL from System32, unregister the provider in the registry.

Create a Standard User for the child:

```powershell
# as admin
$pass = Read-Host -AsSecureString "New child password"
New-LocalUser -Name "child" -Password $pass -FullName "Child Account" -Description "Kid account for Exercise Unlock"
Add-LocalGroupMember -Group "Users" -Member "child"
# do NOT add to Administrators!
```

Check: `Get-LocalGroupMember -Group Administrators` — `child` must
not appear.

The vault from step 3 must hold this child's password — not yours.

## Config

`x64\Release\config.ini`:

```ini
[reward]
pushup_seconds     = 120   ; 1 push-up = 2 min
squat_seconds      = 30    ; 1 squat   = 30 s
min_unlock_seconds = 600   ; minimum 10 min for an unlock

; v1.1 progression: after `progression_after_days` from first install,
; every rep is worth less over time. 0 disables.
;   scale = 1 / (1 + weeks_elapsed * progression_factor)
progression_after_days = 0
progression_factor     = 0.0

[squat]
standing_angle   = 168   ; knee at "up" (fully extended)
down_angle       = 125   ; knee at "down" (deep squat)
going_down_angle = 160
going_up_angle   = 135
min_rep_ms       = 800
max_rep_ms       = 4000
; Optional form-quality gate (0..1). 0 = disabled.
;   quality = (kStand - repMinAngle) / (kStand - kDown)
;   rep counted only when quality >= this value.
quality_min      = 0.0

[pushup]
up_angle         = 160   ; elbow at "up" (arms mostly straight)
down_angle       = 120   ; elbow at "down" (bent)
going_down_angle = 155
going_up_angle   = 130
plank_min_angle  = 30    ; shoulder-hip-ankle minimum (low because
                          ; MoveNet distorts under angled cameras)
min_rep_ms       = 500
max_rep_ms       = 4000
quality_min      = 0.0

[schedule]
; Legacy fallback used for both weekday and weekend when the
; day-specific keys below are absent.
active_from = 07:00
active_to   = 20:00
; v1.1 weekday / weekend overrides — HH:MM. Empty = use legacy.
weekday_from = 07:00
weekday_to   = 20:00
weekend_from = 08:00
weekend_to   = 21:00
; v1.1 per-day min_unlock_seconds override. Missing = use [reward].
weekday_min_unlock_seconds = 600
weekend_min_unlock_seconds = 900

[camera]
preferred = OsmoPocket   ; substring of the device friendly name

[security]
; Case-insensitive substring blocklist for camera friendly names —
; matches any of these mean the device is refused. Blocks the child
; from feeding a recorded video into OBS Virtual Camera etc.
virtual_camera_blocklist = OBS Virtual Camera,XSplit,ManyCam,Snap Camera,SplitCam,e2eSoft,vMix,Virtual Camera,DroidCam
; Liveness: if this many consecutive frames are byte-identical,
; the webcam is marked Failed (frozen / looped feed detected).
; 0 disables. Default ~30 = about 1 s at 30 fps.
liveness_max_static_frames = 30

[ux]
sound_on_rep = 1        ; 1 = short beep from LogonUI on rep count

[warnings]
lock_in_message = Computer will lock in {min} minutes

[labels]
squats        = Squats
pushups       = Push-ups
earned        = Earned
ready         = — ready to unlock
outside_hours = Outside allowed hours ({from}-{to})
```

After editing: `sc stop ExerciseUnlockService && sc start ExerciseUnlockService`.

### Reward formula

```
earned_seconds = pushups × pushup_seconds + squats × squat_seconds
if earned_seconds >= min_unlock_seconds → unlock for earned_seconds
```

With defaults (`pushup=120`, `squat=30`, `min_unlock=600`):

| combination                              | earned | result           |
|------------------------------------------|--------|------------------|
| 5 push-ups                               | 10 min | minimum unlock   |
| 20 squats                                | 10 min | minimum unlock   |
| 3 push-ups + 8 squats                    | 10 min | minimum unlock   |
| 10 push-ups                              | 20 min | 20-min session   |
| 15 push-ups                              | 30 min | 30-min session   |
| 5 push-ups at 22:00 (outside window)     | 0      | not credited     |

The session is always clamped to the end of the current local day
(23:59:59): if it "should" end tomorrow, it will lock at midnight.

### Bank (v1.0)

If the child does exercises on the sign-in screen but doesn't press
Submit (walked away, the screen re-locked on timeout, or `Win+L`
fired), the earned seconds are **not lost** — they're stored in
the bank (`state.dat`). On the next visit to the sign-in screen the
bank is added to the current counters:

```
earned = bank + (pushups × pushup_s) + (squats × squat_s)
```

On a successful unlock the bank is zeroed (spent on the session).

### Session persistence (v1.1)

The session expiry is now persisted to `state.dat` as well. If the
service crashes / the machine reboots mid-session, on next start the
service picks up where it left off. Sessions that would already have
expired are simply cleared.

### Difficulty progression (v1.1)

`[reward]/progression_after_days` and `progression_factor` — after
the first N days, every per-rep reward scales down each week:

```
scale = 1 / (1 + weeks_elapsed * progression_factor)
```

Example: `progression_after_days=14, progression_factor=0.1` — reps
are worth their base value for the first 2 weeks, then 10% less each
subsequent week. Never drops below 0.25× (keeps unlock reachable).

### Form-quality gate (v1.1)

`quality_min` in `[squat]` / `[pushup]` — reject reps that don't dip
deep enough into the down band. Quality is measured as the fraction
of the standing→down span the rep actually crossed. Set to 0.7 to
reject shallow half-reps. Every counted rep logs `q=0.NN` so you can
see what's happening. Default 0 = disabled.

### Bypass detection (v1.1)

- Virtual cameras (OBS Virtual Camera, DroidCam, XSplit, …) are
  refused by friendly-name substring match. Configurable via
  `[security]/virtual_camera_blocklist`.
- Liveness — the service hashes each captured frame; if too many
  identical frames arrive in a row, the webcam is marked Failed
  (frozen / looped feed detected) and no reps are counted.

## Parent overrides

```cmd
ExerciseUnlockAdmin.exe grant --minutes 30
```

Arms an unlock session for 30 minutes immediately, without requiring
reps. Use when the child is sick / the camera is broken / etc. Rep
counters and the bank are left alone. Uses the pipe, so the service
must be running.

## Files

`%ProgramData%\ExerciseUnlock\`:
- `vault.dat` — DPAPI-encrypted credentials, ACL SYSTEM+Admins
- `state.dat` — persistent counters + bank + session expiry
  + install date
- `service.log` — rolling service log (rotates at 5 MB
  → `service.log.1`), UTF-8
- `activity.log` — audit trail of unlock / expire events, UTF-8

`x64\Release\`:
- `debug_frame.bmp` — first camera frame after service start

`C:\Users\Public\`:
- `ExerciseUnlockWebcam.bmp` — latest camera frame dump (on demand
  via pipe)

## Diagnostics

**service.log** is the primary source:
```powershell
Get-Content 'C:\ProgramData\ExerciseUnlock\service.log' -Tail 100 -Wait
```
Everything is here: service startup, config/pose loading, camera
state, per-second DBG lines from each detector (with angles + phase),
`pushup +1` / `squat +1` on counted reps, `DROP dur=Xms` on reps
outside the timing window, plus `q=0.NN` quality scores.

**Dashboard** (offline HTML report):
```powershell
pwsh -File tools\dashboard.ps1 -Open
```
Live counters, 7-day bar charts of unlocked minutes / session count,
recent unlock history, service log tail. Double-click `dashboard.html`
after the first run to re-open; re-run the script to refresh.

**test-pipe.ps1** — pipe sanity check without the lock screen:
```powershell
pwsh -File tools\test-pipe.ps1
```
Prints everything the service knows: webcam, pose, counters, reward,
session. Also dumps the current webcam frame into
`tools\screenshots\YYYY-MM-DD_HH-MM-SS.bmp` for calibration.

**skeleton-viewer.ps1** — live overlay of MoveNet skeleton on the
webcam feed at ~4 Hz. Useful when tuning detector thresholds.

**DebugView** (Sysinternals): for realtime monitoring. Enable
*Capture Global Win32* + *Capture Events*. All `LogF` events fly
there with the `[ExerciseUnlockService]` prefix.

## Uninstall

```cmd
sc stop ExerciseUnlockService
sc delete ExerciseUnlockService
reg import "...\ExerciseUnlockProvider\Unregister.reg"
del C:\Windows\System32\ExerciseUnlockProvider.dll
rmdir /S /Q %ProgramData%\ExerciseUnlock
```

If the DLL is locked by LogonUI — sign out / reboot first.

## Requirements

- Windows 10/11 x64
- Visual Studio 2022+ (Community works) with workload
  *Desktop development with C++*
- MSVC v145 toolset (or adjust `PlatformToolset` in the `.vcxproj`)
- Windows SDK 10/11
- ONNX Runtime 1.30.0 CPU x64: download `onnxruntime-win-x64-1.30.0.zip` from
  https://github.com/microsoft/onnxruntime/releases extract
  `include` and `lib` into `third_party/onnxruntime/`
- MoveNet SinglePose ONNX → `models/movenet_lightning.onnx` (source: <URL>)
- External or built-in webcam — substring of its friendly name in
  `[camera]/preferred`

## License

MIT, see [LICENSE](LICENSE).

External components:
- ONNX Runtime: MIT License, © Microsoft Corporation
- MoveNet: Apache License 2.0, © Google