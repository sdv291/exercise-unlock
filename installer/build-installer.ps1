# Builds the Exercise Unlock installer:
#   1. compiles the solution (Release x64) into installer\build\bin
#      (a separate OutDir, so a dev service running from x64\Release
#      doesn't lock the outputs),
#   2. stages config.ini / tools / icon / vc_redist.x64.exe under installer\build,
#   3. runs Inno Setup -> installer\out\ExerciseUnlock-Setup-<version>.exe
#
# Usage:
#   pwsh -File installer\build-installer.ps1           # build + package
#   pwsh -File installer\build-installer.ps1 -NoBuild  # package existing build
#
# Needs Visual Studio (C++ workload) and Inno Setup 6
# (winget install JRSoftware.InnoSetup). No admin rights required.

param([switch]$NoBuild)

$ErrorActionPreference = 'Stop'
$here  = $PSScriptRoot
$root  = Split-Path -Parent $here
$stage = Join-Path $here 'build'
$bin   = Join-Path $stage 'bin'

$version = (Select-String -Path (Join-Path $root 'ExerciseUnlockCommon\Version.h') `
            -Pattern 'kVersionString\[\]\s*=\s*L"([^"]+)"').Matches[0].Groups[1].Value
Write-Host "Exercise Unlock $version"

# --- 1. compile -------------------------------------------------------
if (-not $NoBuild)
{
    $vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars))
    {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere)
        {
            $vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vs) { $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
        }
    }
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found - install Visual Studio with the C++ workload" }

    Write-Host "building (Release x64) -> $bin"
    New-Item -ItemType Directory -Force $bin | Out-Null
    $sln = Join-Path $root 'ExerciseUnlock.sln'
    & cmd /c "call `"$vcvars`" >nul && msbuild `"$sln`" /p:Configuration=Release /p:Platform=x64 /p:OutDir=`"$bin\\`" /nologo /v:minimal /m"
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed ($LASTEXITCODE)" }
}

foreach ($f in 'ExerciseUnlockService.exe', 'ExerciseUnlockAdmin.exe', 'ExerciseUnlockProvider.dll', 'ExerciseUnlockCalibrate.exe',
               'onnxruntime.dll', 'movenet_lightning.onnx')
{
    if (-not (Test-Path (Join-Path $bin $f))) { throw "missing build output: $bin\$f" }
}

# --- 2. stage ---------------------------------------------------------
# config.ini ships without the developer's personal camera preference:
# a preferred camera that doesn't exist on the target PC would make the
# service wait for it forever. Encoding (UTF-16 LE) is preserved.
$cfgText = [IO.File]::ReadAllText((Join-Path $root 'ExerciseUnlockService\config.ini'))
$cfgText = [regex]::Replace($cfgText, '(?m)^(\s*preferred\s*=).*$', '$1')
[IO.File]::WriteAllText((Join-Path $stage 'config.ini'), $cfgText, [Text.UnicodeEncoding]::new($false, $true))

# Tools are run by Windows PowerShell 5.1 on the target PC, which reads
# BOM-less files as ANSI (and chokes on the em-dashes inside) - re-save
# them as UTF-8 with BOM.
$tools = Join-Path $stage 'tools'
# Start empty: setup ships tools\*.ps1, so a renamed / dropped script
# must not linger here from an earlier build.
if (Test-Path $tools) { Remove-Item $tools -Recurse -Force }
New-Item -ItemType Directory -Force $tools | Out-Null
foreach ($t in 'lock-user.ps1', 'dashboard.ps1', 'skeleton-viewer.ps1', 'test-pipe.ps1')
{
    $txt = [IO.File]::ReadAllText((Join-Path $root "tools\$t"))
    [IO.File]::WriteAllText((Join-Path $tools $t), $txt, [Text.UTF8Encoding]::new($true))
}

# VC++ runtime for the target PC: every binary (and onnxruntime.dll) links
# the CRT dynamically, so without MSVCP140 / VCRUNTIME140(_1) LogonUI can't
# load the provider and the service won't start. Setup runs the
# redistributable only when the installed runtime is missing or older.
# Taken from the local Visual Studio (VC\Redist\MSVC\<ver>), newest copy
# if there are several.
$vsDirs = @('C:\Program Files\Microsoft Visual Studio\18\Community')
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) { $vsDirs += @(& $vswhere -all -prerelease -products * -property installationPath) }
$redist = $vsDirs | Select-Object -Unique |
    ForEach-Object { Get-ChildItem (Join-Path $_ 'VC\Redist\MSVC\*\vc_redist.x64.exe') -ErrorAction SilentlyContinue } |
    Sort-Object { $v = $_.VersionInfo; [version]::new($v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart) } -Descending |
    Select-Object -First 1
if (-not $redist)
{
    throw "vc_redist.x64.exe not found in <Visual Studio>\VC\Redist\MSVC\*\ (searched: $($vsDirs -join '; ')) - install the Visual Studio C++ workload"
}
Copy-Item $redist.FullName (Join-Path $stage 'vc_redist.x64.exe') -Force
Write-Host "VC++ redistributable $($redist.VersionInfo.FileVersion) <- $($redist.FullName)"

# app.ico: the app icon (branding\app-icon.svg, rendered by
# branding\render-icons.ps1 - the .ico is committed).
Copy-Item (Join-Path $root 'branding\app.ico') (Join-Path $stage 'app.ico') -Force

# --- 3. package -------------------------------------------------------
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found - winget install JRSoftware.InnoSetup" }

& $iscc /Qp "/DAppVersion=$version" "/DStageDir=build" (Join-Path $here 'ExerciseUnlock.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Write-Host "installer: $(Join-Path $here "out\ExerciseUnlock-Setup-$version.exe")"
