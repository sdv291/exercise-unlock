#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Prepare %ProgramData%\ExerciseUnlock for custom lock-screen sounds.

.DESCRIPTION
    Does the one-time setup needed before the credential provider can
    play custom WAVs on the sign-in / lock desktop:

      1. Creates C:\ProgramData\ExerciseUnlock\sounds\ if it doesn't
         already exist.
      2. Copies sounds.ini.sample from the project into
         C:\ProgramData\ExerciseUnlock\sounds.ini unless a file is
         already there (edits are preserved on reruns).
      3. Grants NT AUTHORITY\SYSTEM read access on sounds.ini and
         (inheritable) on the sounds\ directory.

    The ACL step is the key one. ProgramData child files inherit from
    the parent dir's DACL; on at least one install we saw the parent
    only grant the admin user, so a user-created sounds.ini ended up
    sdv29-only and the SYSTEM-hosted LogonUI couldn't read it.
    Service RunUntilStopped now runs the same ACL fix at every startup,
    so this script is a one-shot convenience for first-time setup.

    After this, drop .wav files into the sounds\ folder, point the keys
    in sounds.ini at them, and lock the screen — PlaySound in LogonUI
    picks the configured files up on the next poll (~500ms).

.PARAMETER SampleIni
    Alternate path to sounds.ini.sample. Default: resolved relative
    to this script.
#>

param(
    [string]$SampleIni = (Join-Path (Split-Path -Parent $PSScriptRoot) `
                                    'ExerciseUnlockProvider\sounds.ini.sample')
)

$ErrorActionPreference = 'Stop'

# Data folder: <InstallDir>\data for installed builds (HKLM\SOFTWARE\ExerciseUnlock),
# the legacy C:\ProgramData\ExerciseUnlock for dev deployments - same rule as Paths.h.
$installDir = (Get-ItemProperty 'HKLM:\SOFTWARE\ExerciseUnlock' -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
$dataDir = if ($installDir) { Join-Path $installDir 'data' } else { 'C:\ProgramData\ExerciseUnlock' }
$base      = $dataDir
$soundsDir = Join-Path $base 'sounds'
$ini       = Join-Path $base 'sounds.ini'

# 1. Directories.
if (-not (Test-Path $base))      { New-Item -ItemType Directory -Path $base      | Out-Null }
if (-not (Test-Path $soundsDir)) { New-Item -ItemType Directory -Path $soundsDir | Out-Null }
Write-Host "sounds dir: $soundsDir"

# 2. Seed sounds.ini on first run.
if (-not (Test-Path $ini))
{
    if (-not (Test-Path $SampleIni))
    {
        throw "sample ini not found at '$SampleIni' — pass -SampleIni with the right path."
    }
    Copy-Item -LiteralPath $SampleIni -Destination $ini
    Write-Host "seeded sounds.ini from $SampleIni"
}
else
{
    Write-Host "sounds.ini already exists — not overwriting."
}

# 3. SYSTEM read access.
Write-Host "granting SYSTEM read access..."
icacls $ini       /grant 'SYSTEM:(R)'      /Q | Out-Null
icacls $soundsDir /grant 'SYSTEM:(OI)(CI)(R)' /T /Q | Out-Null

# 4. Report so the admin can spot a problem fast.
Write-Host ""
Write-Host "Verification:"
$aclIni = (Get-Acl $ini).Access |
    Where-Object { $_.IdentityReference -like '*SYSTEM*' }
if ($aclIni)
{
    Write-Host "  sounds.ini SYSTEM access: $($aclIni.FileSystemRights)"
}
else
{
    Write-Host "  WARNING: SYSTEM access on sounds.ini is still missing. Try again elevated."
}
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Drop .wav files into $soundsDir"
Write-Host "  2. Edit $ini to point each key at a file (bare name = relative to library)"
Write-Host "  3. Lock the screen and test. No service restart needed."
