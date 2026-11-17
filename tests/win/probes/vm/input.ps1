# Synthesized mouse and keyboard input in the user's session (run through winps.sh --user), then
# a screenshot. The caller writes $acts on the first line: a list of steps such as
#   'click 2421 686', 'rclick 1279 1207', 'dclick x y', 'drag x1 y1 x2 y2', 'key ESC', 'sleep 3'
# Coordinates are physical pixels of the 3840x2160 screen.
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Threading;
public class Inp {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
  public static void Click(int x, int y, uint down, uint up) { SetCursorPos(x, y); Thread.Sleep(150); mouse_event(down, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(80); mouse_event(up, 0, 0, 0, UIntPtr.Zero); }
  public static void Drag(int x1, int y1, int x2, int y2) {
    SetCursorPos(x1, y1); Thread.Sleep(200); mouse_event(2, 0, 0, 0, UIntPtr.Zero); Thread.Sleep(200);
    for (int i = 1; i <= 20; i++) { SetCursorPos(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20); Thread.Sleep(40); }
    Thread.Sleep(300); mouse_event(4, 0, 0, 0, UIntPtr.Zero);
  }
  public static void Key(byte vk) { keybd_event(vk, 0, 0, UIntPtr.Zero); Thread.Sleep(60); keybd_event(vk, 0, 2, UIntPtr.Zero); }
}
'@
[Inp]::SetProcessDPIAware() | Out-Null
$vk = @{ ESC = 0x1B; ENTER = 0x0D; SPACE = 0x20; LEFT = 0x25; UP = 0x26; RIGHT = 0x27; DOWN = 0x28; TAB = 0x09 }
foreach ($a in $acts) {
  $w = $a -split ' '
  switch ($w[0]) {
    'click'  { [Inp]::Click([int]$w[1], [int]$w[2], 2, 4) }
    'rclick' { [Inp]::Click([int]$w[1], [int]$w[2], 8, 16) }
    'dclick' { [Inp]::Click([int]$w[1], [int]$w[2], 2, 4); Start-Sleep -Milliseconds 90; [Inp]::Click([int]$w[1], [int]$w[2], 2, 4) }
    'drag'   { [Inp]::Drag([int]$w[1], [int]$w[2], [int]$w[3], [int]$w[4]) }
    'key'    { [Inp]::Key([byte]$vk[$w[1]]) }
    'sleep'  { Start-Sleep ([double]$w[1]) }
  }
  Start-Sleep -Milliseconds 400
  "done: $a"
}
. \\Mac\win\p0\shot.ps1
