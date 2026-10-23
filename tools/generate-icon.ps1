# Generates ExerciseUnlockProvider/tile.bmp — the 128x128 tile image
# LogonUI shows next to the "Exercise Unlock" label. Regenerate any time
# by running: pwsh -File tools/generate-icon.ps1

Add-Type -AssemblyName System.Drawing

$size = 128
$bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

# Background: deep indigo, matches a "night sign-in" mood.
$bg = [System.Drawing.Color]::FromArgb(31, 41, 91)
$g.Clear($bg)

# Simple stylized dumbbell: two side blocks + a bar.
$fg = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(240, 240, 240))
$accent = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(120, 200, 255))

# Bar
$g.FillRectangle($fg, 30, 60, 68, 8)
# Left weight
$g.FillRectangle($fg, 18, 44, 14, 40)
$g.FillRectangle($accent, 10, 52, 8, 24)
# Right weight
$g.FillRectangle($fg, 96, 44, 14, 40)
$g.FillRectangle($accent, 110, 52, 8, 24)

# Small caption "EU" below the dumbbell.
$font = New-Object System.Drawing.Font('Segoe UI', 18, [System.Drawing.FontStyle]::Bold)
$textBrush = [System.Drawing.Brushes]::White
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = [System.Drawing.StringAlignment]::Center
$fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
$rect = New-Object System.Drawing.RectangleF(0, 88, $size, 30)
$g.DrawString('EU', $font, $textBrush, $rect, $fmt)

$out = Join-Path $PSScriptRoot '..\ExerciseUnlockProvider\tile.bmp'
$out = [System.IO.Path]::GetFullPath($out)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Bmp)

$g.Dispose()
$bmp.Dispose()
$fg.Dispose()
$accent.Dispose()
$font.Dispose()

Write-Host "wrote $out"
