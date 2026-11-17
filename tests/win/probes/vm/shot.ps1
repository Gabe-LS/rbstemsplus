# Saves a screenshot of the user's screen to \\Mac\win\shot.png (GDI copy from the user's
# session: prlctl capture misses rekordbox's GPU drawing). Dot-source it from a --user job.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type 'using System.Runtime.InteropServices; public class DpiA { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[DpiA]::SetProcessDPIAware() | Out-Null
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save('\\Mac\win\shot.png', [System.Drawing.Imaging.ImageFormat]::Png)
"shot saved ($($b.Width)x$($b.Height))"
