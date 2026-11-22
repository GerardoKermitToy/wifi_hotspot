param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\resources\wifi.ico')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$images = [System.Collections.Generic.List[byte[]]]::new()
$sizes = @(32, 48, 256)
foreach ($size in $sizes) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $scale = $size / 64.0
    $g.ScaleTransform($scale, $scale)
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc(1, 1, 18, 18, 180, 90)
    $path.AddArc(45, 1, 18, 18, 270, 90)
    $path.AddArc(45, 45, 18, 18, 0, 90)
    $path.AddArc(1, 45, 18, 18, 90, 90)
    $path.CloseFigure()
    $background = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(8,119,133))
    $g.FillPath($background, $path)
    $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, 4)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($pen, 9, 13, 46, 46, 222, 96)
    $g.DrawArc($pen, 18, 23, 28, 28, 222, 96)
    $g.FillEllipse([System.Drawing.Brushes]::White, 28, 40, 8, 8)
    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images.Add($stream.ToArray())
    $stream.Dispose(); $pen.Dispose(); $background.Dispose(); $path.Dispose(); $g.Dispose(); $bitmap.Dispose()
}
$fullPath = [System.IO.Path]::GetFullPath($OutputPath)
$output = [System.IO.File]::Create($fullPath)
$writer = [System.IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($image in $images) { $writer.Write($image) }
} finally { $writer.Dispose() }
Write-Output "Created icon: $fullPath"
