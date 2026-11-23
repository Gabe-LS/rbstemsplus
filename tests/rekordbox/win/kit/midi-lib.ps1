# midi-lib.ps1 (dot-source it): the virtual MIDI port pair Loopback A/B through WinMM.
# We send on Loopback A (rekordbox receives on Loopback B, mapped in
# rekordbox6\MidiMappings\Loopback B.midi.csv) and read rekordbox's lights on Loopback A's input
# (what rekordbox sends to Loopback B). The pair comes from Windows MIDI Services: it is not
# saved by the service (no config file), so Ensure-RbspLoopback recreates it after a restart.
# Notes (channel 1), as mapped: 64 Browse Down, 65 Load deck 1, 66 Browse Up, 67 Forward,
# 68 Back, 69 Area, 70 PlayPause deck 1, 71 Sort by track number, 72 Browse view (SwitchActiveWindow),
# 73 Sort by track title (SortSONG), 74 Area Left, 75 Area Right.
$RbspNote = @{ Down = 64; Load = 65; Up = 66; Forward = 67; Back = 68; Area = 69; Play = 70; SortTrack = 71; View = 72; SortSong = 73; AreaLeft = 74; AreaRight = 75 }
if (-not ('RbspMidi2' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Collections.Concurrent; using System.Runtime.InteropServices;
public static class RbspMidi2 {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct OUTCAPS { public ushort wMid, wPid; public uint ver; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string name; public ushort tech, voices, notes, chmask; public uint support; }
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct INCAPS { public ushort wMid, wPid; public uint ver; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string name; public uint support; }
  public delegate void InProc(IntPtr h, uint msg, IntPtr inst, IntPtr p1, IntPtr p2);
  [DllImport("winmm.dll")] public static extern uint midiOutGetNumDevs();
  [DllImport("winmm.dll")] public static extern uint midiInGetNumDevs();
  [DllImport("winmm.dll", CharSet = CharSet.Unicode)] public static extern uint midiOutGetDevCapsW(UIntPtr id, ref OUTCAPS c, uint size);
  [DllImport("winmm.dll", CharSet = CharSet.Unicode)] public static extern uint midiInGetDevCapsW(UIntPtr id, ref INCAPS c, uint size);
  [DllImport("winmm.dll")] public static extern uint midiOutOpen(out IntPtr h, uint id, IntPtr cb, IntPtr inst, uint flags);
  [DllImport("winmm.dll")] public static extern uint midiOutShortMsg(IntPtr h, uint msg);
  [DllImport("winmm.dll")] public static extern uint midiOutClose(IntPtr h);
  [DllImport("winmm.dll")] public static extern uint midiInOpen(out IntPtr h, uint id, InProc cb, IntPtr inst, uint flags);
  [DllImport("winmm.dll")] public static extern uint midiInStart(IntPtr h);
  [DllImport("winmm.dll")] public static extern uint midiInStop(IntPtr h);
  [DllImport("winmm.dll")] public static extern uint midiInClose(IntPtr h);
  // what rekordbox sends: (time, status, data1, data2); the last value per note is kept too
  public static ConcurrentQueue<Tuple<DateTime, int, int, int>> Got = new ConcurrentQueue<Tuple<DateTime, int, int, int>>();
  public static int[] Last = new int[128];
  public static DateTime[] LastAt = new DateTime[128];
  public static DateTime First = DateTime.MinValue;
  static InProc keep = OnIn;   // kept alive for the callback
  static void OnIn(IntPtr h, uint msg, IntPtr inst, IntPtr p1, IntPtr p2) {
    if (msg != 0x3C3) return;   // MIM_DATA
    uint d = (uint)p1.ToInt64();
    int st = (int)(d & 0xFF), a = (int)((d >> 8) & 0x7F), b = (int)((d >> 16) & 0x7F);
    var now = DateTime.Now;
    if (First == DateTime.MinValue) First = now;
    Got.Enqueue(Tuple.Create(now, st, a, b));
    if ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80) { Last[a] = (st & 0xF0) == 0x80 ? 0 : b; LastAt[a] = now; }
  }
  public static IntPtr OpenIn(uint id) {
    IntPtr h; uint r = midiInOpen(out h, id, keep, IntPtr.Zero, 0x30000);   // CALLBACK_FUNCTION
    if (r != 0) throw new Exception("midiInOpen failed: " + r);
    midiInStart(h); return h;
  }
}
"@
}
# every WinMM port's name: 'out N : NAME' and 'in N : NAME' lines (what rekordbox sees)
function Get-RbspPorts {
    $n = [RbspMidi2]::midiOutGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) { $c = New-Object RbspMidi2+OUTCAPS; [void][RbspMidi2]::midiOutGetDevCapsW([UIntPtr][uint32]$i, [ref]$c, [Runtime.InteropServices.Marshal]::SizeOf($c)); "out $i : $($c.name)" }
    $n = [RbspMidi2]::midiInGetNumDevs()
    for ($i = 0; $i -lt $n; $i++) { $c = New-Object RbspMidi2+INCAPS; [void][RbspMidi2]::midiInGetDevCapsW([UIntPtr][uint32]$i, [ref]$c, [Runtime.InteropServices.Marshal]::SizeOf($c)); "in $i : $($c.name)" }
}
function Find-RbspPort([string]$name, [switch]$In) {
    $n = if ($In) { [RbspMidi2]::midiInGetNumDevs() } else { [RbspMidi2]::midiOutGetNumDevs() }
    for ($i = 0; $i -lt $n; $i++) {
        if ($In) { $c = New-Object RbspMidi2+INCAPS; [void][RbspMidi2]::midiInGetDevCapsW([UIntPtr][uint32]$i, [ref]$c, [Runtime.InteropServices.Marshal]::SizeOf($c)) }
        else { $c = New-Object RbspMidi2+OUTCAPS; [void][RbspMidi2]::midiOutGetDevCapsW([UIntPtr][uint32]$i, [ref]$c, [Runtime.InteropServices.Marshal]::SizeOf($c)) }
        if ($c.name -eq $name) { return [uint32]$i }
    }
    return $null
}
function Test-RbspLoopback { ($null -ne (Find-RbspPort 'Loopback A')) -and ($null -ne (Find-RbspPort 'Loopback A' -In)) -and ($null -ne (Find-RbspPort 'Loopback B')) -and ($null -ne (Find-RbspPort 'Loopback B' -In)) }
# Windows MIDI Services' console (midi.exe), from PATH or its install folder ($null: not installed)
function Find-MidiConsole {
    $c = Get-Command midi.exe -ErrorAction SilentlyContinue; if ($c) { return $c.Source }
    $f = Get-ChildItem -LiteralPath (Join-Path $env:ProgramFiles 'Windows MIDI Services') -Recurse -Filter midi.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { return $f.FullName } else { return $null }
}
# the Loopback A/B pair: there already, or created with the console (lasts until the MIDI service
# restarts). Returns a one-line result; throws when the pair can't be had.
function Ensure-RbspLoopback {
    if (Test-RbspLoopback) { return 'Loopback A/B present' }
    $midi = Find-MidiConsole
    if (-not $midi) { throw 'no Loopback A/B and no Windows MIDI Services console (midi.exe): install Windows MIDI Services Tools (README)' }
    $o = & $midi loopback create --name-a 'Loopback A' --name-b 'Loopback B' --unique-identifier 'rbsp-loopback' 2>&1 | Out-String
    $t = Get-Date
    while (-not (Test-RbspLoopback) -and ((Get-Date) - $t).TotalSeconds -lt 20) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-RbspLoopback)) { throw ("Loopback A/B created but not seen by WinMM in 20 s; midi.exe said: " + ($o -replace '\s+', ' ').Trim()) }
    return 'Loopback A/B recreated (midi loopback create)'
}
function Open-RbspOut {
    $i = Find-RbspPort 'Loopback A'; if ($null -eq $i) { throw "no MIDI output 'Loopback A' (the loopback pair is gone: midi-auto.ps1 -Do ensure recreates it)" }
    $h = [IntPtr]::Zero; $r = [RbspMidi2]::midiOutOpen([ref]$h, $i, [IntPtr]::Zero, [IntPtr]::Zero, 0)
    if ($r -ne 0) { throw "midiOutOpen failed: $r" }; return $h
}
function Open-RbspIn {
    $i = Find-RbspPort 'Loopback A' -In; if ($null -eq $i) { throw "no MIDI input 'Loopback A'" }
    return [RbspMidi2]::OpenIn($i)
}
# one press of a raw note as Pioneer hardware does it: note on 127, then note on 0 as the release
# (a note off isn't a release for rekordbox: it holds and repeats)
function Press-Note([IntPtr]$h, [int]$note) {
    [void][RbspMidi2]::midiOutShortMsg($h, [uint32](0x90 -bor ($note -shl 8) -bor (127 -shl 16)))
    Start-Sleep -Milliseconds 60
    [void][RbspMidi2]::midiOutShortMsg($h, [uint32](0x90 -bor ($note -shl 8)))
}
function Press([IntPtr]$h, [string]$what, [int]$times = 1, [int]$gapMs = 250) {
    $note = $RbspNote[$what]; if ($null -eq $note) { throw "unknown control $what" }
    for ($k = 0; $k -lt $times; $k++) {
        Press-Note $h $note
        if ($k -lt $times - 1) { Start-Sleep -Milliseconds $gapMs }
    }
}
