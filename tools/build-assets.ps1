$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assetRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\assets'))
function RoundPath([single]$x,[single]$y,[single]$w,[single]$h,[single]$radius) {
    $p = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $d = 2*$radius
    $p.AddArc($x,$y,$d,$d,180,90); $p.AddArc($x+$w-$d,$y,$d,$d,270,90)
    $p.AddArc($x+$w-$d,$y+$h-$d,$d,$d,0,90); $p.AddArc($x,$y+$h-$d,$d,$d,90,90); $p.CloseFigure()
    return ,$p
}
$master = [System.Drawing.Bitmap]::new(512,512)
$g = [System.Drawing.Graphics]::FromImage($master)
$g.SmoothingMode = 'AntiAlias'; $g.Clear([System.Drawing.Color]::Transparent); $g.ScaleTransform(2,2)
$background = RoundPath 8 8 240 240 56
$bgBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(8,8),[System.Drawing.Point]::new(248,248),[System.Drawing.ColorTranslator]::FromHtml('#1d2e45'),[System.Drawing.ColorTranslator]::FromHtml('#0b111e'))
$g.FillPath($bgBrush,$background)
$border = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(60,102,138,164),1)
$g.DrawPath($border,$background)
$scan = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(148,119,232,210),5)
$scan.StartCap = 'Round';$scan.EndCap = 'Round';$scan.LineJoin = 'Round'
$g.DrawLines($scan,[System.Drawing.PointF[]]@([System.Drawing.PointF]::new(34,82),[System.Drawing.PointF]::new(34,53),[System.Drawing.PointF]::new(63,53)))
$g.DrawLines($scan,[System.Drawing.PointF[]]@([System.Drawing.PointF]::new(193,53),[System.Drawing.PointF]::new(222,53),[System.Drawing.PointF]::new(222,82)))
$g.DrawLines($scan,[System.Drawing.PointF[]]@([System.Drawing.PointF]::new(34,181),[System.Drawing.PointF]::new(34,211),[System.Drawing.PointF]::new(63,211)))
$g.DrawLines($scan,[System.Drawing.PointF[]]@([System.Drawing.PointF]::new(193,211),[System.Drawing.PointF]::new(222,211),[System.Drawing.PointF]::new(222,181)))
$folder = [System.Drawing.Drawing2D.GraphicsPath]::new()
$folder.AddBezier(48,85,48,75,53,70,63,70);$folder.AddLine(63,70,91,70);$folder.AddBezier(91,70,96,70,98,72,100,74)
$folder.AddLine(100,74,116,89);$folder.AddLine(116,89,193,89);$folder.AddBezier(193,89,203,89,208,94,208,104)
$folder.AddLine(208,104,208,173);$folder.AddBezier(208,173,208,183,203,188,193,188);$folder.AddLine(193,188,63,188)
$folder.AddBezier(63,188,53,188,48,183,48,173);$folder.CloseFigure()
$backBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(48,70),[System.Drawing.Point]::new(48,188),[System.Drawing.ColorTranslator]::FromHtml('#3dd0ba'),[System.Drawing.ColorTranslator]::FromHtml('#168b88'))
$g.FillPath($backBrush,$folder)
$face = RoundPath 48 103 160 85 15
$faceBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new([System.Drawing.Point]::new(48,103),[System.Drawing.Point]::new(208,188),[System.Drawing.ColorTranslator]::FromHtml('#a0ffe1'),[System.Drawing.ColorTranslator]::FromHtml('#33b9b0'))
$g.FillPath($faceBrush,$face)
$highlight = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(115,226,255,245),2);$g.DrawLine($highlight,64,105,192,105)
$ink = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#153a46'))
foreach($bar in @(@(83,149,14,23),@(115,132,14,40),@(147,115,14,57))) { $shape = RoundPath $bar[0] $bar[1] $bar[2] $bar[3] 5; $g.FillPath($ink,$shape); $shape.Dispose() }
$master.Save((Join-Path $assetRoot 'ScanFolder.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$sizes = @(16,24,32,48,64,128,256);$images = @()
foreach($size in $sizes) { $bitmap=[System.Drawing.Bitmap]::new($size,$size);$gfx=[System.Drawing.Graphics]::FromImage($bitmap);$gfx.InterpolationMode='HighQualityBicubic';$gfx.DrawImage($master,0,0,$size,$size);$stream=[System.IO.MemoryStream]::new();$bitmap.Save($stream,[System.Drawing.Imaging.ImageFormat]::Png);$images+=,@($stream.ToArray());$stream.Dispose();$gfx.Dispose();$bitmap.Dispose() }
$file=[System.IO.File]::Create((Join-Path $assetRoot 'ScanFolder.ico'));$writer=[System.IO.BinaryWriter]::new($file)
$writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$sizes.Count);$offset=6+16*$sizes.Count
for($i=0;$i -lt $sizes.Count;$i++) { $dimension=if($sizes[$i] -eq 256){0}else{$sizes[$i]};$writer.Write([byte]$dimension);$writer.Write([byte]$dimension);$writer.Write([byte]0);$writer.Write([byte]0);$writer.Write([uint16]1);$writer.Write([uint16]32);$writer.Write([uint32]$images[$i].Length);$writer.Write([uint32]$offset);$offset+=$images[$i].Length }
foreach($bytes in $images) { $writer.Write([byte[]]$bytes) };$writer.Dispose();$file.Dispose()
$g.Dispose();$master.Dispose();$background.Dispose();$bgBrush.Dispose();$border.Dispose();$scan.Dispose();$folder.Dispose();$backBrush.Dispose();$face.Dispose();$faceBrush.Dispose();$highlight.Dispose();$ink.Dispose()
