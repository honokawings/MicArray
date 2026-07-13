param(
    [string]$OutputPath = (Join-Path $PSScriptRoot 'assets\mock_video_1920x1080.jpg')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$width = 1920
$height = 1080
$quality = 88L
$outputDirectory = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

$bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$gridPen = $null
$crossPen = $null
$borderPen = $null
$titleFont = $null
$detailFont = $null
$whiteBrush = $null
$panelBrush = $null
$encoderParameters = $null
$qualityParameter = $null

try {
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([System.Drawing.Color]::FromArgb(18, 20, 24))

    $barColors = @(
        [System.Drawing.Color]::White,
        [System.Drawing.Color]::Yellow,
        [System.Drawing.Color]::Cyan,
        [System.Drawing.Color]::Lime,
        [System.Drawing.Color]::Magenta,
        [System.Drawing.Color]::Red,
        [System.Drawing.Color]::Blue
    )
    $barWidth = [int]($width / $barColors.Count)
    for ($index = 0; $index -lt $barColors.Count; ++$index) {
        $brush = New-Object System.Drawing.SolidBrush($barColors[$index])
        try {
            $left = $index * $barWidth
            $right = if ($index -eq $barColors.Count - 1) { $width } else { ($index + 1) * $barWidth }
            $graphics.FillRectangle($brush, $left, 0, $right - $left, 650)
        }
        finally {
            $brush.Dispose()
        }
    }

    for ($index = 0; $index -lt 9; ++$index) {
        $level = [int](255 * $index / 8)
        $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($level, $level, $level))
        try {
            $left = [int]($index * $width / 9)
            $right = [int](($index + 1) * $width / 9)
            $graphics.FillRectangle($brush, $left, 650, $right - $left, 210)
        }
        finally {
            $brush.Dispose()
        }
    }

    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(225, 18, 20, 24))
    $graphics.FillRectangle($panelBrush, 0, 860, $width, 220)

    $gridPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(105, 255, 255, 255), 2)
    for ($x = 0; $x -le $width; $x += 120) {
        $graphics.DrawLine($gridPen, $x, 0, $x, $height)
    }
    for ($y = 0; $y -le $height; $y += 120) {
        $graphics.DrawLine($gridPen, 0, $y, $width, $y)
    }

    $crossPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 6)
    $graphics.DrawLine($crossPen, 840, 540, 1080, 540)
    $graphics.DrawLine($crossPen, 960, 420, 960, 660)
    $graphics.DrawEllipse($crossPen, 900, 480, 120, 120)

    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 8)
    $graphics.DrawRectangle($borderPen, 4, 4, $width - 9, $height - 9)

    $titleFont = New-Object System.Drawing.Font([System.Drawing.FontFamily]::GenericSansSerif, 52, [System.Drawing.FontStyle]::Bold)
    $detailFont = New-Object System.Drawing.Font([System.Drawing.FontFamily]::GenericMonospace, 34, [System.Drawing.FontStyle]::Regular)
    $whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $graphics.DrawString('MIC ARRAY VIDEO TEST', $titleFont, $whiteBrush, 72, 885)
    $graphics.DrawString('1920x1080', $detailFont, $whiteBrush, 72, 980)
    $graphics.DrawString('16:9  JPEG  COLOR / GRAY / GRID', $detailFont, $whiteBrush, 1180, 985)

    $jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
        Where-Object { $_.MimeType -eq 'image/jpeg' } |
        Select-Object -First 1
    if ($null -eq $jpegCodec) {
        throw 'JPEG encoder is unavailable'
    }

    $encoderParameters = New-Object System.Drawing.Imaging.EncoderParameters(1)
    $qualityParameter = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, $quality)
    $encoderParameters.Param[0] = $qualityParameter
    $bitmap.Save($OutputPath, $jpegCodec, $encoderParameters)
}
finally {
    if ($null -ne $qualityParameter) { $qualityParameter.Dispose() }
    if ($null -ne $encoderParameters) { $encoderParameters.Dispose() }
    if ($null -ne $whiteBrush) { $whiteBrush.Dispose() }
    if ($null -ne $panelBrush) { $panelBrush.Dispose() }
    if ($null -ne $detailFont) { $detailFont.Dispose() }
    if ($null -ne $titleFont) { $titleFont.Dispose() }
    if ($null -ne $borderPen) { $borderPen.Dispose() }
    if ($null -ne $crossPen) { $crossPen.Dispose() }
    if ($null -ne $gridPen) { $gridPen.Dispose() }
    $graphics.Dispose()
    $bitmap.Dispose()
}

$asset = Get-Item -LiteralPath $OutputPath
Write-Host "Generated $($asset.FullName) ($($asset.Length) bytes, ${width}x${height})"
