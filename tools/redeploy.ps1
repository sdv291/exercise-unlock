# Full rebuild + redeploy loop for the ExerciseUnlockService and its
# provider DLL. Run from an ELEVATED PowerShell — needs admin for
# `sc` and for writing to C:\Windows\System32.
#
# Usage:
#   pwsh -File tools\redeploy.ps1              # Release, build + copy
#   pwsh -File tools\redeploy.ps1 -Config Debug
#   pwsh -File tools\redeploy.ps1 -NoBuild     # just stop/copy/start
#   pwsh -File tools\redeploy.ps1 -NoService   # skip service, DLL only
#
# The script fixes the LNK1104 race by waiting for STOPPED before
# handing off to msbuild, and by unblocking LogonUI's hold on the DLL
# via `sfc`-safe copy semantics (System32 replace works only after the
# lock is released; we retry a few times to ride out the tail).

param(
    [string]$Config = 'Release',
    [switch]$NoBuild,
    [switch]$NoService
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$sln   = Join-Path $root 'ExerciseUnlock.sln'
$out   = Join-Path $root "x64\$Config"
$exe   = Join-Path $out 'ExerciseUnlockService.exe'
$dll   = Join-Path $out 'ExerciseUnlockProvider.dll'
$svc   = 'ExerciseUnlockService'

function Wait-ServiceState([string]$name, [string]$want, [int]$timeoutSec)
{
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline)
    {
        $s = (Get-Service -Name $name -ErrorAction SilentlyContinue)
        if (-not $s)  { return }         # not installed = "stopped"
        if ($s.Status -eq $want) { return }
        Start-Sleep -Milliseconds 250
    }
    throw "timeout waiting for service '$name' to reach '$want' (was $($s.Status))"
}

# 1. Stop service if installed & running.
if (-not $NoService)
{
    $svcObj = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if ($svcObj -and $svcObj.Status -ne 'Stopped')
    {
        Write-Host "stopping $svc..."
        Stop-Service -Name $svc -Force
        Wait-ServiceState $svc 'Stopped' 15
        Write-Host "  stopped."
    }
}

# 2. Build. Retry on LNK1104 — Defender's real-time scan can grab the
#    output file during link. A second attempt almost always sticks.
if (-not $NoBuild)
{
    Write-Host "building ($Config)..."
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars))
    {
        throw "vcvars64.bat not found at $vcvars — edit path in redeploy.ps1"
    }
    $built = $false
    for ($i = 1; $i -le 3 -and -not $built; $i++)
    {
        if ($i -gt 1) { Write-Host "  retry $i..." ; Start-Sleep -Milliseconds 800 }
        & cmd /c "call `"$vcvars`" >nul && msbuild `"$sln`" /p:Configuration=$Config /p:Platform=x64 /nologo /v:minimal /m"
        if ($LASTEXITCODE -eq 0) { $built = $true }
    }
    if (-not $built)
    {
        throw "msbuild failed after 3 attempts (last exit $LASTEXITCODE) — check Defender exclusions for the output folder"
    }
}

# 3. Copy the provider DLL into System32. LogonUI may briefly hold it
#    after a lock-screen session; retry a few times.
Write-Host "copying provider DLL..."
$dst = 'C:\Windows\System32\ExerciseUnlockProvider.dll'
$copied = $false
for ($i = 0; $i -lt 8 -and -not $copied; $i++)
{
    try {
        Copy-Item -Force $dll $dst
        $copied = $true
    } catch {
        Write-Host "  attempt $($i+1): $($_.Exception.Message)"
        Start-Sleep -Milliseconds 500
    }
}
if (-not $copied)
{
    throw "could not overwrite $dst — likely still held by LogonUI. Sign out or reboot, then re-run with -NoBuild -NoService."
}

# 4. Start service.
if (-not $NoService)
{
    if (Get-Service -Name $svc -ErrorAction SilentlyContinue)
    {
        Write-Host "starting $svc..."
        Start-Service -Name $svc
        Wait-ServiceState $svc 'Running' 15
        Write-Host "  running."
    }
    else
    {
        Write-Host "service '$svc' not installed. To install:"
        Write-Host "  sc create $svc binPath= `"$exe`" start= auto DisplayName= `"Exercise Unlock Service`""
    }
}

Write-Host "done."
