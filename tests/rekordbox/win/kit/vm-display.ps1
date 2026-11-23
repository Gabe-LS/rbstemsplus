# vm-display.ps1: the Parallels VM target's display made the owner's PC's (the shots and OCR regions
# are 1920x1080 at 100 %). Runs as the test account in its desktop session (pc-session.sh session
# display). The resolution is set with ChangeDisplaySettingsEx and kept in the registry; the scale
# comes from the VM's Parallels setting (high resolution off) and needs a fresh sign-in to change.
# Output ends with "DISPLAY WxH dpi N". Exit 0: the size and 96 dpi; 3: the size, but another dpi
# (a restart signs the account in again); 1: the size could not be set.
param([int]$Width = 1920, [int]$Height = 1080)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class RbspVmDisplay {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
    public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string d, int m, ref DEVMODE dm);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsEx(string d, ref DEVMODE dm, IntPtr h, int f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern uint GetDpiForSystem();
  // DISP_CHANGE_* (0 done); CDS_UPDATEREGISTRY keeps it across sign-ins
  public static int Set(int w, int h) {
    var dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
    if (!EnumDisplaySettings(null, -1, ref dm)) return -100;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = 0x80000 | 0x100000;
    return ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, 0x01, IntPtr.Zero);
  }
}
'@
# per-monitor aware: the real pixels, not a scaled virtual screen
$null = [RbspVmDisplay]::SetProcessDpiAwarenessContext([IntPtr]-4)
function Size { '{0}x{1}' -f [RbspVmDisplay]::GetSystemMetrics(0), [RbspVmDisplay]::GetSystemMetrics(1) }
$want = '{0}x{1}' -f $Width, $Height
if ((Size) -ne $want) {
    $r = [RbspVmDisplay]::Set($Width, $Height)
    if ($r -ne 0) { "vm-display: ChangeDisplaySettingsEx said $r" }
    Start-Sleep -Seconds 2
}
$size = Size
$dpi = [RbspVmDisplay]::GetDpiForSystem()
"DISPLAY $size dpi $dpi"
if ($size -ne $want) { exit 1 }
if ($dpi -ne 96) { exit 3 }
exit 0
