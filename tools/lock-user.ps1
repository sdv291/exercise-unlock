#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Prevent the child account from adding alternative sign-in methods
    (PIN, Windows Hello, picture password) or changing their own
    password, so the Exercise Unlock tile is the only sign-in path.

.DESCRIPTION
    Combines several Windows restrictions:

      1. Disables "Convenience PIN sign-in" via local Group Policy
         (HKLM\...\Policies\...\System\AllowDomainPINLogon = 0 AND
         HKLM\...\Policies\PassportForWork\Enabled = 0). This blocks
         PIN creation in Settings → Accounts → Sign-in options AND
         prevents an existing PIN from being used on the lock screen.
         Applies to ALL users on the machine — a parent re-enabling
         it for themselves is a one-line GPO toggle.

      2. Disables non-password credential-provider tiles on the
         sign-in / lock screen (PIN, face, fingerprint, picture
         password, smart card) via Credential Provider Filters.
         Our tile stays, and the standard password tile stays so a
         parent can still type the password manually in a pinch.

      3. Marks the child account "user cannot change password" (SAM
         flag UF_PASSWD_CANT_CHANGE). Via net user /passwordchg:no.
         Removes the "Change a password..." option from Ctrl+Alt+Del.

    Changes are reversible — run with -Undo to restore defaults.

.PARAMETER User
    Local username to restrict. Required - there is no default; when
    omitted, PowerShell asks for it.

.PARAMETER Undo
    Reverse all restrictions.

.EXAMPLE
    .\lock-user.ps1 -User kid
    .\lock-user.ps1 -User kid -Undo
#>

param(
    [Parameter(Mandatory = $true, HelpMessage = 'Local account to restrict (the child account)')]
    [ValidateNotNullOrEmpty()]
    [string]$User,
    [switch]$Undo
)

$ErrorActionPreference = 'Stop'

# A typo would otherwise apply the machine-wide policies below and only
# then fail on the account itself. (-Undo still runs for a deleted account.)
& net user $User *> $null
if ($LASTEXITCODE -ne 0)
{
    if (-not $Undo) { throw "Local account '$User' not found - nothing changed." }
    Write-Host "warning: local account '$User' not found - restoring machine-wide settings only"
}

# --- Credential provider CLSIDs to disable on lock/sign-in screen ---
# Keep PasswordCredentialProvider enabled as a parent escape hatch.
# Ordered hashtable so output lists the same way every run; some
# of these keys are owned by TrustedInstaller and refuse a plain
# admin write — those are logged and skipped (GPO pass above
# already covers PIN at policy level, which is the real block).
$providersToDisable = [ordered]@{
    'PIN (NGC)'          = '{cb82ea12-9f71-446d-89e1-8d0924e1256e}'
    'PIN (NGC login)'    = '{D6886603-9D2F-4EB2-B667-1971041FA96B}'
    'Face / Fingerprint' = '{BEC09223-B018-416D-A0AC-523971B639F5}'
    'Picture Password'   = '{2135F72A-90B5-4ED3-A7F1-8BB705AC276A}'
    'Smart Card'         = '{1B283861-754F-4022-AD47-A5EAAA618894}'
    'Windows Hello'      = '{8AF662BF-65A0-4D0A-A540-A338A999D36F}'
}

function Set-RegDword($path, $name, $value)
{
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    Set-ItemProperty -Path $path -Name $name -Value $value -Type DWord
}

# Try to set a DWORD under a credential-provider key, surviving the
# "Requested registry access is not allowed" that TrustedInstaller-
# owned keys throw at a plain admin. Logs result either way.
function Try-DisableProvider($name, $clsid)
{
    $base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers'
    $key  = Join-Path $base $clsid
    try {
        Set-RegDword $key 'Disabled' 1
        Write-Host "        $name disabled"
        return $true
    } catch {
        Write-Host "        $name — skipped (OS-protected: $($_.Exception.Message.Trim()))"
        return $false
    }
}

function Try-RestoreProvider($name, $clsid)
{
    $base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers'
    $key  = Join-Path $base $clsid
    try {
        if (Test-Path $key) {
            Remove-ItemProperty -Path $key -Name 'Disabled' -ErrorAction Stop
        }
        Write-Host "        $name restored"
    } catch {
        Write-Host "        $name — nothing to restore"
    }
}

function Remove-RegValue($path, $name)
{
    if (Test-Path $path) {
        Remove-ItemProperty -Path $path -Name $name -ErrorAction SilentlyContinue
    }
}

if (-not $Undo)
{
    Write-Host "Locking account '$User' to Exercise Unlock tile only..."
    Write-Host ""

    # 1. Block convenience PIN sign-in via policy. Both keys together
    #    cover Windows 10 and 11 variations.
    Write-Host "  [1/3] Disabling PIN sign-in policy..."
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                 'AllowDomainPINLogon' 0
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\PassportForWork' `
                 'Enabled' 0
    # Also hide the Sign-in options tiles so the user can't even
    # see other methods were available.
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                 'BlockDomainPicturePassword' 1
    Set-RegDword 'HKLM:\SOFTWARE\Microsoft\PolicyManager\default\Settings\AllowSignInOptions' `
                 'value' 0

    # 2. Disable non-password credential providers at the sign-in
    #    screen. The filters live under
    #    HKLM\...\Authentication\Credential Provider Filters\{CLSID}
    #    with a "Disabled" DWORD = 1.
    Write-Host "  [2/3] Hiding alternative sign-in tiles..."
    foreach ($name in $providersToDisable.Keys)
    {
        [void](Try-DisableProvider $name $providersToDisable[$name])
    }

    # 3. Prevent the account from changing its own password.
    #    The native tool `net user` sets the UF_PASSWD_CANT_CHANGE
    #    flag in SAM.
    Write-Host "  [3/3] Blocking '$User' password change..."
    & net user $User /passwordchg:no | Out-Null
    if ($LASTEXITCODE -ne 0)
    {
        Write-Host "        warning: 'net user $User' failed (account missing?)"
    }

    Write-Host ""
    Write-Host "Done. Changes take effect at next sign-in / lock screen."
    Write-Host "Reverse with: pwsh -File `"$PSCommandPath`" -User '$User' -Undo"
}
else
{
    Write-Host "Reverting restrictions for account '$User'..."
    Write-Host ""

    Write-Host "  [1/3] Re-enabling PIN sign-in policy..."
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                    'AllowDomainPINLogon'
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\PassportForWork' `
                    'Enabled'
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                    'BlockDomainPicturePassword'
    Remove-RegValue 'HKLM:\SOFTWARE\Microsoft\PolicyManager\default\Settings\AllowSignInOptions' `
                    'value'

    Write-Host "  [2/3] Re-enabling alternative sign-in tiles..."
    foreach ($name in $providersToDisable.Keys)
    {
        Try-RestoreProvider $name $providersToDisable[$name]
    }

    Write-Host "  [3/3] Allowing '$User' to change password..."
    & net user $User /passwordchg:yes | Out-Null

    Write-Host ""
    Write-Host "Done. Restrictions removed; defaults restored."
}
