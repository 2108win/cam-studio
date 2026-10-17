# Draws the Cam Studio icon (camera body + lens on a Windows-blue tile) and
# writes app\Assets\CamStudio.ico (PNG-compressed, 16-256 px) plus a 256 px PNG.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$assets = Join-Path $root "app\Assets"
New-Item -ItemType Directory -Force $assets | Out-Null

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconBitmap([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.Clear([System.Drawing.Color]::Transparent)

    # Tile with a soft vertical gradient.
    $tile = New-RoundRect 0 0 $s $s ($s * 0.22)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new(0, $s)), ([System.Drawing.Color]::FromArgb(255, 76, 160, 255)), ([System.Drawing.Color]::FromArgb(255, 0, 95, 204))
    $g.FillPath($grad, $tile)

    # Camera body + viewfinder wedge.
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $body = New-RoundRect ($s * 0.14) ($s * 0.30) ($s * 0.50) ($s * 0.40) ($s * 0.09)
    $g.FillPath($white, $body)
    $wedge = [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new($s * 0.66, $s * 0.44),
        [System.Drawing.PointF]::new($s * 0.86, $s * 0.33),
        [System.Drawing.PointF]::new($s * 0.86, $s * 0.67),
        [System.Drawing.PointF]::new($s * 0.66, $s * 0.56))
    $g.FillPolygon($white, $wedge)

    # Lens.
    $lens = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 95, 204))
    $r = $s * 0.115
    $g.FillEllipse($lens, $s * 0.39 - $r, $s * 0.50 - $r, 2 * $r, 2 * $r)
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $bmp.Save((Join-Path $assets "CamStudio.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    , $ms.ToArray()
}

# ICO container: header, one directory entry per size, then the PNG blobs.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256))   # 256 is stored as 0
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
[System.IO.File]::WriteAllBytes((Join-Path $assets "CamStudio.ico"), $out.ToArray())
Write-Host "Wrote $assets\CamStudio.ico"
