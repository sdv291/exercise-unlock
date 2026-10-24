# Renders branding\app-icon.svg into the app's icon files:
#   branding\app.ico            16-256 px, PNG entries (installer, shortcuts, exes)
#   branding\png\app-<n>.png    the same sizes as files
#   branding\png\wizard-<n>.png Inno Setup's small wizard image (55 px at 100 % DPI)
# Needs Microsoft Edge (headless) - run by hand after changing the SVG;
# the outputs are committed, the installer build only reads them.
#   pwsh -File branding\render-icons.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$svg = Join-Path $root 'app-icon.svg'
$out = Join-Path $root 'png'
New-Item -ItemType Directory -Force $out | Out-Null

$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
          "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found' }

# Each size straight from the vector, on a transparent page of exactly
# that size, so small icons stay crisp.
$tmp = Join-Path ([IO.Path]::GetTempPath()) "eu-icons-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$svgText = Get-Content $svg -Raw
function Render([int]$size, [string]$file, [int]$canvas = 0)
{
    if ($canvas -eq 0) { $canvas = $size }
    $pad = [int](($canvas - $size) / 2)
    $inner = $svgText -replace 'width="256" height="256"', "width=`"$size`" height=`"$size`""
    $html = "<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;overflow:hidden}" +
            "svg{position:absolute;left:${pad}px;top:${pad}px}</style></head><body>$inner</body></html>"
    $page = Join-Path $tmp "icon-$size-$canvas.html"
    Set-Content $page $html -Encoding utf8
    & $edge --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
        --default-background-color=00000000 --window-size="$canvas,$canvas" `
        --screenshot="$file" ([Uri]$page).AbsoluteUri 2>$null | Out-Null
    for ($i = 0; $i -lt 50 -and -not (Test-Path $file); $i++) { Start-Sleep -Milliseconds 100 }
    if (-not (Test-Path $file)) { throw "Edge did not render $file" }
}

$sizes = 256, 128, 64, 48, 40, 32, 24, 20, 16
foreach ($s in $sizes) { Render $s (Join-Path $out "app-$s.png") }

# Wizard corner image: the icon with a little air around it, at the
# DPI steps Inno Setup picks from (100/125/150/200/250 %).
foreach ($s in 55, 69, 83, 110, 138) { Render ([int]($s * 0.86)) (Join-Path $out "wizard-$s.png") $s }

# app.ico: PNG-compressed entries (Vista+), largest first.
$ico = Join-Path $root 'app.ico'
$pngs = foreach ($s in $sizes) { , [IO.File]::ReadAllBytes((Join-Path $out "app-$s.png")) }
$fs = [IO.File]::Create($ico)
$w = [IO.BinaryWriter]::new($fs)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++)
{
    $dim = [byte]($sizes[$i] % 256)          # 256 is stored as 0
    $w.Write($dim); $w.Write($dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Dispose()
Remove-Item $tmp -Recurse -Force
Write-Host "icons: $ico + $out"
