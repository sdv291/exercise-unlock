#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Removes a manual / developer deployment of Exercise Unlock (service
    registered from x64\Release, provider DLL copied by redeploy.ps1,
    Register.reg, lock-user.ps1 restrictions, ProgramData state) so the
    installer can start from a clean slate.

.PARAMETER User
    Account lock-user.ps1 was applied to. Required; asked for when omitted.

.PARAMETER KeepData
    Keep C:\ProgramData\ExerciseUnlock (vault, counters, sounds).
#>

param(
    [Parameter(Mandatory = $true, HelpMessage = 'Account lock-user.ps1 was applied to (the child account)')]
    [ValidateNotNullOrEmpty()]
    [string]$User,
    [switch]$KeepData
)

$svc   = 'ExerciseUnlockService'
$clsid = '{8FDE7B3A-3E5A-4F1C-9C88-6B5F1B7D9E20}'

Write-Host "[1/5] Service..."
if (Get-Service -Name $svc -ErrorAction SilentlyContinue)
{
    Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
    & sc.exe delete $svc | Out-Null
    Write-Host "      removed"
}
else { Write-Host "      not installed" }

Write-Host "[2/5] Credential provider..."
Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\$clsid" -Recurse -ErrorAction SilentlyContinue
Remove-Item "HKLM:\SOFTWARE\Classes\CLSID\$clsid" -Recurse -ErrorAction SilentlyContinue
$dll = Join-Path $env:windir 'System32\ExerciseUnlockProvider.dll'
if (Test-Path $dll)
{
    try   { Remove-Item $dll -Force -ErrorAction Stop; Write-Host "      registration + DLL removed" }
    catch { Write-Host "      registration removed; DLL is in use (lock screen) - reboot, the installer overwrites it anyway" }
}
else { Write-Host "      registration removed" }

Write-Host "[3/5] Sign-in restrictions for '$User' (lock-user -Undo)..."
& (Join-Path $PSScriptRoot 'lock-user.ps1') -User $User -Undo | Out-Null
Write-Host "      PIN / Windows Hello restored, password change allowed"

Write-Host "[4/5] Data (vault, counters, sounds)..."
if ($KeepData) { Write-Host "      kept (-KeepData)" }
else
{
    Remove-Item 'C:\ProgramData\ExerciseUnlock' -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "      C:\ProgramData\ExerciseUnlock removed"
}

Write-Host "[5/5] Debug files in C:\Users\Public..."
Remove-Item 'C:\Users\Public\exerciseunlock-provider.log', 'C:\Users\Public\eu-service-trace.log',
            'C:\Users\Public\ExerciseUnlockWebcam.bmp' -ErrorAction SilentlyContinue
Write-Host "      removed (skeleton-viewer snapshots in C:\Users\Public\ExerciseUnlockSnapshots were left alone)"

Write-Host ""
Write-Host "Done. Reboot, then run installer\out\ExerciseUnlock-Setup-*.exe"
