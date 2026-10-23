# Live skeleton viewer for Exercise Unlock.
#
# Every ~250 ms:
#   1) asks the service to dump its latest webcam frame to
#      <data folder>\webcam.bmp
#   2) reads the pipe status (joints + angles + phase)
#   3) draws the skeleton, joint dots, and knee/elbow/plank angle
#      labels on top of the frame in a WinForms window.
#
# Every `SnapshotIntervalSec` seconds (default 5s) saves the
# currently-displayed annotated frame to
# $SnapshotDir\snapshot-YYYY-MM-DDTHH-MM-SS.png so a diagnostic
# session can be reviewed later without eyes glued to the window.
#
# Run elevated (the service pipe is SYSTEM + Administrators only);
# needs the service running. Close the window to stop.

param(
    [int]$SnapshotIntervalSec = 5,
    [string]$SnapshotDir = ''
)

# Data folder: <InstallDir>\data for installed builds (HKLM\SOFTWARE\ExerciseUnlock),
# the legacy C:\ProgramData\ExerciseUnlock for dev deployments - same rule as Paths.h.
$installDir = (Get-ItemProperty 'HKLM:\SOFTWARE\ExerciseUnlock' -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
$dataDir = if ($installDir) { Join-Path $installDir 'data' } else { 'C:\ProgramData\ExerciseUnlock' }
if (-not $SnapshotDir) { $SnapshotDir = Join-Path $dataDir 'snapshots' }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$pipeName = 'ExerciseUnlock'
$magic    = [byte[]] (0x45, 0x55, 0x4C, 0x4B)  # 'EULK'

if (-not (Test-Path $SnapshotDir)) {
    New-Item -ItemType Directory -Path $SnapshotDir -Force | Out-Null
}
Write-Host "Snapshots -> $SnapshotDir (every $SnapshotIntervalSec s)"

# 17 COCO joints as returned by MoveNet.
$JointNames = @(
    'nose','L-eye','R-eye','L-ear','R-ear',
    'L-shoulder','R-shoulder','L-elbow','R-elbow','L-wrist','R-wrist',
    'L-hip','R-hip','L-knee','R-knee','L-ankle','R-ankle')

# Bone connections: (jointA, jointB) pairs.
$Bones = @(
    @(5,6),   @(5,7),  @(7,9),   @(6,8),  @(8,10),
    @(5,11),  @(6,12), @(11,12),
    @(11,13), @(13,15),@(12,14), @(14,16),
    @(0,1),   @(0,2),  @(1,3),   @(2,4))

function Send-PipeRequest([byte]$op, [int]$respBytes = 592)
{
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
        '.', $pipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None,
        [System.Security.Principal.TokenImpersonationLevel]::Impersonation)
    try { $pipe.Connect(1500) } catch { $pipe.Dispose(); return $null }
    $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message

    $req = New-Object byte[] 8
    [Array]::Copy($magic, 0, $req, 0, 4)
    $req[4] = 1; $req[5] = 0
    $req[6] = $op; $req[7] = 0
    $pipe.Write($req, 0, 8); $pipe.Flush()

    $buf = New-Object byte[] $respBytes
    $n = $pipe.Read($buf, 0, $buf.Length)
    $pipe.Close()
    if ($n -ne $respBytes) { return $null }
    return $buf
}

function Parse-Status([byte[]]$buf)
{
    if (-not $buf) { return $null }
    $status = @{
        joints = @()
        squatCount   = [System.BitConverter]::ToUInt32($buf, 288)
        squatKnee    = [System.BitConverter]::ToSingle($buf, 292)
        squatPhase   = $buf[296]
        pushCount    = [System.BitConverter]::ToUInt32($buf, 300)
        pushElbow    = [System.BitConverter]::ToSingle($buf, 304)
        pushPlank    = [System.BitConverter]::ToSingle($buf, 308)
        pushPhase    = $buf[312]
        earned       = [System.BitConverter]::ToUInt32($buf, 316)
        minUnlock    = [System.BitConverter]::ToUInt32($buf, 320)
        personDet    = $buf[280]
        avgConf      = [System.BitConverter]::ToSingle($buf, 276)
        webcamState  = $buf[44]
    }
    for ($i = 0; $i -lt 17; $i++) {
        $off = 72 + $i * 12
        $status.joints += @{
            y    = [System.BitConverter]::ToSingle($buf, $off)
            x    = [System.BitConverter]::ToSingle($buf, $off + 4)
            conf = [System.BitConverter]::ToSingle($buf, $off + 8)
        }
    }
    return $status
}

$SquatPhaseNames = @{ 0='Unknown';1='Standing';2='GoingDown';3='Down';4='GoingUp' }
$PushPhaseNames  = @{ 0='Unknown';1='Up';2='GoingDown';3='Down';4='GoingUp';5='NotInPlank' }

# --- WinForms window ---
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Exercise Unlock — Skeleton Viewer'
$form.Size = New-Object System.Drawing.Size(1300, 780)
$form.StartPosition = 'CenterScreen'
$form.BackColor = [System.Drawing.Color]::Black

$pic = New-Object System.Windows.Forms.PictureBox
$pic.Dock = 'Fill'
$pic.SizeMode = 'Zoom'
$form.Controls.Add($pic)

$statusBar = New-Object System.Windows.Forms.StatusStrip
$statusLabel = New-Object System.Windows.Forms.ToolStripStatusLabel
$statusLabel.Text = 'starting...'
$statusBar.Items.Add($statusLabel) | Out-Null
$form.Controls.Add($statusBar)

# Render one frame into the PictureBox.
function Update-Frame
{
    $status = Parse-Status (Send-PipeRequest 1)
    if (-not $status) {
        $statusLabel.Text = 'service not reachable — is ExerciseUnlockService running?'
        return
    }
    # WebcamState.Idle: the service only runs the camera on the lock
    # screen. For calibrating from the desktop set [camera]
    # lock_screen_only = 0 in config.ini and restart the service.
    if ($status.webcamState -eq 5) {
        $statusLabel.Text = 'camera idle — it runs only on the lock screen ([camera] lock_screen_only = 0 + service restart to calibrate here)'
        return
    }

    # Kick service to dump latest camera frame.
    $null = Send-PipeRequest 4

    $bmpPath = Join-Path $dataDir 'webcam.bmp'
    if (-not (Test-Path $bmpPath)) {
        $statusLabel.Text = 'no webcam frame yet (session active or camera absent)'
        return
    }

    # Read bytes directly and construct the Bitmap from a MemoryStream.
    # Avoids [Bitmap]::FromFile which keeps the file locked for the
    # Bitmap's lifetime, and avoids the OS file cache serving stale
    # bytes between quick successive polls.
    $bytes = $null
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try {
            $fs = [IO.File]::Open($bmpPath,
                [IO.FileMode]::Open, [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
            try {
                $bytes = New-Object byte[] $fs.Length
                [void]$fs.Read($bytes, 0, $bytes.Length)
            } finally { $fs.Dispose() }
            break
        } catch {
            Start-Sleep -Milliseconds 15   # service writes with CREATE_ALWAYS — brief race
        }
    }
    if (-not $bytes -or $bytes.Length -lt 54) {
        $statusLabel.Text = 'bmp read failed (short or locked)'
        return
    }

    $ms  = New-Object System.IO.MemoryStream (,$bytes)
    try   { $bmp = [System.Drawing.Bitmap]::new($ms) }
    catch { $statusLabel.Text = "bmp decode failed: $($_.Exception.Message)"; return }
    # NOTE: don't dispose $ms until the Bitmap is itself disposed —
    # GDI+ Bitmap over a stream reads lazily. Attach it to the bitmap
    # Tag so GC keeps it alive as long as the bitmap lives.
    $bmp.Tag = $ms

    # Stale detection via source mtime — pixel-content hashing gives
    # false positives when the room is empty (near-identical frames
    # are still legitimately fresh). File mtime answers the only
    # question we care about: is the service refreshing the dump at
    # all? Each Op::SaveWebcamFrame on the service calls CreateFile
    # with CREATE_ALWAYS, which bumps the LastWriteTime.
    $mtime = (Get-Item $bmpPath -ErrorAction SilentlyContinue).LastWriteTime
    if (-not (Get-Variable -Scope Script -Name _lastFrameMtime -ErrorAction SilentlyContinue)) {
        $script:_lastFrameMtime = [datetime]::MinValue
        $script:_frameCount     = 0
        $script:_staleCount     = 0
    }
    if ($mtime -eq $script:_lastFrameMtime) { $script:_staleCount++ }
    else { $script:_staleCount = 0; $script:_frameCount++ }
    $script:_lastFrameMtime = $mtime

    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'

    $w = $bmp.Width; $h = $bmp.Height
    $penBone   = New-Object System.Drawing.Pen ([System.Drawing.Color]::Cyan, 3)
    $penHi     = New-Object System.Drawing.Pen ([System.Drawing.Color]::Yellow, 4)
    $brushDot  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $brushBad  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(120,255,0,0))
    $brushTxt  = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::Yellow)
    $brushShad = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(180,0,0,0))
    $font      = New-Object System.Drawing.Font ('Consolas', 14, [System.Drawing.FontStyle]::Bold)
    $fontSm    = New-Object System.Drawing.Font ('Consolas', 10)

    # Confidence floor for drawing anything at all. MoveNet always
    # returns 17 points — low-confidence sets form the "ghost skeleton"
    # glitches when the frame is empty. Service's personDetected flag
    # (avgConfidence >= 0.3) is a solid signal: below it, we hide the
    # overlay and show a NO PERSON banner so the user understands why
    # the camera-raw image is unannotated.
    $personPresent = ($status.personDet -ne 0) -and ($status.avgConf -ge 0.25)
    if (-not $personPresent) {
        $banner = "NO PERSON  (avgConf={0:F2})" -f $status.avgConf
        $sz = $g.MeasureString($banner, $font)
        $cx = ($w - $sz.Width) / 2
        $cy = $h - $sz.Height - 20
        $g.FillRectangle($brushShad, $cx - 10, $cy - 6, $sz.Width + 20, $sz.Height + 12)
        $g.DrawString($banner, $font, (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::OrangeRed)), $cx, $cy)
    }

    # Bones — only when a person is actually there, otherwise skip
    # (prevents the "random stickman draped over the fridge" glitch).
    foreach ($b in $Bones) {
        if (-not $personPresent) { break }
        $a = $status.joints[$b[0]]; $c = $status.joints[$b[1]]
        if ($a.conf -lt 0.20 -or $c.conf -lt 0.20) { continue }
        $ax = [int]($a.x * $w); $ay = [int]($a.y * $h)
        $cx = [int]($c.x * $w); $cy = [int]($c.y * $h)
        $g.DrawLine($penBone, $ax, $ay, $cx, $cy)
    }

    # Highlighted bones for the two exercises: knee triangle + elbow.
    foreach ($tri in @(@(11,13,15), @(12,14,16), @(5,7,9), @(6,8,10))) {
        if (-not $personPresent) { break }
        $j1 = $status.joints[$tri[0]]; $j2 = $status.joints[$tri[1]]; $j3 = $status.joints[$tri[2]]
        if ($j1.conf -ge 0.3 -and $j2.conf -ge 0.3 -and $j3.conf -ge 0.3) {
            $x1 = [int]($j1.x*$w); $y1 = [int]($j1.y*$h)
            $x2 = [int]($j2.x*$w); $y2 = [int]($j2.y*$h)
            $x3 = [int]($j3.x*$w); $y3 = [int]($j3.y*$h)
            $g.DrawLine($penHi, $x1, $y1, $x2, $y2)
            $g.DrawLine($penHi, $x2, $y2, $x3, $y3)
        }
    }

    # Joint dots — raised confidence floor + skipped entirely when no
    # person is present, to kill the "ghost skeleton" artefacts.
    if ($personPresent) {
        for ($i = 0; $i -lt 17; $i++) {
            $j = $status.joints[$i]
            if ($j.conf -lt 0.25) { continue }
            $x = [int]($j.x * $w); $y = [int]($j.y * $h)
            $r = if ($j.conf -ge 0.4) { 6 } else { 4 }
            $brush = if ($j.conf -ge 0.4) { $brushDot } else { $brushBad }
            $g.FillEllipse($brush, $x - $r, $y - $r, $r*2, $r*2)
        }
    }

    # Top-left overlay: current angles + counters.
    $lines = @(
        ("SQUAT  count={0}  phase={1}  knee={2:F1}°" -f `
            $status.squatCount, $SquatPhaseNames[[int]$status.squatPhase], $status.squatKnee),
        ("PUSHUP count={0}  phase={1}  elbow={2:F1}°  plank={3:F1}°" -f `
            $status.pushCount, $PushPhaseNames[[int]$status.pushPhase], $status.pushElbow, $status.pushPlank),
        ("EARNED {0}s / {1}s  person={2}  avgConf={3:F2}" -f `
            $status.earned, $status.minUnlock,
            ($(if ($status.personDet) { 'YES' } else { 'no' })), $status.avgConf)
    )
    $y0 = 8
    foreach ($ln in $lines) {
        $sz = $g.MeasureString($ln, $font)
        $g.FillRectangle($brushShad, 4, $y0 - 2, [int]$sz.Width + 8, [int]$sz.Height + 4)
        $g.DrawString($ln, $font, $brushTxt, 8, $y0)
        $y0 += [int]$sz.Height + 6
    }

    # Joint labels for the sport-relevant ones only (shoulder/elbow/wrist/hip/knee/ankle).
    if ($personPresent) {
        $labelIdx = @(5,6,7,8,9,10,11,12,13,14,15,16)
        foreach ($i in $labelIdx) {
            $j = $status.joints[$i]
            if ($j.conf -lt 0.35) { continue }
            $x = [int]($j.x * $w) + 6
            $y = [int]($j.y * $h) - 6
            $lbl = "{0} {1:F2}" -f $JointNames[$i], $j.conf
            $sz = $g.MeasureString($lbl, $fontSm)
            $g.FillRectangle($brushShad, $x - 2, $y - 2, [int]$sz.Width + 4, [int]$sz.Height + 2)
            $g.DrawString($lbl, $fontSm, $brushDot, $x, $y)
        }
    }

    $g.Dispose()
    $pen | ForEach-Object { if ($_) { $_.Dispose() } }
    $font.Dispose(); $fontSm.Dispose()

    # Save a snapshot of the annotated frame on a wall-clock cadence
    # (not every N ticks — the tick loop can jitter, and we want
    # evenly-spaced samples for post-session review). Format is PNG
    # because annotated frames compress ~5× smaller than the raw
    # BMP, and a 15-minute session at 5 s cadence still fits in a
    # few hundred MB. Timestamp with ISO-local, colons swapped to
    # dashes so Windows accepts the filename.
    $savedPath = $null
    if ($SnapshotIntervalSec -gt 0)
    {
        $now = Get-Date
        if (-not (Get-Variable -Scope Script -Name _lastSnapshotAt -ErrorAction SilentlyContinue)) {
            $script:_lastSnapshotAt = [datetime]::MinValue
            $script:_snapshotCount  = 0
        }
        if (($now - $script:_lastSnapshotAt).TotalSeconds -ge $SnapshotIntervalSec)
        {
            $stamp = $now.ToString('yyyy-MM-ddTHH-mm-ss')
            $savedPath = Join-Path $SnapshotDir "snapshot-$stamp.png"
            try {
                $bmp.Save($savedPath, [System.Drawing.Imaging.ImageFormat]::Png)
                $script:_snapshotCount++
                $script:_lastSnapshotAt = $now
            } catch {
                # Non-fatal; keep running even if a save hiccups.
                $savedPath = "save failed: $($_.Exception.Message)"
            }
        }
    }

    $old = $pic.Image
    $pic.Image = $bmp
    if ($old) { $old.Dispose() }

    $staleTag = if ($script:_staleCount -gt 2) {
        "  — STALE (same frame x$($script:_staleCount+1), session active or camera idle?)"
    } else { '' }
    $snapTag = if ($savedPath) { "  [SAVED #$($script:_snapshotCount)]" }
               elseif ($script:_snapshotCount) { "  snaps=$($script:_snapshotCount)" }
               else { '' }

    $statusLabel.Text = "frames={0}  squats={1}  pushups={2}  earned={3}s  phase(P/S)={4}/{5}{6}{7}" -f `
        $script:_frameCount, $status.squatCount, $status.pushCount, $status.earned,
        $PushPhaseNames[[int]$status.pushPhase], $SquatPhaseNames[[int]$status.squatPhase],
        $staleTag, $snapTag
}

# Timer drives the redraw loop at ~4 Hz.
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({ Update-Frame })
$form.Add_Shown({ $timer.Start() })
$form.Add_FormClosing({ $timer.Stop(); $timer.Dispose() })

[System.Windows.Forms.Application]::Run($form)
