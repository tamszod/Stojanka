# Renders the Strojanka icon (same shapes as assets/icon/strojanka.svg) into:
#   assets/icon/png/strojanka-<size>.png   16 ... 1024
#   assets/icon/strojanka.ico               Windows (PNG-compressed entries, 16-256)
#   assets/icon/strojanka.icns              macOS (PNG entries, 16-1024)
# Run on Windows:  powershell -ExecutionPolicy Bypass -File tools\make_icons.ps1
# The outputs are committed, so normal builds (including macOS) do not need this script.

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'assets\icon'
$pngDir = Join-Path $out 'png'
New-Item -ItemType Directory -Force $pngDir | Out-Null

function New-Color([string]$hex, [int]$alpha = 255) {
    $c = [System.Drawing.ColorTranslator]::FromHtml($hex)
    return [System.Drawing.Color]::FromArgb($alpha, $c.R, $c.G, $c.B)
}

function Render-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = $size / 100.0
    # Small sizes: use more of the canvas and thicker strokes so the shape stays readable.
    $small = $size -le 32
    $inset = if ($small) { 1 } else { 4 }

    # Background tile with rounded corners and diagonal gradient.
    $x = $inset * $s; $w = (100 - 2 * $inset) * $s; $r = 22 * $s
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x, $x, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($x + $w - 2 * $r, $x, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($x + $w - 2 * $r, $x + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($x, $x + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($x, $x)), (New-Object System.Drawing.PointF(($x + $w), ($x + $w))),
        (New-Color '#1E1B4B'), (New-Color '#0E7490'))
    $g.FillPath($brush, $path)

    # Support / resistance levels (only where there is room for them).
    if ($size -ge 48) {
        $dash = New-Object System.Drawing.Pen((New-Color '#FFFFFF' 56), (2 * $s))
        $dash.DashPattern = @(2.0, 2.0)
        $g.DrawLine($dash, 16 * $s, 27 * $s, 84 * $s, 27 * $s)
        $g.DrawLine($dash, 16 * $s, 64 * $s, 84 * $s, 64 * $s)
    }

    # Price swings.
    $lineWidth = if ($small) { 10 } else { 7 }
    $pen = New-Object System.Drawing.Pen((New-Color '#FFFFFF'), ($lineWidth * $s))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF((17 * $s), (74 * $s))),
        (New-Object System.Drawing.PointF((37 * $s), (38 * $s))),
        (New-Object System.Drawing.PointF((55 * $s), (64 * $s))),
        (New-Object System.Drawing.PointF((79 * $s), (27 * $s))))
    $g.DrawLines($pen, $pts)

    # SUPPORT (green) and PEAK (amber) markers.
    $dotR = if ($small) { 10 } else { 7.5 }
    $ring = New-Object System.Drawing.Pen((New-Color '#FFFFFF'), (3 * $s))
    foreach ($m in @(@(55, 64, '#34D399'), @(79, 27, '#FBBF24'))) {
        $cx = $m[0] * $s; $cy = $m[1] * $s; $rr = $dotR * $s
        $g.FillEllipse((New-Object System.Drawing.SolidBrush((New-Color $m[2]))), ($cx - $rr), ($cy - $rr), (2 * $rr), (2 * $rr))
        if (-not $small) { $g.DrawEllipse($ring, ($cx - $rr), ($cy - $rr), (2 * $rr), (2 * $rr)) }
    }

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256, 512, 1024
$pngs = @{}
foreach ($size in $sizes) {
    $pngs[$size] = Render-Icon $size
    [System.IO.File]::WriteAllBytes((Join-Path $pngDir "strojanka-$size.png"), $pngs[$size])
}

# ICO: 6-byte header, 16-byte directory entries, then PNG data. Width/height 0 means 256.
$icoSizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$ico = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ico)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$icoSizes.Count)
$offset = 6 + 16 * $icoSizes.Count
foreach ($size in $icoSizes) {
    $dim = if ($size -ge 256) { 0 } else { $size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$size].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$size].Length
}
foreach ($size in $icoSizes) { $bw.Write($pngs[$size]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $out 'strojanka.ico'), $ico.ToArray())

# ICNS: 'icns' + big-endian total length, then (type, big-endian length incl. 8-byte header, PNG).
function BE32([int]$v) { $b = [BitConverter]::GetBytes([UInt32]$v); [Array]::Reverse($b); return , $b }
$icnsTypes = [ordered]@{ 'icp4' = 16; 'icp5' = 32; 'icp6' = 64; 'ic07' = 128; 'ic08' = 256; 'ic09' = 512; 'ic10' = 1024 }
$body = New-Object System.IO.MemoryStream
foreach ($type in $icnsTypes.Keys) {
    $data = $pngs[$icnsTypes[$type]]
    $body.Write([Text.Encoding]::ASCII.GetBytes($type), 0, 4)
    $body.Write((BE32 ($data.Length + 8)), 0, 4)
    $body.Write($data, 0, $data.Length)
}
$icns = New-Object System.IO.MemoryStream
$icns.Write([Text.Encoding]::ASCII.GetBytes('icns'), 0, 4)
$icns.Write((BE32 ($body.Length + 8)), 0, 4)
$body.WriteTo($icns)
[System.IO.File]::WriteAllBytes((Join-Path $out 'strojanka.icns'), $icns.ToArray())

Write-Output "icons written to $out"
