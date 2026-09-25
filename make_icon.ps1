Add-Type -AssemblyName System.Drawing

function New-Master {
  $s = 256
  $bmp = New-Object System.Drawing.Bitmap($s,$s)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.Clear([System.Drawing.Color]::Transparent)

  # Rounded-rect background path
  function RR($x,$y,$w,$h,$r){
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r*2
    $p.AddArc($x,$y,$d,$d,180,90)
    $p.AddArc($x+$w-$d,$y,$d,$d,270,90)
    $p.AddArc($x+$w-$d,$y+$h-$d,$d,$d,0,90)
    $p.AddArc($x,$y+$h-$d,$d,$d,90,90)
    $p.CloseFigure()
    return $p
  }

  $bg = RR 10 10 236 236 52
  $c1 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,46,134,255))
  $c2 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255,91,91,245))
  $lgb = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
    (New-Object System.Drawing.Rectangle(10,10,236,236)),
    [System.Drawing.Color]::FromArgb(255,46,134,255),
    [System.Drawing.Color]::FromArgb(255,120,80,245),
    135)
  $g.FillPath($lgb,$bg)

  $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
  $wp = New-Object System.Drawing.Pen([System.Drawing.Color]::White,12)
  $wp.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $wp.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

  # Trash can handle
  $h = RR 110 58 36 16 8
  $g.FillPath($white,$h)
  # Lid
  $lid = RR 68 74 120 22 10
  $g.FillPath($white,$lid)
  # Body (trapezoid)
  $body = New-Object System.Drawing.Drawing2D.GraphicsPath
  $body.AddLines(@(
    (New-Object System.Drawing.PointF(86,108)),
    (New-Object System.Drawing.PointF(170,108)),
    (New-Object System.Drawing.PointF(160,192)),
    (New-Object System.Drawing.PointF(96,192))
  ))
  $body.CloseFigure()
  $g.FillPath($white,$body)
  # Ridges (cut with background color)
  $cut = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255,80,110,240),8)
  $cut.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
  $cut.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
  $g.DrawLine($cut,116,124,113,178)
  $g.DrawLine($cut,128,124,128,180)
  $g.DrawLine($cut,140,124,143,178)

  $g.Dispose()
  return $bmp
}

$master = New-Master
$master.Save('C:\app_icon_preview.png',[System.Drawing.Imaging.ImageFormat]::Png)
$sizes = @(16,24,32,48,64,128,256)
$pngs = @()
foreach($sz in $sizes){
  $b = New-Object System.Drawing.Bitmap($sz,$sz)
  $gg = [System.Drawing.Graphics]::FromImage($b)
  $gg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $gg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $gg.Clear([System.Drawing.Color]::Transparent)
  $gg.DrawImage($master,0,0,$sz,$sz)
  $ms = New-Object System.IO.MemoryStream
  $b.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png)
  $pngs += ,$ms.ToArray()
  $gg.Dispose();$b.Dispose();$ms.Dispose()
}

# Assemble ICO
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)
$offset = 6 + (16*$sizes.Count)
for($i=0;$i -lt $sizes.Count;$i++){
  $sz=$sizes[$i]; $data=$pngs[$i]
  $bw.Write([byte]($(if($sz -ge 256){0}else{$sz})))
  $bw.Write([byte]($(if($sz -ge 256){0}else{$sz})))
  $bw.Write([byte]0)
  $bw.Write([byte]0)
  $bw.Write([uint16]1)
  $bw.Write([uint16]32)
  $bw.Write([uint32]$data.Length)
  $bw.Write([uint32]$offset)
  $offset += $data.Length
}
foreach($data in $pngs){ $bw.Write($data) }
$target = "C:\app_icon.ico"
[System.IO.File]::WriteAllBytes($target,$out.ToArray())
$bw.Dispose();$out.Dispose()
"ico written: " + (Get-Item $target).Length
