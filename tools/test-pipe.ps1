# Sends a GET_STATUS request to the ExerciseUnlock service pipe and
# prints the decoded response. Useful when iterating on the service
# without needing to Win+L each time.
#
# Run with the service already running, either via `sc start` or
# `.\ExerciseUnlockService.exe --console`.

$pipeName = 'ExerciseUnlock'
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $pipeName,
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None,
    [System.Security.Principal.TokenImpersonationLevel]::Impersonation)

try
{
    $pipe.Connect(2000)
}
catch
{
    Write-Error "cannot connect to \\.\pipe\$pipeName — is the service running?"
    exit 1
}
$pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message

# Request layout matches ExerciseUnlockCommon/Protocol.h::Request.
$req = New-Object byte[] 8
$magic = [byte[]] (0x45, 0x55, 0x4C, 0x4B)  # 'EULK'
[Array]::Copy($magic, 0, $req, 0, 4)
$req[4] = 1; $req[5] = 0    # version = 1
$req[6] = 1; $req[7] = 0    # op = GetStatus
$pipe.Write($req, 0, $req.Length)
$pipe.Flush()

# Response layout matches Protocol.h::Response (592 bytes).
$buf = New-Object byte[] 592
$n = $pipe.Read($buf, 0, $buf.Length)
if ($n -ne 592)
{
    Write-Error "short response: $n bytes"
    exit 1
}

$magic       = [System.BitConverter]::ToUInt32($buf, 0)
$version     = [System.BitConverter]::ToUInt16($buf, 4)
$op          = [System.BitConverter]::ToUInt16($buf, 6)
$pushDone    = [System.BitConverter]::ToInt32($buf, 8)
$pushGoal    = [System.BitConverter]::ToInt32($buf, 12)
$squatDone   = [System.BitConverter]::ToInt32($buf, 16)
$squatGoal   = [System.BitConverter]::ToInt32($buf, 20)
$state       = $buf[24]
$timeRem     = [System.BitConverter]::ToInt64($buf, 28)
$ts          = [System.BitConverter]::ToInt64($buf, 36)
$webState    = $buf[44]
$webFrames   = [System.BitConverter]::ToUInt32($buf, 48)
$webWidth    = [System.BitConverter]::ToUInt32($buf, 52)
$webHeight   = [System.BitConverter]::ToUInt32($buf, 56)
$webHresult  = [System.BitConverter]::ToUInt32($buf, 60)
$poseState   = $buf[64]
$poseHresult = [System.BitConverter]::ToUInt32($buf, 68)

$states = @{ 0 = 'Waiting'; 1 = 'PushUps'; 2 = 'Squats'; 3 = 'Completed' }
$webStates = @{ 0 = 'Unknown'; 1 = 'Probing'; 2 = 'Ok'; 3 = 'NoDevice'; 4 = 'Failed'; 5 = 'Idle' }
$poseStates = @{ 0 = 'Unknown'; 1 = 'NoModel'; 2 = 'Loading'; 3 = 'Ok'; 4 = 'Failed' }

"magic         : 0x{0:X8}" -f $magic
"version       : $version"
"op            : $op"
"push-ups      : $pushDone / $pushGoal"
"squats        : $squatDone / $squatGoal"
"state         : $($states[[int]$state]) ($state)"
"time remaining: $timeRem sec"
"server time   : $ts"
"webcam        : $($webStates[[int]$webState]) ($webState)"
"webcam size   : ${webWidth} x ${webHeight}"
"webcam frames : $webFrames"
"webcam hr     : 0x{0:X8}" -f $webHresult
"pose          : $($poseStates[[int]$poseState]) ($poseState)"
"pose hr       : 0x{0:X8}" -f $poseHresult

# v0.4b: 17 joints (12B each) at offset 72, then poseAvgConfidence,
# personDetected + 3 pad, poseFramesInferred.
$jointsOffset      = 72
$avgConfOffset     = $jointsOffset + 17 * 12  # = 276
$personDetOffset   = $avgConfOffset + 4        # = 280
$poseFramesOffset  = $personDetOffset + 4      # = 284

$avgConf     = [System.BitConverter]::ToSingle($buf, $avgConfOffset)
$personDet   = $buf[$personDetOffset]
$poseFrames  = [System.BitConverter]::ToUInt32($buf, $poseFramesOffset)

$good = 0
for ($i = 0; $i -lt 17; $i++)
{
    $c = [System.BitConverter]::ToSingle($buf, $jointsOffset + $i * 12 + 8)
    if ($c -ge 0.3) { $good++ }
}

"pose avg conf : {0:F3}" -f $avgConf
"person        : $(if ($personDet) { 'YES' } else { 'no' })"
"good joints   : $good / 17  (conf >= 0.3)"
"pose frames   : $poseFrames"

# v0.5: squat counter fields at offset 288.
$squatCount     = [System.BitConverter]::ToUInt32($buf, 288)
$squatKneeAngle = [System.BitConverter]::ToSingle($buf, 292)
$squatPhase     = $buf[296]

$squatPhases = @{ 0 = 'Unknown'; 1 = 'Standing'; 2 = 'GoingDown'; 3 = 'Down'; 4 = 'GoingUp' }

"squat count   : $squatCount"
"squat phase   : $($squatPhases[[int]$squatPhase]) ($squatPhase)"
"knee angle    : {0:F1} deg  (standing~170, deep squat~80)" -f $squatKneeAngle

# v0.6: push-up counter at offset 300.
$pushCount  = [System.BitConverter]::ToUInt32($buf, 300)
$pushElbow  = [System.BitConverter]::ToSingle($buf, 304)
$pushPlank  = [System.BitConverter]::ToSingle($buf, 308)
$pushPhase  = $buf[312]
$pushPhases = @{ 0 = 'Unknown'; 1 = 'Up'; 2 = 'GoingDown'; 3 = 'Down'; 4 = 'GoingUp'; 5 = 'NotInPlank' }

"pushup count  : $pushCount"
"pushup phase  : $($pushPhases[[int]$pushPhase]) ($pushPhase)"
"elbow angle   : {0:F1} deg  (top~170, bottom~90)" -f $pushElbow
"plank angle   : {0:F1} deg  (proper plank~170+)" -f $pushPlank

# v0.7: reward at offset 316.
$rewardEarned = [System.BitConverter]::ToUInt32($buf, 316)
$rewardMin    = [System.BitConverter]::ToUInt32($buf, 320)
$rewardEnough = $buf[324]

"reward earned : $rewardEarned s ({0}:{1:D2})  (bank+current)" -f ([int]($rewardEarned / 60)), [int]($rewardEarned % 60)
"reward min    : $rewardMin s ({0}:{1:D2})" -f ([int]($rewardMin / 60)), [int]($rewardMin % 60)
"reward enough : $(if ($rewardEnough) { 'YES — ready to unlock' } else { 'no' })"

# v0.9: session at offset 328.
$sessionActive    = $buf[328]
$sessionRemaining = [System.BitConverter]::ToUInt32($buf, 332)

"session active: $(if ($sessionActive) { 'YES' } else { 'no' })"
"session left  : $sessionRemaining s ({0}:{1:D2})" -f ([int]($sessionRemaining / 60)), [int]($sessionRemaining % 60)

# v0.9 rendered tile text (wchar_t[128] at offset 336).
$statusLen = 0
for ($i = 0; $i -lt 128; $i++)
{
    $c = [System.BitConverter]::ToUInt16($buf, 336 + $i * 2)
    if ($c -eq 0) { $statusLen = $i; break }
}
if ($statusLen -eq 0 -and [System.BitConverter]::ToUInt16($buf, 336) -ne 0)
{
    $statusLen = 128
}
$statusText = [System.Text.Encoding]::Unicode.GetString($buf, 336, $statusLen * 2)
""
"--- rendered tile text ---"
$statusText

$pipe.Close()

# Ask the service to dump its most recent webcam frame (BGRA -> BMP)
# to a fixed world-readable path, then copy that file into
# tools\screenshots\<timestamp>.bmp so successive calls don't overwrite
# each other. Only the service can talk to the camera (MF is single-
# owner), so we can't grab it directly from PowerShell.
try
{
    $shotDir = Join-Path $PSScriptRoot 'screenshots'
    if (-not (Test-Path $shotDir)) { New-Item -ItemType Directory -Path $shotDir | Out-Null }

    $pipe2 = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None,
        [System.Security.Principal.TokenImpersonationLevel]::Impersonation)
    $pipe2.Connect(2000)
    $pipe2.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message

    # Op::SaveWebcamFrame = 4 (see Protocol.h). Note: the outer $magic
    # variable was reassigned to a UInt32 above when we parsed the
    # GetStatus response, so rebuild the 4-byte 'EULK' header locally.
    $saveReq = New-Object byte[] 8
    $saveReq[0] = 0x45; $saveReq[1] = 0x55; $saveReq[2] = 0x4C; $saveReq[3] = 0x4B  # 'EULK'
    $saveReq[4] = 1; $saveReq[5] = 0    # version = 1
    $saveReq[6] = 4; $saveReq[7] = 0    # op = SaveWebcamFrame
    $pipe2.Write($saveReq, 0, $saveReq.Length)
    $pipe2.Flush()

    # Drain the fixed 592-byte response so the pipe closes cleanly.
    $drain = New-Object byte[] 592
    [void]$pipe2.Read($drain, 0, $drain.Length)
    $pipe2.Close()

    $installDir = (Get-ItemProperty 'HKLM:\SOFTWARE\ExerciseUnlock' -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
    $src = if ($installDir) { Join-Path $installDir 'data\webcam.bmp' } else { 'C:\ProgramData\ExerciseUnlock\webcam.bmp' }
    if (-not (Test-Path $src))
    {
        ""
        "webcam frame  : not available (service hasn't captured a frame yet)"
    }
    else
    {
        $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
        $dst   = Join-Path $shotDir "$stamp.bmp"
        Copy-Item -Path $src -Destination $dst -Force
        ""
        "webcam frame  : $dst"
    }
}
catch
{
    ""
    "webcam frame  : failed — $($_.Exception.Message)"
}
