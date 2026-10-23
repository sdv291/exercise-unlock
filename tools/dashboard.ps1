# Exercise Unlock — parent dashboard.
#
# Reads:
#   <data folder>\state.dat     — persistent counters + bank
#   <data folder>\activity.log  — unlock/expire audit trail
#   <data folder>\service.log   — last N service events
#   config.ini                   — active configuration
# (<InstallDir>\data when installed, C:\ProgramData\ExerciseUnlock for dev builds)
# and via the pipe:
#   Op::GetStatus                              — live state + webcam + reps
#
# Emits an offline HTML page you can double-click any time to see today's
# progress + a 7-day chart + the recent unlock history. Read-only — no
# configuration changes here (use ExerciseUnlockAdmin for that).

param(
    [string]$Out = (Join-Path $PSScriptRoot 'dashboard.html'),
    [switch]$Open
)

# Data folder: <InstallDir>\data for installed builds (HKLM\SOFTWARE\ExerciseUnlock),
# the legacy C:\ProgramData\ExerciseUnlock for dev deployments - same rule as Paths.h.
$installDir = (Get-ItemProperty 'HKLM:\SOFTWARE\ExerciseUnlock' -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
$dataDir = if ($installDir) { Join-Path $installDir 'data' } else { 'C:\ProgramData\ExerciseUnlock' }
$statePath   = Join-Path $dataDir  'state.dat'
$activityLog = Join-Path $dataDir  'activity.log'
$serviceLog  = Join-Path $dataDir  'service.log'
$releaseDir  = if ($installDir) { $installDir } else { Join-Path $PSScriptRoot '..\x64\Release' }
$configIni   = Join-Path $releaseDir 'config.ini'

function Read-StateDat([string]$path)
{
    if (-not (Test-Path $path)) { return $null }
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 16) { return $null }
    $magic   = [BitConverter]::ToUInt32($b, 0)
    $version = [BitConverter]::ToUInt16($b, 4)
    if ($magic -ne 0x54535545) { return $null }   # 'EUST'
    $result = [ordered]@{
        Version   = $version
        Pushups   = [BitConverter]::ToUInt32($b, 8)
        Squats    = [BitConverter]::ToUInt32($b, 12)
        Bank      = 0; SessionExpires = 0; InstallStart = 0
    }
    if ($b.Length -ge 20) { $result.Bank = [BitConverter]::ToUInt32($b, 16) }
    if ($b.Length -ge 36) {
        $result.SessionExpires = [BitConverter]::ToInt64($b, 20)
        $result.InstallStart   = [BitConverter]::ToInt64($b, 28)
    }
    return $result
}

function Parse-Activity([string]$path)
{
    if (-not (Test-Path $path)) { return @() }
    $lines = Get-Content $path -Encoding UTF8 -ErrorAction SilentlyContinue
    $events = @()
    foreach ($ln in $lines) {
        if ($ln -match '^\uFEFF?(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})\s+(.+)$') {
            $ts = [datetime]::ParseExact($matches[1], 'yyyy-MM-ddTHH:mm:ss', $null)
            $events += [pscustomobject]@{
                When  = $ts
                Text  = $matches[2]
            }
        }
    }
    return $events
}

# Ask the service for live status (best-effort).
function Query-Live
{
    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(
            '.', 'ExerciseUnlock',
            [System.IO.Pipes.PipeDirection]::InOut,
            [System.IO.Pipes.PipeOptions]::None,
            [System.Security.Principal.TokenImpersonationLevel]::Impersonation)
        $pipe.Connect(1000)
        $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
        $req = New-Object byte[] 8
        $req[0]=0x45;$req[1]=0x55;$req[2]=0x4C;$req[3]=0x4B
        $req[4]=1;$req[5]=0;$req[6]=1;$req[7]=0
        $pipe.Write($req,0,8); $pipe.Flush()
        $buf = New-Object byte[] 592
        $n = $pipe.Read($buf,0,$buf.Length)
        $pipe.Close()
        if ($n -ne 592) { return $null }
        return @{
            SquatCount = [BitConverter]::ToUInt32($buf, 288)
            PushCount  = [BitConverter]::ToUInt32($buf, 300)
            Earned     = [BitConverter]::ToUInt32($buf, 316)
            MinUnlock  = [BitConverter]::ToUInt32($buf, 320)
            SessionAct = $buf[328]
            SessionRem = [BitConverter]::ToUInt32($buf, 332)
            WebcamState= $buf[44]
            PoseState  = $buf[64]
        }
    } catch { return $null }
}

function Fmt-Seconds([int]$s) {
    if ($s -le 0) { return '0:00' }
    # Pull out minutes / remaining seconds BEFORE calling -f so
    # PowerShell's operator-precedence quirk (-f binding one arg
    # then the trailing `, expr` becoming an array element rather
    # than a second -f argument) can't produce "M:S*60" output.
    $m = [int]($s / 60)
    $r = $s % 60
    return ('{0}:{1:D2}' -f $m, $r)
}

# ---- gather data ----
$state = Read-StateDat $statePath
$events = Parse-Activity $activityLog
$live = Query-Live

# Aggregate unlock events per day for the last 7 days.
$today = (Get-Date).Date
$dayLabels = @()
$dailyMinutes = @()
$dailyUnlocks = @()
for ($i = 6; $i -ge 0; $i--) {
    $d = $today.AddDays(-$i)
    $dayLabels += $d.ToString('ddd d.MM')
    $mins = 0; $cnt = 0
    foreach ($e in $events) {
        if ($e.When.Date -eq $d -and $e.Text -match 'unlock granted:.*earned=(\d+)s') {
            $mins += [int]$matches[1] / 60
            $cnt++
        }
    }
    $dailyMinutes += [int]$mins
    $dailyUnlocks += $cnt
}

$recentUnlocks = @($events | Where-Object { $_.Text -like 'unlock granted:*' } |
                   Select-Object -Last 20 | Sort-Object When -Descending)

# ---- build HTML ----
$installedAgo = if ($state -and $state.InstallStart -gt 0) {
    $s = [datetimeoffset]::FromUnixTimeSeconds([long]$state.InstallStart).LocalDateTime
    $days = [int]((Get-Date) - $s).TotalDays
    "$days days ($($s.ToString('yyyy-MM-dd')))"
} else { 'unknown' }

$live_html = if ($live) {
    $webcamNames = @{ 0='Unknown'; 1='Probing'; 2='Ok'; 3='NoDevice'; 4='Failed'; 5='Idle (off outside lock screen)' }
    $poseNames   = @{ 0='Unknown'; 1='NoModel'; 2='Loading'; 3='Ok'; 4='Failed' }
    $sessTxt = if ($live.SessionAct -ne 0) {
        "active — " + (Fmt-Seconds $live.SessionRem) + " left"
    } else { 'idle' }
    @"
<div class=grid>
  <div class=stat><div class=stat-label>Squats now</div><div class=stat-value>$($live.SquatCount)</div></div>
  <div class=stat><div class=stat-label>Push-ups now</div><div class=stat-value>$($live.PushCount)</div></div>
  <div class=stat><div class=stat-label>Earned</div><div class=stat-value>$(Fmt-Seconds $live.Earned) <span class=stat-sub>/ $(Fmt-Seconds $live.MinUnlock)</span></div></div>
  <div class=stat><div class=stat-label>Session</div><div class=stat-value>$sessTxt</div></div>
  <div class=stat><div class=stat-label>Webcam</div><div class=stat-value>$($webcamNames[[int]$live.WebcamState])</div></div>
  <div class=stat><div class=stat-label>Pose</div><div class=stat-value>$($poseNames[[int]$live.PoseState])</div></div>
</div>
"@
} else {
    "<p class=warn>Service not reachable — start it with <code>sc start ExerciseUnlockService</code>.</p>"
}

$state_html = if ($state) {
    $sessExp = if ($state.SessionExpires -gt 0) {
        [datetimeoffset]::FromUnixTimeSeconds([long]$state.SessionExpires).LocalDateTime.ToString('HH:mm:ss')
    } else { '—' }
    "<p>Bank: <b>$(Fmt-Seconds $state.Bank)</b> · Persisted squats: <b>$($state.Squats)</b> · Persisted push-ups: <b>$($state.Pushups)</b> · Session-expires-at: <b>$sessExp</b></p>"
} else { "<p class=warn>state.dat missing — no persisted counters yet.</p>" }

# Chart data as JSON.
$chartLabels = ($dayLabels | ForEach-Object { '"' + $_ + '"' }) -join ','
$chartMins   = $dailyMinutes -join ','
$chartCounts = $dailyUnlocks -join ','

# Recent unlocks table rows.
$rows = ($recentUnlocks | ForEach-Object {
    "<tr><td>$($_.When.ToString('yyyy-MM-dd HH:mm'))</td><td>$([Web.HttpUtility]::HtmlEncode($_.Text))</td></tr>"
}) -join "`n"

# Recent service.log tail (last 40 lines).
$svcTail = ''
if (Test-Path $serviceLog) {
    $svcTail = (Get-Content $serviceLog -Tail 40 -Encoding UTF8) -join "`n"
    $svcTail = [Web.HttpUtility]::HtmlEncode($svcTail)
}

Add-Type -AssemblyName System.Web

$html = @"
<!doctype html>
<html lang=en>
<head>
<meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>Exercise Unlock — Dashboard</title>
<style>
:root { color-scheme: dark; --bg:#0a0a0b; --panel:#131316; --panel2:#1a1a1e;
        --border:#2a2a30; --text:#ececef; --muted:#8a8a94; --accent:#22d3ee;
        --ok:#34d399; --warn:#f59e0b; --err:#f87171; }
body { margin:0; font-family:-apple-system,'Segoe UI',Roboto,sans-serif;
       background:var(--bg); color:var(--text); }
header { padding:20px 28px; border-bottom:1px solid var(--border);
         display:flex; align-items:baseline; gap:16px; }
h1 { margin:0; font-size:22px; font-weight:600; }
.gen { color:var(--muted); font-size:12px; }
main { max-width:1100px; margin:0 auto; padding:24px 20px 60px; }
h2 { margin:32px 0 12px; font-size:16px; color:var(--muted);
     text-transform:uppercase; letter-spacing:.8px; font-weight:600; }
.grid { display:grid; gap:12px;
        grid-template-columns:repeat(auto-fit,minmax(160px,1fr)); }
.stat { background:var(--panel); border:1px solid var(--border);
        border-radius:12px; padding:14px 16px; }
.stat-label { color:var(--muted); font-size:11px; letter-spacing:.4px;
              text-transform:uppercase; margin-bottom:6px; }
.stat-value { font-size:22px; font-weight:600; }
.stat-sub { font-size:13px; color:var(--muted); font-weight:400; }
.card { background:var(--panel); border:1px solid var(--border);
        border-radius:12px; padding:16px; overflow:auto; }
table { width:100%; border-collapse:collapse; font-size:13px; }
th,td { text-align:left; padding:8px 10px; border-bottom:1px solid var(--border); }
th { color:var(--muted); font-weight:500; font-size:11px; text-transform:uppercase; letter-spacing:.4px; }
code, pre { font-family:Consolas,'JetBrains Mono',monospace;
            background:var(--panel2); border-radius:6px; padding:2px 6px; }
pre { padding:12px; overflow:auto; font-size:12px; line-height:1.5; }
.warn { color:var(--warn); }
canvas { max-width:100%; height:220px; }
footer { color:var(--muted); font-size:11px; text-align:center; padding:24px 0; }
</style>
</head>
<body>
<header>
  <h1>Exercise Unlock — Dashboard</h1>
  <span class=gen>generated $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') · installed $installedAgo</span>
</header>
<main>
  <h2>Live</h2>
  $live_html

  <h2>Persistent state</h2>
  <div class=card>$state_html</div>

  <h2>Last 7 days — unlocked minutes</h2>
  <div class=card><canvas id=chartMins></canvas></div>

  <h2>Last 7 days — unlock sessions</h2>
  <div class=card><canvas id=chartCnt></canvas></div>

  <h2>Recent unlocks</h2>
  <div class=card>
    <table><thead><tr><th>when</th><th>event</th></tr></thead>
    <tbody>$rows</tbody></table>
  </div>

  <h2>Service log tail (40 lines)</h2>
  <div class=card><pre>$svcTail</pre></div>
</main>
<footer>Exercise Unlock · offline dashboard · re-run tools\dashboard.ps1 to refresh</footer>

<script>
// Tiny canvas bar chart — no libraries, no CDN.
function bars(canvasId, labels, values, color) {
    const c = document.getElementById(canvasId);
    if (!c) return;
    const w = c.clientWidth * devicePixelRatio;
    const h = c.clientHeight * devicePixelRatio;
    c.width = w; c.height = h;
    const g = c.getContext('2d');
    g.scale(1, 1);
    const pad = 24 * devicePixelRatio;
    const iw = w - pad * 2;
    const ih = h - pad * 2;
    const n = values.length;
    const max = Math.max(1, ...values) * 1.15;
    const bw = iw / n * 0.75;
    const gap = iw / n * 0.25;
    g.fillStyle = '#8a8a94';
    g.font = (11 * devicePixelRatio) + 'px system-ui';
    g.textAlign = 'center';
    for (let i = 0; i < n; i++) {
        const v = values[i];
        const bh = ih * (v / max);
        const x = pad + i * (bw + gap);
        const y = pad + ih - bh;
        g.fillStyle = color;
        g.fillRect(x, y, bw, bh);
        g.fillStyle = '#ececef';
        g.fillText(String(v), x + bw / 2, y - 4 * devicePixelRatio);
        g.fillStyle = '#8a8a94';
        g.fillText(labels[i], x + bw / 2, h - 6 * devicePixelRatio);
    }
}
bars('chartMins', [$chartLabels], [$chartMins], '#22d3ee');
bars('chartCnt',  [$chartLabels], [$chartCounts], '#8b5cf6');
</script>
</body>
</html>
"@

$html | Out-File -FilePath $Out -Encoding UTF8
Write-Host "wrote $Out"
if ($Open) { Start-Process $Out }
