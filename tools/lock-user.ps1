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

      4. Takes "Change the time zone" (SeTimeZonePrivilege) from
         BUILTIN\Users. The schedule and the daily reset go by local
         time, and by default every standard account may move the time
         zone - which shifts the allowed hours. Administrators and
         LOCAL SERVICE (automatic time zone) keep it. Applies to ALL
         standard accounts, from their next sign-in.

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

# A policy key the lock created and -Undo emptied: gone too, when nothing
# else (a real policy) is in it.
function Remove-EmptyKey($path)
{
    if (-not (Test-Path $path)) { return }
    $key = Get-Item -Path $path
    if ($key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0) {
        Remove-Item -Path $path -ErrorAction SilentlyContinue
    }
}

# Windows' own default for the "Sign-in options" page (value 1 = allowed),
# with the policy's other fields next to it. Step 1 sets it to 0; -Undo
# puts 1 back - deleting it (1.4.0 and older) left that page "managed by
# your organization" for every account after the uninstall.
$signInOptionsKey = 'HKLM:\SOFTWARE\Microsoft\PolicyManager\default\Settings\AllowSignInOptions'

# --- Step 4: the time zone right of BUILTIN\Users ---
# -Undo gives it back only when this script took it (the marker value):
# a PC where a policy had already removed it stays that way.
$usersSid      = 'S-1-5-32-545'
$timeZoneRight = 'SeTimeZonePrivilege'
$markerKey     = 'HKLM:\SOFTWARE\ExerciseUnlock'
$markerName    = 'TimeZoneLocked'

# User rights live in the LSA policy; Windows PowerShell has no cmdlet.
function Add-UserRightsType
{
    if ('EuUserRights' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Principal;

public static class EuUserRights
{
    [StructLayout(LayoutKind.Sequential)]
    struct LSA_UNICODE_STRING { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }

    [StructLayout(LayoutKind.Sequential)]
    struct LSA_OBJECT_ATTRIBUTES
    {
        public int Length; public IntPtr RootDirectory; public IntPtr ObjectName;
        public uint Attributes; public IntPtr SecurityDescriptor; public IntPtr SecurityQualityOfService;
    }

    [DllImport("advapi32.dll")]
    static extern uint LsaOpenPolicy(IntPtr systemName, ref LSA_OBJECT_ATTRIBUTES attributes, uint access, out IntPtr policy);
    [DllImport("advapi32.dll")]
    static extern uint LsaEnumerateAccountRights(IntPtr policy, byte[] sid, out IntPtr rights, out uint count);
    [DllImport("advapi32.dll")]
    static extern uint LsaAddAccountRights(IntPtr policy, byte[] sid, LSA_UNICODE_STRING[] rights, uint count);
    [DllImport("advapi32.dll")]
    static extern uint LsaRemoveAccountRights(IntPtr policy, byte[] sid, [MarshalAs(UnmanagedType.U1)] bool allRights,
                                              LSA_UNICODE_STRING[] rights, uint count);
    [DllImport("advapi32.dll")]
    static extern uint LsaFreeMemory(IntPtr buffer);
    [DllImport("advapi32.dll")]
    static extern uint LsaClose(IntPtr policy);
    [DllImport("advapi32.dll")]
    static extern int LsaNtStatusToWinError(uint status);

    const uint POLICY_CREATE_ACCOUNT = 0x10, POLICY_LOOKUP_NAMES = 0x800;
    const uint STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;

    static void Check(uint status)
    {
        if (status != 0) throw new Win32Exception(LsaNtStatusToWinError(status));
    }

    static IntPtr Open(uint access)
    {
        LSA_OBJECT_ATTRIBUTES attributes = new LSA_OBJECT_ATTRIBUTES();
        attributes.Length = Marshal.SizeOf(typeof(LSA_OBJECT_ATTRIBUTES));
        IntPtr policy;
        Check(LsaOpenPolicy(IntPtr.Zero, ref attributes, access, out policy));
        return policy;
    }

    static byte[] Sid(string sid)
    {
        SecurityIdentifier id = new SecurityIdentifier(sid);
        byte[] bytes = new byte[id.BinaryLength];
        id.GetBinaryForm(bytes, 0);
        return bytes;
    }

    public static bool Has(string sid, string right)
    {
        IntPtr policy = Open(POLICY_LOOKUP_NAMES);
        try
        {
            IntPtr rights;
            uint count;
            uint status = LsaEnumerateAccountRights(policy, Sid(sid), out rights, out count);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND) return false;   // holds no rights at all
            Check(status);
            try
            {
                int size = Marshal.SizeOf(typeof(LSA_UNICODE_STRING));
                for (int i = 0; i < count; i++)
                {
                    LSA_UNICODE_STRING s = (LSA_UNICODE_STRING)Marshal.PtrToStructure(
                        new IntPtr(rights.ToInt64() + (long)i * size), typeof(LSA_UNICODE_STRING));
                    if (string.Equals(Marshal.PtrToStringUni(s.Buffer, s.Length / 2), right, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                return false;
            }
            finally { LsaFreeMemory(rights); }
        }
        finally { LsaClose(policy); }
    }

    public static void Set(string sid, string right, bool grant)
    {
        LSA_UNICODE_STRING[] rights = new LSA_UNICODE_STRING[1];
        rights[0].Buffer = Marshal.StringToHGlobalUni(right);
        rights[0].Length = (ushort)(right.Length * 2);
        rights[0].MaximumLength = (ushort)((right.Length + 1) * 2);
        IntPtr policy = Open(POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES);
        try
        {
            Check(grant ? LsaAddAccountRights(policy, Sid(sid), rights, 1)
                        : LsaRemoveAccountRights(policy, Sid(sid), false, rights, 1));
        }
        finally
        {
            LsaClose(policy);
            Marshal.FreeHGlobal(rights[0].Buffer);
        }
    }
}
'@
}

if (-not $Undo)
{
    Write-Host "Locking account '$User' to Exercise Unlock tile only..."
    Write-Host ""

    # 1. Block convenience PIN sign-in via policy. Both keys together
    #    cover Windows 10 and 11 variations.
    Write-Host "  [1/4] Disabling PIN sign-in policy..."
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                 'AllowDomainPINLogon' 0
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\PassportForWork' `
                 'Enabled' 0
    # Also hide the Sign-in options tiles so the user can't even
    # see other methods were available.
    Set-RegDword 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                 'BlockDomainPicturePassword' 1
    Set-RegDword $signInOptionsKey 'value' 0

    # 2. Disable non-password credential providers at the sign-in
    #    screen. The filters live under
    #    HKLM\...\Authentication\Credential Provider Filters\{CLSID}
    #    with a "Disabled" DWORD = 1.
    Write-Host "  [2/4] Hiding alternative sign-in tiles..."
    foreach ($name in $providersToDisable.Keys)
    {
        [void](Try-DisableProvider $name $providersToDisable[$name])
    }

    # 3. Prevent the account from changing its own password.
    #    The native tool `net user` sets the UF_PASSWD_CANT_CHANGE
    #    flag in SAM.
    Write-Host "  [3/4] Blocking '$User' password change..."
    & net user $User /passwordchg:no | Out-Null
    if ($LASTEXITCODE -ne 0)
    {
        Write-Host "        warning: 'net user $User' failed (account missing?)"
    }

    # 4. Standard accounts can't move the time zone (see the header).
    #    Marker first: if the removal then fails, -Undo only re-grants
    #    a right Users still have.
    Write-Host "  [4/4] Forbidding time zone changes to standard accounts..."
    try {
        Add-UserRightsType
        if ([EuUserRights]::Has($usersSid, $timeZoneRight)) {
            Set-RegDword $markerKey $markerName 1
            [EuUserRights]::Set($usersSid, $timeZoneRight, $false)
            Write-Host "        taken from Users"
        } else {
            Write-Host "        Users don't have it - nothing to do"
        }
    } catch {
        Write-Host "        warning: $($_.Exception.Message.Trim())"
    }

    Write-Host ""
    Write-Host "Done. Changes take effect at next sign-in / lock screen."
    Write-Host "Reverse with: pwsh -File `"$PSCommandPath`" -User '$User' -Undo"
}
else
{
    Write-Host "Reverting restrictions for account '$User'..."
    Write-Host ""

    Write-Host "  [1/4] Re-enabling PIN sign-in policy..."
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                    'AllowDomainPINLogon'
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\PassportForWork' `
                    'Enabled'
    Remove-RegValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System' `
                    'BlockDomainPicturePassword'
    Remove-EmptyKey 'HKLM:\SOFTWARE\Policies\Microsoft\PassportForWork'
    try {
        if (Test-Path $signInOptionsKey) {
            Set-ItemProperty -Path $signInOptionsKey -Name 'value' -Value 1 -Type DWord
        }
    } catch {
        Write-Host "        warning: sign-in options: $($_.Exception.Message.Trim())"
    }

    Write-Host "  [2/4] Re-enabling alternative sign-in tiles..."
    foreach ($name in $providersToDisable.Keys)
    {
        Try-RestoreProvider $name $providersToDisable[$name]
    }

    Write-Host "  [3/4] Allowing '$User' to change password..."
    & net user $User /passwordchg:yes | Out-Null

    Write-Host "  [4/4] Allowing time zone changes again..."
    try {
        $marker = Get-ItemProperty -Path $markerKey -Name $markerName -ErrorAction SilentlyContinue
        if ($marker -and $marker.$markerName -eq 1) {
            Add-UserRightsType
            [EuUserRights]::Set($usersSid, $timeZoneRight, $true)
            Remove-RegValue $markerKey $markerName
            Write-Host "        given back to Users"
        } else {
            Write-Host "        not taken by this script - left as is"
        }
    } catch {
        Write-Host "        warning: $($_.Exception.Message.Trim())"
    }

    Write-Host ""
    Write-Host "Done. Restrictions removed; defaults restored."
}
