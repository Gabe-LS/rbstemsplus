# ocr-lib.ps1 (dot-source it): reads text from a region of a screenshot with Windows' own OCR
# (Windows.Media.Ocr, part of Windows; nothing installed).
#   Read-ScreenText PNG X Y W H   -> the recognised text (light text on rekordbox's dark background:
#                                    the region is inverted and scaled x2 first, which OCR reads better)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Storage.FileAccessMode, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics, ContentType = WindowsRuntime]
$null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType = WindowsRuntime]
$null = [Windows.Media.Ocr.OcrResult, Windows.Foundation, ContentType = WindowsRuntime]
$script:RbspAsTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await-Rt($op, [Type]$t) { $task = $script:RbspAsTask.MakeGenericMethod($t).Invoke($null, @($op)); if (-not $task.Wait(15000)) { throw "a Windows Runtime call ($($t.Name)) gave no result in 15 s" }; $task.Result }
$script:RbspOcr = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
function Read-ScreenText([string]$png, [int]$x, [int]$y, [int]$w, [int]$h, [switch]$Plain, [int]$Scale = 2) {
    $src = [System.Drawing.Bitmap]::FromFile($png)
    try { Read-BitmapText $src $x $y $w $h -Plain:$Plain -Scale $Scale }
    finally { $src.Dispose() }
}

function Read-BitmapText([System.Drawing.Bitmap]$src, [int]$x, [int]$y, [int]$w, [int]$h, [switch]$Plain, [int]$Scale = 2) {
    $memory = [IO.MemoryStream]::new()
    try {
        $dst = New-Object System.Drawing.Bitmap ($w * $Scale), ($h * $Scale)
        try {
            $g = [System.Drawing.Graphics]::FromImage($dst)
            try {
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $inv = New-Object System.Drawing.Imaging.ColorMatrix
                $inv.Matrix00 = -1; $inv.Matrix11 = -1; $inv.Matrix22 = -1; $inv.Matrix40 = 1; $inv.Matrix41 = 1; $inv.Matrix42 = 1; $inv.Matrix33 = 1; $inv.Matrix44 = 1
                $ia = New-Object System.Drawing.Imaging.ImageAttributes
                try {
                    if (-not $Plain) { $ia.SetColorMatrix($inv) }
                    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, ($w * $Scale), ($h * $Scale)), $x, $y, $w, $h, [System.Drawing.GraphicsUnit]::Pixel, $ia)
                } finally { $ia.Dispose() }
            } finally { $g.Dispose() }
            $dst.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $dst.Dispose() }
        $memory.Position = 0
        $stream = [IO.WindowsRuntimeStreamExtensions]::AsRandomAccessStream($memory)
        try {
            $dec = Await-Rt ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
            $bmp = Await-Rt ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
            try { return (Await-Rt ($script:RbspOcr.RecognizeAsync($bmp)) ([Windows.Media.Ocr.OcrResult])).Text }
            finally { $bmp.Dispose() }
        } finally { $stream.Dispose() }
    } finally { $memory.Dispose() }
}
# The regions below are for the test PC's 3840x2096 screen at 200 % with rekordbox maximised (as
# screenshotted 2026-11-18); another layout needs new numbers (the README says how). A screenshot
# of the same layout at another size (1920x1080 at 100 %, 2026-11-19) gets them scaled to its width.
$RbspDeck1 = 110, 430, 760, 70     # deck 1's title (x, y, w, h), without the BPM and time readouts
$RbspHeader = 530, 930, 900, 55    # the track list's header ("RBSP Tests (36 Tracks)")
$RbspDeck1Time = 1300, 476, 260, 44   # deck 1's time readouts ("-01:23.4 00:36.6"), right of its title
if (-not (Get-Command Scale-Region -ErrorAction SilentlyContinue)) { . (Join-Path $PSScriptRoot 'match-lib.ps1') }
# a region scaled to the screenshot's width
function Region([string]$png, [int[]]$r) {
    $img = [System.Drawing.Image]::FromFile($png)
    try { return Scale-Region $img.Width $r } finally { $img.Dispose() }
}
# rekordbox's deck 1 title
# (only the title: the BPM and time readouts beside it are left out); inverted first, then as it is
function Read-Deck1Title([string]$png) {
    $r = Region $png $RbspDeck1
    $t = Read-ScreenText $png @r
    if (-not $t.Trim()) { $t = Read-ScreenText $png @r -Plain }
    return $t
}
# the track list's header ("RBSP Tests (36 Tracks)"), above the list
function Read-ListHeader([string]$png, [int]$Scale = 2) {
    $r = Region $png $RbspHeader
    $t = Read-ScreenText $png @r -Scale $Scale
    if (-not $t.Trim()) { $t = Read-ScreenText $png @r -Plain -Scale $Scale }
    return $t
}
# Read the full strip above the track columns for the longer format playlist headings.
function Read-FormatListHeaders([string]$png) {
    $r = Region $png @(490, 898, 1620, 84)
    foreach ($scale in 2, 3, 4) {
        if ($r[2] * $scale -gt [Windows.Media.Ocr.OcrEngine]::MaxImageDimension) { continue }
        Read-ScreenText $png @r -Scale $scale
        Read-ScreenText $png @r -Scale $scale -Plain
    }
}
# deck 1's time readouts (the DirectML bench's playback check), inverted first, then as they are
function Read-Deck1Time([string]$png) { $r = Region $png $RbspDeck1Time; $t = Read-ScreenText $png @r; if (-not ($t -match '\d')) { $t = Read-ScreenText $png @r -Plain }; return $t }
# the same title read at four scales (OCR slips differ by scale: w15 read as 'wi5' at x1 and x4, '0' at x2)
function Read-Deck1Titles([string]$png) { $r = Region $png $RbspDeck1; @(foreach ($sc in 1, 2, 3, 4) { Read-ScreenText $png @r -Scale $sc }) }
# (Norm, which compares a reading with a corpus name, is in match-lib.ps1)
