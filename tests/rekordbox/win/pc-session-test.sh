#!/bin/bash
# Tests of the rekordbox toolkit's Windows driver that need no PC: shell syntax, a PowerShell parse
# of every kit script (skipped without pwsh), pc-session.sh's argument handling and the PowerShell
# it sends, with the machine transports and notifier stubbed, and the "nothing personal tracked" checks.
# Run by make check.
# A direct "sh" invocation on macOS puts Bash in POSIX mode, where the
# temporary environment settings before fixture functions leak into later cases.
if [ -z "${BASH_VERSION:-}" ] || shopt -oq posix; then exec bash +o posix "$0" "$@"; fi
set -uo pipefail
unset RBKIT_TARGET RBKIT_VM_SERVER
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
tmp_parent="$(cd "${TMPDIR:-/tmp}" && pwd -P)" || exit 1
tmp="$(mktemp -d "$tmp_parent/rbkit-test.XXXXXX")" || exit 1
trap 'rm -rf "${tmp:?}"' EXIT
# pwsh's startup cache (StartupProfileData-*) is this run's own: pwsh processes of suites running
# at once rewrite the shared ~/.cache/powershell copy and corrupt it (pwsh then aborts on start).
export XDG_CACHE_HOME="$tmp/xdg-cache"; mkdir -p "$XDG_CACHE_HOME" || exit 1
# A target-selection regression in an existing PC case must never reach a real VM.
export VMRUN_PRLCTL=/dev/null VMRUN_SHARE="$tmp/vm-disabled"
pass=0; fail=0
ok() { pass=$((pass + 1)); }
no() { fail=$((fail + 1)); echo "FAIL: $*"; }
[ "$tmp" = "$(cd "$tmp" && pwd -P)" ] && ok || no "test temp directory must have a physical path"

# The kit must never change a physical disk's online state, including through embedded
# PowerShell, DiskPart or a direct storage-provider call.
disk_mutations='set-disk|diskpart|msft_disk|ioctl_disk_set_disk_attributes|(offline|online)[[:space:]]+disk|[.](offline|online)[[:space:]]*[(]|[.]isoffline[[:space:]]*='
if hits="$(grep -inE "$disk_mutations" "$here/pc-session.sh" "$here/gen-corpus-win.sh" "$here/kitps-run.ps1" "$here/kit/"*.ps1)"; then
  no "kit disk-state mutation: $hits"
else
  [ "$?" = 1 ] && ok || no "could not scan the kit for disk-state mutations"
fi

readme="$here/../README.md"
rog_example='VEVTVFNFUklBTDAxfFJPRyBFU0QtUzFD' # TESTSERIAL01|ROG ESD-S1C
if grep -Fq 'Base64 of UTF-8 `SERIAL|FriendlyName`' "$readme" &&
   grep -Fxq "git config --local --add rbstemsplus.pcProtectedDisks $rog_example" "$readme" &&
   grep -Fq '`'"$rog_example"'`' "$readme" &&
   grep -Fq '`'"$(printf '%s' "$rog_example" | base64 -D)"'`' "$readme"; then ok
else no "README protection format or ROG example is stale"; fi
grep -Fq "printf '%s' 'SERIAL|FriendlyName' | base64" "$readme" && ok ||
  no "README lacks the command to compute a disk protection value"

# --- syntax
bash -n "$here/pc-session.sh" && ok || no "bash -n pc-session.sh"
bash -n "$0" && ok || no "bash -n pc-session-test.sh"
grep -F 'foreach ($rel in ' "$here/kit/midi-auto.ps1" | grep -Fq "'task-out.txt.tmp'" &&
  ok || no "the format scratch guard skips task-out.txt.tmp"
bash -n "$here/gen-corpus-win.sh" && ok || no "bash -n gen-corpus-win.sh"
if command -v pwsh > /dev/null; then
  cat > "$tmp/parse.ps1" << 'EOF'
    $bad = 0
    foreach ($f in Get-ChildItem -LiteralPath $args[0] -Filter *.ps1) {
      $t = $null; $e = $null
      [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$t, [ref]$e)
      foreach ($x in $e) { "$($f.Name):$($x.Extent.StartLineNumber): $($x.Message)"; $bad++ }
      "parsed $($f.Name)"
    }
    exit $bad
EOF
  out="$(pwsh -NoProfile -NonInteractive -File "$tmp/parse.ps1" "$here/kit" 2>&1)"
  if [ $? = 0 ]; then pass=$((pass + $(grep -c '^parsed ' <<< "$out"))); else no "PowerShell parse:"; grep -v '^parsed ' <<< "$out" | sed 's/^/    /'; fi
  cat > "$tmp/winlist-win32.cs" << 'EOF'
using System;
public static class RbspWin {
    public static bool CheckDismissal;
    public static bool CheckMessageBox;
    public static string PickerMode;
    public static bool Clicked;
    public static bool ExtraButton;
    public static int ButtonStyle;
    public static int ButtonReads;
    public static string ButtonCaption = "&OK";
    public static string StaticText = "Unable to read C:\\Private Folder\\My Song.flac. Check the file format.";
    public struct RECT { public int L, T, R, B; }
    public static IntPtr GetForegroundWindow() { return new IntPtr(16); }
    public static IntPtr[] HandlesForProcess(int pid) { return new IntPtr[] { new IntPtr(16), new IntPtr(17), new IntPtr(18) }; }
    public static int Pid(IntPtr h) { return h.ToInt64() == 18 || h.ToInt64() == 26 ? 124 : 123; }
    public static bool GetWindowRect(IntPtr h, out RECT r) { r = new RECT { L = -10, T = 20, R = 356, B = 152 }; return true; }
    public static string WindowClass(IntPtr h) {
        switch (h.ToInt64()) {
            case 17: return "#32770";
            case 20: case 22: case 23: case 24: return "Static";
            case 21: case 26: case 28: return "Button";
            case 25: return "Edit";
            case 27: return "DirectUIHWND";
            case 29: case 30: return "CtrlNotifySink";
            case 31: return "ScrollBar";
            case 32: return "SysLink";
            default: return "JUCE_abcd";
        }
    }
    public static IntPtr[] DialogChildren(IntPtr h, int pid) {
        if (h.ToInt64() != 17 || pid != 123) throw new Exception("unexpected dialog");
        int[] children = PickerMode == "picker" ? new[] { 25, 28 } : new[] { 20, 21, 22, 23, 24, 25, 26, 27 };
        if (CheckDismissal) children = ExtraButton ? new[] { 21, 27, 28 } : new[] { 21, 27, 29, 30, 31, 32 };
        return Array.ConvertAll(children, n => new IntPtr(n));
    }
    public static int GetDlgCtrlID(IntPtr h) {
        if (PickerMode == "picker") return h.ToInt64() == 25 ? 1148 : 2;
        return h.ToInt64() == 21 ? 1 : h.ToInt64() == 25 ? 100 : -1;
    }
    public static string DialogControlText(IntPtr dialog, IntPtr h, int pid, int id) {
        switch (h.ToInt64()) {
            case 20: return StaticText;
            case 21: return CheckDismissal && ButtonReads++ == 0 ? "OK" : ButtonCaption;
            case 28: return "Cancel";
            case 22: return "Track title: My Song\r\nCould not load the track \"Other Song\".";
            case 23: return null;
            default: throw new Exception("unexpected control text read");
        }
    }
    public static IntPtr GetWindow(IntPtr h, uint relation) {
        return !IsWindowVisible(h) || (!String.IsNullOrEmpty(PickerMode) && h.ToInt64() == 17) ? new IntPtr(16) : IntPtr.Zero;
    }
    public static bool IsWindowVisible(IntPtr h) {
        return h.ToInt64() == 16 || (CheckDismissal && (h.ToInt64() == 17 || h.ToInt64() == 21 || h.ToInt64() == 27 || h.ToInt64() == 29)) ||
            (CheckMessageBox && (h.ToInt64() == 17 || h.ToInt64() == 20 || h.ToInt64() == 27)) ||
            (PickerMode == "picker" && (h.ToInt64() == 17 || h.ToInt64() == 25 || h.ToInt64() == 28));
    }
    public static bool IsWindowEnabled(IntPtr h) { return IsWindowVisible(h) || (CheckDismissal && h.ToInt64() >= 30 && h.ToInt64() <= 32); }
    public static bool IsIconic(IntPtr h) { return false; }
    public static bool? IsCloaked(IntPtr h) { return IsWindowVisible(h) ? (bool?)false : null; }
    public static int GetWindowLongW(IntPtr h, int index) {
        return index == -16 ? unchecked((int)0x90000000) | (h.ToInt64() == 24 ? 3 : h.ToInt64() == 21 ? ButtonStyle : 0) : 0;
    }
    public static string WindowCaption(IntPtr h) {
        if (!String.IsNullOrEmpty(PickerMode) && h.ToInt64() == 17) return "Open";
        return IsWindowVisible(h) ? "rekordbox" : "private caption";
    }
    public static bool OrdinaryDesktop() { return true; }
    public static bool IsWindow(IntPtr h) { return h.ToInt64() == 17 && !Clicked; }
    public static bool PostMessageW(IntPtr h, uint message, IntPtr w, IntPtr l) {
        if (h.ToInt64() != 21 || message != 0xF5 || w != IntPtr.Zero || l != IntPtr.Zero)
            throw new Exception("unexpected input target or message");
        return Clicked = true;
    }
}
namespace Windows.Forms { public static class SystemInformation { public static object VirtualScreen; } }
EOF
  cat > "$tmp/directui.cs" << 'EOF'
using System;
namespace Windows.Automation {
    public enum TreeScope { Descendants = 4 }
    public sealed class ControlType {
        public static readonly ControlType Text = new ControlType("Text"), Button = new ControlType("Button"),
            Hyperlink = new ControlType("Hyperlink"), CheckBox = new ControlType("CheckBox"), Pane = new ControlType("Pane"),
            TitleBar = new ControlType("TitleBar"), Image = new ControlType("Image"), ScrollBar = new ControlType("ScrollBar");
        public string ProgrammaticName { get; private set; }
        private ControlType(string name) { ProgrammaticName = "ControlType." + name; }
    }
    public class AutomationProperty {
        internal AutomationProperty() { }
    }
    public class AutomationPattern {
        public AutomationPattern(string name) { ProgrammaticName = name; }
        public string ProgrammaticName { get; private set; }
    }
    public class ValuePattern {
        public static readonly AutomationPattern Pattern = new AutomationPattern("ValuePatternIdentifiers.Pattern");
        public ValuePattern(bool readOnly) { IsReadOnly = readOnly; }
        public ValuePattern Current { get { return this; } }
        public bool IsReadOnly { get; private set; }
    }
    public class Condition {
        public static readonly Condition TrueCondition = new Condition();
        internal virtual bool Matches(ControlType type) { return true; }
    }
    public sealed class PropertyCondition : Condition {
        private readonly object value;
        public PropertyCondition(AutomationProperty property, object value) {
            if (property != AutomationElement.ControlTypeProperty) throw new ArgumentException("unexpected property");
            this.value = value;
        }
        internal override bool Matches(ControlType type) { return object.Equals(value, type); }
    }
    public sealed class OrCondition : Condition {
        private readonly Condition[] conditions;
        public OrCondition(params Condition[] conditions) { this.conditions = conditions; }
        internal override bool Matches(ControlType type) { return Array.Exists(conditions, c => c.Matches(type)); }
    }
    public sealed class TreeWalker {
        public static readonly TreeWalker ControlViewWalker = new TreeWalker();
        public AutomationElement GetParent(AutomationElement element) { return element.Parent; }
    }
    public class AutomationElement {
        public static readonly AutomationProperty ControlTypeProperty = new AutomationProperty();
        public static AutomationElement[] Descendants = new AutomationElement[0];
        public struct AutomationElementInformation {
            public string Name { get; internal set; }
            public int ProcessId { get; internal set; }
            public int NativeWindowHandle { get; internal set; }
            public ControlType ControlType { get; internal set; }
            public bool IsEnabled { get { return true; } }
            public bool IsOffscreen { get; internal set; }
            public bool IsKeyboardFocusable { get; internal set; }
        }
        public AutomationElementInformation Current { get; private set; }
        internal AutomationElement Parent { get; private set; }
        private string pattern;
        private AutomationElement(AutomationElementInformation current) { Current = current; }
        public static AutomationElement FromHandle(IntPtr h) {
            return new AutomationElement(new AutomationElementInformation { ProcessId = 123, NativeWindowHandle = h.ToInt32() });
        }
        public static AutomationElement Fixture(ControlType type, string name) {
            return Fixture(type, name, null);
        }
        public static AutomationElement Fixture(ControlType type, string name, AutomationElement parent) {
            return Fixture(type, name, parent, false, false, "");
        }
        public static AutomationElement Fixture(ControlType type, string name, AutomationElement parent, bool offscreen, bool focusable, string pattern) {
            return new AutomationElement(new AutomationElementInformation {
                ProcessId = 123, ControlType = type, Name = name, IsOffscreen = offscreen, IsKeyboardFocusable = focusable
            }) {
                Parent = parent, pattern = pattern
            };
        }
        public AutomationPattern[] GetSupportedPatterns() {
            if (pattern == "invoke") return new[] { new AutomationPattern("InvokePatternIdentifiers.Pattern") };
            if (pattern == "read-only" || pattern == "writable") return new[] { ValuePattern.Pattern };
            return new AutomationPattern[0];
        }
        public ValuePattern GetCurrentPattern(AutomationPattern requested) {
            if (requested != ValuePattern.Pattern || (pattern != "read-only" && pattern != "writable"))
                throw new ArgumentException("unexpected pattern");
            return new ValuePattern(pattern == "read-only");
        }
        public AutomationElementCollection FindAll(TreeScope scope, Condition condition) {
            if (scope != TreeScope.Descendants) throw new ArgumentException("unexpected scope");
            return new AutomationElementCollection(Array.FindAll(Descendants, e => condition.Matches(e.Current.ControlType)));
        }
    }
    public class AutomationElementCollection : System.Collections.IEnumerable {
        private readonly AutomationElement[] elements;
        internal AutomationElementCollection(AutomationElement[] elements) { this.elements = elements; }
        public System.Collections.IEnumerator GetEnumerator() { return elements.GetEnumerator(); }
    }
}
EOF
  cat > "$tmp/winlist-test.ps1" << 'EOF'
param($KitSource, $Work)
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $Work 'winlist-win32.cs')
Add-Type -Path (Join-Path $Work 'directui.cs')
function Screen-Dpi { }
function Initialize-RbspWin { }
function Uia { }
function Add-Type([string[]]$AssemblyName) { }
function Get-RbProcess([int]$SessionId) { if ($SessionId -eq 5) { @{ Id = 123 } } }
$B = $Work
. (Join-Path $KitSource 'format-ui.ps1')
. (Join-Path $KitSource 'winlist.ps1')
function Save-RbWindowCrop($h, $processId, $allowedPids, $rect, $screen, $file) {
    if ($h.ToInt64() -ne 16 -or $processId -ne 123 -or $allowedPids.Count -ne 1 -or $rect.L -ne -10 -or
        (Split-Path -Parent $file) -cne (Join-Path $B 'shots')) { throw 'unexpected crop request' }
    'saved'
}
$list = Get-RbWindowList 5 | ConvertFrom-Json
$main, $hidden = $list.Windows
if ($list.Processes.Count -ne 1 -or $list.Processes[0] -ne 123 -or $list.Windows.Count -ne 2 -or
    $main.Handle -cne '0x10' -or $main.Owner -cne '0x0' -or $main.Class -cne 'JUCE_abcd' -or
    -not $main.Visible -or -not $main.Enabled -or -not $main.Foreground -or $main.Cloaked -ne $false -or
    ($main.Rect -join ',') -cne '-10,20,356,152' -or $main.Style -cne '0x90000000' -or $main.ExStyle -cne '0x00000000' -or
    $main.Caption -cne 'rekordbox' -or $main.Shot -cnotmatch '^winlist-[0-9a-f]{32}-0x10\.png$' -or
    $hidden.Handle -cne '0x11' -or $hidden.Owner -cne '0x10' -or $hidden.Visible -or $hidden.Enabled -or
    $null -ne $hidden.Cloaked -or $hidden.Class -cne '#32770' -or $hidden.Caption -cne '<redacted>' -or
    $hidden.Capture -cne 'unavailable' -or $hidden.Shot -or $null -ne $main.Controls) {
    throw 'unexpected window inventory'
}
$path, $ok, $titles, $unreadable, $icon, $edit, $direct = $hidden.Controls
if ($hidden.Controls.Count -ne 7 -or $path.Class -cne 'Static' -or $path.Id -ne -1 -or -not $path.TextRead -or
    $path.Text -cne 'Unable to read <redacted path>. Check the file format.' -or $ok.Class -cne 'Button' -or
    $ok.Id -ne 1 -or -not $ok.TextRead -or $ok.Text -cne '&OK' -or
    ($titles.Text -split '\r?\n' -join '|') -cne 'Track title: <redacted title>|Could not load the track <redacted title>.' -or
    $unreadable.TextRead -or $null -ne $unreadable.Text -or $icon.TextRead -or $null -ne $icon.Text -or
    $edit.Class -cne 'Edit' -or $edit.TextRead -or $null -ne $edit.Text) {
    throw 'unexpected dialog controls or redaction'
}
$closed = Get-RbWindowList 6 | ConvertFrom-Json
if ($closed.Processes.Count -or $closed.Windows.Count) { throw 'expected an empty inventory in the other session' }

$redactionFailures = @()
foreach ($probe in @(
    @{ Input = '"Private Song" could not be loaded.'; Expected = '<redacted title> could not be loaded.'; Hash = 'a7d94c86fc34' },
    @{ Input = 'Could not load Midnight Radio.'; Expected = 'Could not load Midnight Radio.'; Hash = 'fb3dada8674b' },
    @{ Input = 'Unable to open Music\Midnight Radio.flac.'; Expected = 'Unable to open <redacted path>.'; Hash = 'b20849ef5176' },
    @{ Input = 'File: C:\...\Song A.mp3'; Expected = 'File: <redacted path>'; Hash = 'ab00239b5943' },
    @{ Input = 'Error. Could not load Midnight Radio.flac.'; Expected = 'Error. <redacted path>.'; Hash = '45eda7c89f10' },
    @{ Input = ('Could not load {0}Private Song{1}.' -f [char]0x201C, [char]0x201D); Expected = 'Could not load <redacted title>.'; Hash = '76772cc8b75c' }
)) {
    $actual = Protect-RbDialogText $probe.Input
    if ($actual -cne $probe.Expected) { $redactionFailures += "'$($probe.Input)' became '$actual'" }
    [RbspWin]::CheckMessageBox = $true
    [RbspWin]::StaticText = $probe.Input
    $expected = "0x11: unrecognised text ($($probe.Input.Length) chars, sha256:$($probe.Hash))"
    if ((Get-RbMessageBoxes 5) -cne $expected) { $redactionFailures += "STOP did not hash '$($probe.Input)' verbatim" }
}
[RbspWin]::StaticText = 'Unexpected{0} application error' -f [char]0x200B
if ((Get-RbMessageBoxes 5) -cne '0x11: unrecognised text (29 chars, sha256:c50620f0bd36)') {
    $redactionFailures += 'STOP treated a zero-width character as an exact match'
}
[RbspWin]::CheckMessageBox = $false

if (@(Get-RbMessageBoxes 5).Count) { throw 'hidden dialog was treated as a run error' }
[RbspWin]::CheckMessageBox = $true
[RbspWin]::StaticText = 'Unexpected application error'
$A = [Windows.Automation.AutomationElement]
$CT = [Windows.Automation.ControlType]
$A::Descendants = @($A::Fixture($CT::Text, 'Could not load "Private Song".'))
$messages = @(Get-RbMessageBoxes 5)
if ($messages.Count -ne 1 -or $messages[0] -cne '0x11: Unexpected application error / unrecognised text (30 chars, sha256:36e2a480003b)' -or [RbspWin]::Clicked) {
    $redactionFailures += 'run message box did not allowlist the Win32 text and hash the DirectUI text'
}
if ($redactionFailures.Count) { throw ($redactionFailures -join "`n") }
[RbspWin]::PickerMode = 'look-alike'
if (@(Get-RbMessageBoxes 5 -AllowPicker).Count -ne 1) { throw 'a file picker caption hid a message box' }
[RbspWin]::CheckMessageBox = $false
[RbspWin]::PickerMode = 'picker'
if (@(Get-RbMessageBoxes 5 -AllowPicker).Count -or @(Get-RbMessageBoxes 5).Count -ne 1) {
    throw 'a verified picker should pass only the pre-quit recovery check'
}
[RbspWin]::PickerMode = $null

$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $KitSource 'session-task.ps1'), [ref]$null, [ref]$null)
$fn = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Dismiss-RbError' }, $true)
. ([scriptblock]::Create($fn.Extent.Text))
function Assert-RbDesktopProcess($p) { if ($p.Id -ne 123) { throw 'unexpected process' } }
$sid = 5
[RbspWin]::CheckDismissal = $true
$known = 'Unexpected application error'
$nativeRefusal = 'expected a DirectUI message with a single enabled OK button'
$uiRefusal = 'rekordbox dialog text or actions did not match the known error'
$dismissalFailures = @()
foreach ($scenario in @(
    @{ Name = 'other message'; Message = 'Licence activation is required'; Reason = $uiRefusal },
    @{ Name = 'near-identical message'; Message = ('Unexpected{0} application error' -f [char]0x200B); Reason = $uiRefusal },
    @{ Name = 'second native button'; Message = $known; ExtraButton = $true; Reason = $nativeRefusal },
    @{ Name = 'changed caption'; Message = $known; Caption = '&OK'; Reason = 'the rekordbox error button or desktop changed' },
    @{ Name = 'OK checkbox style'; Message = $known; Style = 3; Reason = $nativeRefusal },
    @{ Name = 'unnamed UIA checkbox'; Message = $known; CheckBox = $true; Reason = $uiRefusal },
    @{ Name = 'focusable error icon'; Message = $known; Image = $true; Focusable = $true; Reason = $uiRefusal },
    @{ Name = 'invokable error icon'; Message = $known; Image = $true; Pattern = 'invoke'; Reason = $uiRefusal },
    @{ Name = 'writable error icon'; Message = $known; Image = $true; Pattern = 'writable'; Reason = $uiRefusal },
    @{ Name = 'second decorative icon'; Message = $known; Image = $true; ExtraImage = $true; Reason = $uiRefusal }
)) {
    [RbspWin]::Clicked = $false
    [RbspWin]::ExtraButton = [bool]$scenario.ExtraButton
    [RbspWin]::ButtonStyle = [int]$scenario.Style
    [RbspWin]::ButtonReads = 0
    [RbspWin]::ButtonCaption = if ($scenario.Caption) { $scenario.Caption } else { 'OK' }
    $A::Descendants = @($A::Fixture($CT::Text, $scenario.Message))
    if ($scenario.CheckBox) { $A::Descendants += $A::Fixture($CT::CheckBox, '') }
    if ($scenario.Image) { $A::Descendants += $A::Fixture($CT::Image, 'Icon', $null, $false, [bool]$scenario.Focusable, [string]$scenario.Pattern) }
    if ($scenario.ExtraImage) { $A::Descendants += $A::Fixture($CT::Image, '') }
    $refused = $false
    try { [void](Dismiss-RbError) } catch { $refused = $_.Exception.Message -ceq $scenario.Reason }
    if (-not $refused -or [RbspWin]::Clicked) { $dismissalFailures += "dialog dismissal guard failed: $($scenario.Name)" }
}
[RbspWin]::Clicked = $false
[RbspWin]::ButtonStyle = 1
$titleBar = $A::Fixture($CT::TitleBar, 'rekordbox')
$A::Descendants = @($titleBar, $A::Fixture($CT::Button, 'Close', $titleBar),
    $A::Fixture($CT::Image, 'Private icon name', $null, $false, $false, 'read-only'),
    $A::Fixture($CT::Text, $known), $A::Fixture($CT::Pane, 'OK'),
    $A::Fixture($CT::ScrollBar, '', $null, $true, $false, ''),
    $A::Fixture($CT::Hyperlink, '', $null, $true, $true, 'invoke'))
$summary = Get-RbErrorUiaSummary (Get-RbDirectUiContent ([IntPtr]17) 123 -RawText -AllElements -ExcludeTitleBar)
if (($summary.Elements.Type -join ',') -cne 'ControlType.Image,ControlType.Text,ControlType.Pane' -or
    ($summary.Elements.Name -join ',') -cne "<redacted>,$known,OK" -or $summary.Elements[0].Focusable -or
    ($summary.Elements[0].Patterns -join ',') -cne 'ValuePatternIdentifiers.Pattern' -or -not $summary.Elements[0].ReadOnlyValue) {
    throw 'error diagnostic did not match the live element set with fixed-label redaction'
}
if ((Dismiss-RbError) -cnotlike 'BM_CLICK closed*' -or -not [RbspWin]::Clicked) { throw 'the exact error was not dismissed' }
if ($dismissalFailures.Count) { throw ($dismissalFailures -join "`n") }
EOF
  out="$(pwsh -NoProfile -NonInteractive -File "$tmp/winlist-test.ps1" "$here/kit" "$tmp" 2>&1)"
  if [ $? = 0 ]; then ok; else no "Win32 window inventory: $out"; fi
  cat > "$tmp/watcher-offer.ps1" << 'EOF'
param($Source, [string]$FilterCase)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class RbspWin {
    public static string Mode = "";
    public static int Clicks;
    public static int Polls;
    public static bool OrdinaryDesktop() { return true; }
    public static List<IntPtr> HandlesForProcess(int pid) {
        if (pid != 123) throw new Exception("unexpected watcher pid");
        var handles = new List<IntPtr>();
        if (Clicks == 0 || Mode == "stuck") handles.Add(new IntPtr(16));
        if (Clicks == 1 && (Mode == "endless" || Mode == "repeat" || Mode == "non-offer" ||
            (Mode == "successive" && ++Polls >= 3))) handles.Add(new IntPtr(32));
        if (Clicks == 2 && Mode == "endless") handles.Add(new IntPtr(48));
        return handles;
    }
    public static int Pid(IntPtr h) {
        int handle = h.ToInt32();
        if (handle == 16 && Clicks == 1 && Mode == "successive") return 456;
        if (handle == 16 || handle == 32 || handle == 48) return IsWindow(h) ? 123 : 0;
        if (handle == 25) return 123; // Same PID, outside the dialog.
        int child = handle % 16;
        return child >= 4 && child <= 8 && handle >= 20 && handle <= 56 ? Pid(new IntPtr(handle - child)) : 0;
    }
    public static bool IsWindow(IntPtr h) {
        switch (h.ToInt32()) {
            case 16: return Clicks == 0 || Mode == "stuck" || (Mode == "successive" && Clicks == 1);
            case 32: return Clicks == 1 && (Mode == "endless" || Mode == "repeat" || Mode == "non-offer" ||
                (Mode == "successive" && Polls >= 3));
            case 48: return Clicks == 2 && Mode == "endless";
            default: return Pid(h) != 0;
        }
    }
    public static bool IsWindowVisible(IntPtr h) { return IsWindow(h) && !(Mode == "stuck" && Clicks != 0); }
    public static bool IsWindowEnabled(IntPtr h) { return true; }
    public static string WindowCaption(IntPtr h) { return "RB Stems Plus"; }
    public static string WindowClass(IntPtr h) {
        int child = h.ToInt32() % 16;
        if (child == 0) return "#32770";
        if (Mode == "uia-only" && child == 7) return "DirectUIHWND";
        if (Mode == "uia-only" && child == 8) return "CtrlNotifySink";
        if (child == 8 && Mode == "link-body") return "SysLink";
        return child >= 7 ? "Static" : "Button";
    }
    public static IntPtr GetWindow(IntPtr h, uint relation) { return IntPtr.Zero; }
    public static bool? IsCloaked(IntPtr h) { return false; }
    public static List<IntPtr> DialogChildren(IntPtr h, int pid) {
        var children = new List<IntPtr>();
        if (Pid(h) == pid) {
            int[] order = Mode == "uia-only" ? new[] { 7, 8, 4, 5, 6 } :
                Mode == "zero-reordered" ? new[] { 4, 6, 5, 7, 8 } :
                Mode.StartsWith("zero-") ? new[] { 4, 5, 6, 7, 8 } : new[] { 6, 4, 5, 7, 8 };
            foreach (int offset in order) children.Add(new IntPtr(h.ToInt32() + offset));
        }
        return children;
    }
    public static int GetDlgCtrlID(IntPtr h) {
        int child = h.ToInt32() % 16;
        if (child >= 7) return -1;
        if (Mode == "mixed-ids" && child == 5) return 101;
        return Mode.StartsWith("zero-") || Mode == "mixed-ids" || Mode == "uia-only" ? 0 : 96 + child;
    }
    public static string DialogControlText(IntPtr h, IntPtr child, int pid, int id) {
        int offset = child.ToInt32() - h.ToInt32();
        if (Pid(h) != pid || Pid(child) != pid || offset < 4 || offset > 8 || GetDlgCtrlID(child) != id)
            return null;
        if (offset == 4) return Mode == "zero-offer" ? "Reinstall Now" : "Install Now";
        if (offset == 5) return Mode == "other-button" || Mode == "zero-other-button" ? "Install Now" : "Remind Me Later";
        if (offset == 6) return Mode == "grouped" ? "Don't Ask Again for These Versions" : "Don't Ask Again for This Version";
        if (Mode == "uia-only") throw new Exception("unexpected native text read from a DirectUI container");
        if (offset == 7) return "rekordbox was updated";
        if (Mode == "uia-body" || Mode == "uia-non-offer") return null;
        if (Mode == "non-offer" && Clicks == 1) return "Something else from the watcher";
        string versions = Mode == "grouped" ? "7.2.19, 7.2.20" :
            Mode == "uninstalled" ? "7.2.21" : Clicks == 0 || Mode == "repeat" ? "7.2.19" : "7.2.20";
        return "Stems Cache isn't on in rekordbox " + versions + " yet.";
    }
    public static bool PostMessageW(IntPtr h, uint message, IntPtr w, IntPtr l) {
        if (h.ToInt64() != 16L * (Clicks + 1) || !IsWindow(h) || message != 0x466 || w.ToInt64() != 101 || l != IntPtr.Zero)
            throw new Exception("input other than the verified Remind Me Later");
        Clicks++;
        return true;
    }
}
'@
Add-Type -Path (Join-Path $PSScriptRoot 'directui.cs')
$A = [Windows.Automation.AutomationElement]
$CT = [Windows.Automation.ControlType]
$Kit = Split-Path -Parent $Source
$env:LOCALAPPDATA = 'C:\Fixture'
$sid = 7
$RbVersion = '7.2.19'
$identityFailures = @()
foreach ($handle in 16, 20, 21, 22) {
    if ([RbspWin]::Pid([IntPtr]$handle) -ne 123) { $identityFailures += "watcher handle $handle has the wrong PID" }
}
foreach ($case in @(
    @{ Name = 'foreign dialog'; Dialog = 17; Child = 20; Pid = 123; Id = 100 },
    @{ Name = 'foreign PID'; Dialog = 16; Child = 20; Pid = 124; Id = 100 },
    @{ Name = 'non-child'; Dialog = 16; Child = 25; Pid = 123; Id = -1 },
    @{ Name = 'wrong control ID'; Dialog = 16; Child = 20; Pid = 123; Id = 101 }
)) {
    $control = [RbspWin]::DialogControlText([IntPtr]$case.Dialog, [IntPtr]$case.Child, $case.Pid, $case.Id)
    if ($null -ne $control) { $identityFailures += "watcher fake missed the identity mismatch for $($case.Name)" }
}
if ($identityFailures.Count) { throw ($identityFailures -join "`n") }
function Initialize-RbspWin { }
function Uia { }
function Get-Rb { @{ Exe = 'fixture'; Dir = 'fixture'; Version = '7.2.19' } }
function Get-RbInstalls([string]$Version) {
    $installed = @('7.2.19', '7.2.20')
    if ($mode -eq 'repeat') { $installed += '7.2.21' }
    foreach ($v in $installed) {
        if (-not $Version -or $v -eq $Version) { @{ Version = $v; Dir = 'fixture'; Exe = 'fixture' } }
    }
}
function Say([string]$s) { $script:log += $s }
function Assert-RbProcessVersion { }
function Test-RbRunning { return $false }
function Close-TestInputDirector { }
function Get-Process {
    if ($mode -eq 'none') { return }
    $path = if ($mode -eq 'foreign') { 'C:\Other' } else { $env:LOCALAPPDATA + '\Programs\RB Stems Plus\1.5.0' }
    @{ Id = 123; Path = "$path\rbsp-watch-x64.exe"; ProcessName = 'rbsp-watch-x64'; SessionId = $sid }
}
function Start-Process {
    if (@(Get-WatcherOfferWindows).Count) { throw 'started rekordbox with a watcher window open' }
    $script:launches++
}
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Source, [ref]$null, [ref]$null)
foreach ($definition in $ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -cmatch '^(Get|Assert|Defer)-WatcherOffer'
}, $true)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
$switch = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.SwitchStatementAst] -and
    $node.Condition.Extent.Text -ceq '$cmd' }, $true)
$arm = $switch.Clauses | Where-Object { $_.Item1.Value -ceq 'start' }
$body = $arm.Item2.Extent.Text
$start = [scriptblock]::Create($body.Substring(1, $body.Length - 2))
$failures = @()
foreach ($mode in 'none', 'offer', 'zero-offer', 'foreign', 'other-button', 'zero-other-button', 'zero-reordered', 'mixed-ids',
    'successive', 'endless', 'non-offer', 'repeat', 'uninstalled', 'grouped', 'stuck', 'link-body', 'uia-body', 'uia-non-offer', 'uia-only') {
    if ($FilterCase -and $mode -ne $FilterCase) { continue }
    [RbspWin]::Mode = $mode
    [RbspWin]::Clicks = 0
    [RbspWin]::Polls = 0
    $A::Descendants = @()
    if ($mode -clike 'uia-*') {
        $message = if ($mode -eq 'uia-non-offer') { 'Something else from the watcher' } else { "Stems Cache isn't on in rekordbox 7.2.19 yet." }
        $A::Descendants = @($A::Fixture($CT::Text, 'rekordbox was updated'), $A::Fixture($CT::Text, $message))
    }
    $script:launches = 0
    $script:log = @()
    $reason = ''
    try { . $start } catch { $reason = $_.Exception.Message }
    $expected = switch ($mode) {
        'none'       { @{ Clicks = 0; Launches = 1; Versions = '' } }
        'successive' { @{ Clicks = 2; Launches = 1; Versions = '7.2.19,7.2.20' } }
        'endless'    { @{ Clicks = 2; Reason = 'deferral limit'; Versions = '7.2.19,7.2.20' } }
        'non-offer'  { @{ Clicks = 1; Reason = 'offer text'; Versions = '7.2.19' } }
        'repeat'     { @{ Clicks = 1; Reason = 'already deferred'; Versions = '7.2.19' } }
        'uninstalled' { @{ Clicks = 0; Reason = 'not installed'; Versions = '' } }
        'stuck'      { @{ Clicks = 1; Reason = 'verified offer is still open'; Versions = '' } }
        'grouped'    { @{ Clicks = 1; Launches = 1; Versions = '7.2.19,7.2.20' } }
        'zero-reordered' { @{ Clicks = 0; Reason = 'button order'; Versions = '' } }
        { $_ -in 'offer', 'zero-offer', 'link-body', 'uia-body', 'uia-only' } { @{ Clicks = 1; Launches = 1; Versions = '7.2.19' } }
        'uia-non-offer' { @{ Clicks = 0; Reason = 'offer text'; Versions = '' } }
        default      { @{ Clicks = 0; Reason = 'RB Stems Plus watcher'; Versions = '' } }
    }
    $versionsLogged = @(foreach ($line in $log) {
        if ($line -cmatch '^RB Stems Plus watcher: deferred rekordbox ([0-9.]+)$') { $Matches[1] }
    }) -join ','
    if ($launches -ne [int]$expected.Launches -or [RbspWin]::Clicks -ne $expected.Clicks -or
        ($expected.Reason -and $reason -cnotlike "*$($expected.Reason)*") -or (-not $expected.Reason -and $reason) -or
        ($expected.Launches -and $expected.Clicks -and $r -cnotlike '*; RB Stems Plus watcher: pressed Remind Me Later') -or
        $versionsLogged -cne $expected.Versions) {
        $failures += "watcher ${mode}: clicks $([RbspWin]::Clicks), launches $launches, deferred [$versionsLogged], error [$reason]"
    }
    if ($mode -eq 'zero-other-button' -and ($reason -cnotlike '*reminder text/control*' -or
        $reason -cnotlike '*"caption":"RB Stems Plus"*' -or $reason -cnotlike '*"Id":0,"Text":"Install Now"*')) {
        $failures += "watcher offer diagnostic omitted the failed check or the observed label: $reason"
    }
    if ($mode -eq 'mixed-ids' -and $reason -cnotlike '*button IDs*') {
        $failures += "watcher accepted or misdiagnosed mixed IDs: $reason"
    }
}
if ($failures.Count) { throw ($failures -join "`n") }
EOF
  out="$(pwsh -NoProfile -NonInteractive -File "$tmp/watcher-offer.ps1" "$here/kit/session-task.ps1" 2>&1)"
  if [ $? = 0 ]; then ok; else no "watcher offer before start: $out"; fi
  cat > "$tmp/admin-protocol.ps1" << 'EOF'
param($ScriptPath, $Work)
$ErrorActionPreference = 'Stop'
# Load the worker script's functions without running its Windows admin preflight.
$ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$null, [ref]$null)
foreach ($node in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false)) {
    . ([scriptblock]::Create($node.Extent.Text))
}
function Get-Item([string]$LiteralPath) {
    $v = (Split-Path -Leaf (Split-Path -Parent $LiteralPath)).Substring('rekordbox '.Length)
    @{ VersionInfo = @{ ProductVersion = "$v.0342" } }
}
$root = Join-Path $Work 'RB Stems Plus'
[void](New-Item -ItemType Directory -Path (Join-Path $root 'receipts') -Force)
$bridge = 'f' * 64
$bindings = @()
$folders = @(foreach ($v in '7.2.22', '7.2.19', '7.2.21', '7.2.20', '7.2.9') {
    $dir = New-Item -ItemType Directory -Path (Join-Path $Work "rekordbox $v")
    Set-Content -LiteralPath (Join-Path $dir.FullName 'rekordbox.exe') -Value $v
    $live = Join-Path $dir.FullName 'onnxruntime.dll'
    Set-Content -LiteralPath $live -Value "stock $v"
    if ($v -ne '7.2.22') { $bindings += @{ rekordbox = $v; sha256 = Sha $live } }
    if ($v -in '7.2.20', '7.2.21') {
        $saved = New-Item -ItemType Directory -Path (Join-Path $root "pioneer/$v") -Force
        Move-Item -LiteralPath $live -Destination $saved.FullName
        Set-Content -LiteralPath $live -Value "bridge $v"
        if ($v -eq '7.2.20') {
            $exeSha = Sha (Join-Path $dir.FullName 'rekordbox.exe')
            Set-Content -LiteralPath (Join-Path $root "receipts/$v") -Value @(
                "exe_sha256 $exeSha", "original_sha256 $($bindings[-1].sha256)", "bridge_sha256 $(Sha $live)")
        } else {
            $bridge = Sha $live
        }
    }
    $dir
})
$targets = Get-InstallTargets $folders @{ ort = $bindings } $bridge $root
$expected = @(foreach ($v in '7.2.9', '7.2.19', '7.2.20') {
    $sha = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Work "rekordbox $v/rekordbox.exe")).Hash
    "${v}:$($sha.Substring(0, 16).ToLowerInvariant())"
}) -join ','
if ($targets -cne $expected) { throw "Unexpected target token $targets" }
$manifest = 'a' * 64
$name = 'rbstemsplus-admin-' + ('b' * 32)
$source = 'C:\fixture with spaces\'
$install = Get-WorkerArgs install $targets $manifest $source $name
$uninstall = Get-WorkerArgs uninstall '' '' '' $name
if ($install -cne ('"install" "--proto" "2" "--targets" "{0}" "--manifest" "{1}" "--source" "C:\fixture with spaces\\" "--force" "0" "--pipe" "\\.\pipe\{2}"' -f $expected, $manifest, $name) -or
    $uninstall -cne ('"uninstall" "--proto" "2" "--targets" "*" "--manifest" "-" "--source" "" "--force" "0" "--pipe" "\\.\pipe\{0}"' -f $name)) {
    throw "Wrong protocol arguments: $install / $uninstall"
}
if ((Format-WorkerLine '@target 7.2.19 6 put-back') -cne 'worker: rekordbox 7.2.19: put-back (exit 6)' -or
    (Format-WorkerLine '@target 7.2.20 - not-started') -cne 'worker: rekordbox 7.2.20: not-started' -or
    (Format-WorkerLine '@target malformed') -cne 'worker: @target malformed') { throw 'Wrong target diagnostics' }
if ((Format-WorkerExit 15)[0] -notlike '*scope or staged payload changed*' -or (Format-WorkerExit 15)[-1] -cne 'WORKER exit 15' -or
    (Format-WorkerExit 16)[0] -notlike '*partial result*' -or (Format-WorkerExit 16)[-1] -cne 'WORKER exit 16') {
    throw 'Wrong worker exit diagnostics'
}
'PASS PC worker protocol 2'
EOF
  mkdir -p "$tmp/admin-protocol"
  pwsh -NoProfile -NonInteractive -File "$tmp/admin-protocol.ps1" "$here/kit/admin-worker.ps1" "$tmp/admin-protocol" > "$tmp/admin-protocol.out" 2>&1
  if [ $? = 0 ] && grep -qx 'PASS PC worker protocol 2' "$tmp/admin-protocol.out"; then ok
  else no "PC worker protocol 2: $(cat "$tmp/admin-protocol.out")"; fi
  # The files below are text fixtures. Model Windows-only identity, signature and ACL calls.
  versions="$tmp/versions"
  for version in 7.2.19 7.2.20; do
    install="$versions/rekordbox/rekordbox $version"
    mkdir -p "$install"
    touch "$install/rekordbox.exe"
    printf '%s' "$version" > "$install/onnxruntime.dll"
  done
  cat > "$tmp/versions.ps1" << 'EOF'
param($kit, $fixture, $action, $RbVersion, $EvidenceRun)
$ErrorActionPreference = 'Stop'
$env:ProgramFiles = $env:TEMP = $env:APPDATA = $env:LOCALAPPDATA = $fixture
function Get-FileHash($LiteralPath, $Algorithm) {
    $hash = switch ([IO.File]::ReadAllText($LiteralPath)) {
        '7.2.19' { 'e7a702e13bd3e487705204d518c96f1f116aa25b0ac584741838e20bc1ff6e9f' }
        '7.2.20' { '3e55ab558136c0cf413c1e3873faff7c28807bf4b178712a2124e5efd4e03acd' }
        '7.2.21' { 'b' * 64 }
        'probe'  { 'a' * 64 }
    }
    @{ Hash = $hash }
}
function Get-Process {
    if ($action -eq 'quit') {
        [pscustomobject]@{
            SessionId = [Diagnostics.Process]::GetCurrentProcess().SessionId
            Path = Join-Path $fixture 'rekordbox/rekordbox 7.2.20/rekordbox.exe'
        }
    }
}
if ($action -eq 'kit') {
    . (Join-Path $kit 'kit-lib.ps1')
    $rb = Get-Rb
    "kit: $($rb.Version) $(Get-OrtState)"
    exit
}
if ($action -in 'swap-cache', 'swap-final') {
    . (Join-Path $kit 'kit-lib.ps1')
    $queue = [string[]]@('', '')
    if ($EvidenceRun) {
        $root = Join-Path $fixture 'RB Stems Plus'
        $bin = Join-Path $root 'bin'
        $id = Join-Path $bin 'f8b640536cb1'
        $worker = Join-Path $id 'RB Stems Plus.exe.0123456789abcdef.del'
        $paths = @($worker, $id, $bin, $root)
        switch ($EvidenceRun) {
            'unscheduled-file' { $paths = @($id, $bin, $root) }
            'unscheduled-root' { $paths = @($worker, $id, $bin) }
            'scheduled-extra' { $paths = @((Join-Path $root 'extra.txt')) + $paths }
            'scheduled-link' { $paths = @((Join-Path $root 'link')) + $paths }
            'late-delete' { $queue[1] = "\??\$worker`0`0`0" }
            'rename' { $queue[1] = '\??\C:\source' + "`0" + '!\??\Volume{test}\RB1F2A~1\incoming' + "`0`0" }
        }
        $queue[0] = (($paths | ForEach-Object { "*1\??\$($_.ToUpperInvariant())`0`0" }) -join '') + "`0"
    }
    function Get-AppSwapPendingQueue { return ,$queue }
    if ($action -eq 'swap-final') {
        # Run the Windows runner's actual appswap arm with the install session stubbed.
        $ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $kit 'midi-auto.ps1'), [ref]$null, [ref]$null)
        $dispatch = $ast.Find({
            $args[0] -is [System.Management.Automation.Language.SwitchStatementAst] -and $args[0].Condition.Extent.Text -eq '$Do'
        }, $true)
        $arm = $dispatch.Clauses | Where-Object { $_.Item1.Value -eq 'appswap' }
        function Say([string]$s) { $s }
        function Session($cmd) { if ($cmd -eq 'appinstall') { '10:00:00 appinstall: ok; fixture' } }
        $body = $arm.Item2.Extent.Text
        & ([scriptblock]::Create($body.Substring(1, $body.Length - 2)))
        exit
    }
    Get-AppSwapCacheState
    exit
}
if ($action -in 'start', 'quit') {
    . (Join-Path $kit 'kit-lib.ps1')
    function Get-TaskState { }
    function Start-Process($FilePath, $WorkingDirectory) {
        [IO.File]::WriteAllLines((Join-Path $fixture 'launch'), @($FilePath, $WorkingDirectory))
    }
    function Start-SessionTask { & (Join-Path $kit 'session-task.ps1') }
    Session $action
    exit
}
$Mode = if ($action -eq 'install-failure') { 'install' } else { $action }
$Sid = 'S-1-5-21-11-22-33-1001'
$ProbeSha = 'a' * 64
$NoDumps = $true
function Trace-KitPhase($phase) {
    if ($action -eq 'install-failure' -and $phase -eq 'probe: checking installed DLL') { throw 'simulated failure after replacement' }
}
function Get-CimInstance { @{ SID = $Sid; LocalPath = $fixture } }
function Get-AuthenticodeSignature { @{ Status = 'Valid' } }
function icacls.exe { $global:LASTEXITCODE = 0 }
$source = Get-Content -LiteralPath (Join-Path $kit 'probe.ps1') -Raw
$source = $source -replace '(?m)^.*\[Security.Principal.WindowsIdentity\].*$', ''
$source = $source -replace '(?m)^\$user = .*$', '$$user = ''fixture'''
$source = $source.Replace("'C:\rbtest'", "(Join-Path " + '$fixture' + " 'rbtest')")
$source = $source.Replace('($env:SystemDrive + [char]92)', '[string][IO.Path]::DirectorySeparatorChar').Replace('.Split([char]92)', '.Split([IO.Path]::DirectorySeparatorChar)')
# The probe's lock is machine-wide (a Global\ mutex, shared by every pwsh on this Mac too): this
# run's own name, so suites running at once don't refuse each other's probe commands.
$lock = "'Global\RBSP Kit Probe'"
if (-not $source.Contains($lock)) { throw 'probe.ps1: the probe lock name changed' }
$runLock = -join ([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($fixture)) | ForEach-Object { $_.ToString('x2') })
$source = $source.Replace($lock, "'Global\RBSP Kit Probe $($runLock.Substring(0, 16))'")
& ([scriptblock]::Create($source))
exit $LASTEXITCODE
EOF
  version_case() {
    local want="$1" expected="$2" rc; shift 2
    pwsh -NoProfile -NonInteractive -File "$tmp/versions.ps1" "$here/kit" "$versions" "$@" > "$tmp/versions-out" 2>&1; rc=$?
    if [ "$rc" = "$want" ] && grep -Fq -- "$expected" "$tmp/versions-out"; then ok
    else no "versions $* (exit $rc, expected $want): $expected"; sed 's/^/    /' "$tmp/versions-out"; fi
  }
  version_case 0 'kit: 7.2.20 pioneer' kit
  version_case 0 'kit: 7.2.19 pioneer' kit 7.2.19
  version_case 1 'rekordbox 7.2.18 is not installed' kit 7.2.18
  version_case 0 'started rekordbox 7.2.19' start 7.2.19
  version_case 0 'expected rekordbox 7.2.19' quit 7.2.19
  older="$versions/rekordbox/rekordbox 7.2.19"
  newer="$versions/rekordbox/rekordbox 7.2.20"
  mkdir "$versions/RB Stems Plus"
  version_case 0 'leftover' swap-cache 7.2.19
  printf probe > "$newer/onnxruntime.dll"
  version_case 0 'blocked' swap-cache 7.2.19
  printf '7.2.20' > "$newer/onnxruntime.dll"
  root="$versions/RB Stems Plus"
  worker="$root/bin/f8b640536cb1/RB Stems Plus.exe.0123456789abcdef.del"
  mkdir -p "$(dirname "$worker")"; touch "$worker"
  version_case 0 'clear' swap-cache 7.2.19 worker
  version_case 0 'appinstall: ok;' swap-final 7.2.19 worker
  touch "$root/extra.txt"
  version_case 0 'leftover' swap-cache 7.2.19 mixed
  version_case 0 'clear' swap-cache 7.2.19 scheduled-extra
  rm "$root/extra.txt"
  version_case 0 'leftover' swap-cache 7.2.19 unscheduled-file
  version_case 0 'leftover' swap-cache 7.2.19 unscheduled-root
  version_case 0 'leftover' swap-cache 7.2.19 late-delete
  version_case 0 'leftover' swap-cache 7.2.19 rename
  ln -s "$older/onnxruntime.dll" "$root/link"
  version_case 0 'leftover' swap-cache 7.2.19 scheduled-link
  rm -r "$root"
  printf '%s\n' "$older/rekordbox.exe" "$older" > "$tmp/want-launch"
  cmp -s "$tmp/want-launch" "$versions/launch" && ok || no "desktop task did not launch the selected executable and working directory"
  mkdir "$versions/rekordbox/rekordbox 7.2.21"; touch "$versions/rekordbox/rekordbox 7.2.21/rekordbox.exe"
  printf '7.2.21' > "$versions/rekordbox/rekordbox 7.2.21/onnxruntime.dll"
  version_case 2 'REFUSED: no Pioneer hash known for rekordbox 7.2.21' state 7.2.21
  version_case 2 'REFUSED: rekordbox 7.2.18 is not installed' state 7.2.18
  stage="$versions/AppData/Local/Temp/rbsp-kit/probe"
  mkdir -p "$stage"; printf probe > "$stage/onnxruntime.dll"
  version_case 0 'PROBE probe (rekordbox 7.2.20;' install 7.2.20
  version_case 0 'PROBE pioneer (rekordbox 7.2.20;' remove 7.2.20
  version_case 1 'simulated failure after replacement' install-failure 7.2.19
  [ "$(cat "$older/onnxruntime.dll")" = 7.2.19 ] && ok || no "a failed probe install left the replacement in place"
  version_case 0 'PROBE pioneer (rekordbox 7.2.19;' remove
  probe_run_fixture=0123456789abcdef0123456789abcdef
  version_case 0 'PROBE probe (rekordbox 7.2.19;' install 7.2.19 "$probe_run_fixture"
  version_case 2 'REFUSED: the probe backup belongs to rekordbox 7.2.19' remove 7.2.20 "$probe_run_fixture"
  version_case 2 'REFUSED: this run does not own this runtime change' remove 7.2.19 ffffffffffffffffffffffffffffffff
  rm -r "$versions/rekordbox/rekordbox 7.2.21"
  version_case 0 'PROBE pioneer (rekordbox 7.2.19;' remove '' "$probe_run_fixture"
  [ "$(cat "$older/onnxruntime.dll")" = 7.2.19 ] && [ "$(cat "$newer/onnxruntime.dll")" = 7.2.20 ] &&
    [ ! -e "$versions/rbtest" ] && ok || no "probe restore did not put the original bytes back in the replaced folder"
  version_case 0 'PROBE probe (rekordbox 7.2.19;' install 7.2.19
  rm "$versions/rbtest/real/rekordbox-version.txt"
  version_case 0 'PROBE pioneer (rekordbox 7.2.19;' remove
  mkdir "$versions/rbtest"
  ln -s "$older" "$versions/rbtest/real"
  version_case 2 'REFUSED: reparse point in probe path' remove 7.2.19
  cat > "$tmp/mailbox.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$kit = $args[0]
. (Join-Path $kit 'kit-lib.ps1')
$cmdFile = Join-Path $B 'task-cmd.txt'; $out = Join-Path $B 'task-out.txt'; $global:MailboxNow = [datetime]'2026-11-18T10:00:00'
function Get-TaskState { $script:state }
function Get-Date { $global:MailboxNow }
function Start-SessionTask {
    $script:starts++
    if ($script:mode -eq 'ping') { & (Join-Path $kit 'session-task.ps1') }
    else { [IO.File]::WriteAllLines($out, @('outdated-id', '10:00:00 ping: wrong session 99')) }
}
function Start-Sleep([int]$Milliseconds) {
    $global:MailboxNow = $global:MailboxNow.AddMilliseconds($Milliseconds)
    if ($script:mode -eq 'late') { [IO.File]::WriteAllLines($out, @((Get-Content -LiteralPath $cmdFile)[2], '10:00:00 ping: correct session 7')) }
}
$script:state = 'Ready'; $script:mode = 'ping'; $script:starts = 0
$r = Session 'ping' 1
$c = @(Get-Content -LiteralPath $cmdFile); $o = @(Get-Content -LiteralPath $out)
if ($c.Count -eq 3 -and $c[2] -and $o.Count -eq 2 -and $o[0] -eq $c[2] -and $r -eq $o[1] -and $r -match '^\S+ ping: .*session \d+$') { 'ok task echo' }
$script:mode = 'stale'; if ((Session 'ping' 1) -like 'no answer*') { 'ok wrong ID ignored' }
$script:mode = 'late'; if ((Session 'ping' 1) -like '*correct session 7') { 'ok matching ID accepted' }
$script:mode = 'stale'; $script:state = 'Running'; $before = Get-Content -LiteralPath $cmdFile -Raw; $starts = $script:starts
if ((Session 'windows' 1) -like 'no answer*' -and $script:starts -eq $starts -and (Get-Content -LiteralPath $cmdFile -Raw) -eq $before) { 'ok running task undisturbed' }
EOF
  mkdir -p "$tmp/mailbox"
  TEMP="$tmp/mailbox" LOCALAPPDATA="$tmp/mailbox" APPDATA="$tmp/mailbox" pwsh -NoProfile -NonInteractive -File "$tmp/mailbox.ps1" "$here/kit" > "$tmp/mail" 2>&1
  for w in 'task echo' 'wrong ID ignored' 'matching ID accepted' 'running task undisturbed'; do
    grep -qxF "ok $w" "$tmp/mail" && ok || { no "session mailbox: $w"; sed 's/^/    /' "$tmp/mail"; }
  done
  # deck 1's title matching, on PC session 1's real OCR readings (w16's 8.3 name, w15's slip)
  cat > "$tmp/match.ps1" << 'EOF'
. (Join-Path $args[0] 'match-lib.ps1')
$Aliases = @{ 'w16-flac-longpath-copy' = @('W16-FL~1') }
$rows = @('01-mp3-cbr320', '02-mp3-vbr2', '03-mp3-48k', '04-aac-256', '05-aac-128', '06-aac-48k', '07-alac-16', '08-alac-24-96k', '09-flac-16', '10-flac-24-48k',
  '11-flac-24-96k', '12-wav-16', '13-wav-24-48k', '14-wav-f32', '15-wav-f32-96k', '16-aiff-16', '17-aiff-24-48k', '18-aiff-24-96k', '19-mp3-cbr128', '20-wav-16-30s',
  'w01-wav-f32-quiet', 'w02-wav-f64-quiet', 'w03-wav-f32-over1', 'w04-wav-f64-over1', 'w05-flac-24-11025', 'w06-flac-24-352800', 'w07-flac-24-384000', 'w08-flac-5.1',
  'w09-wav-5.1', 'w10-mp4-video-first', 'w12-wav-3s-short', 'w13-flac-silent-stretch', 'w14-flac-mono-48k', 'w15-mp3-22k', 'w16-flac-longpath', 'w16-flac-longpath-copy',
  'fmt-aiff-44100-1ch-s16-aif', 'fmt-aiff-44100-1ch-s16-aiff', 'fmt-wav-pcm-44100-1ch-s16-wav', 'fmt-wav-pcm-44100-2ch-s16-wav') | Sort-Object
function Name($j) { if ($j -ge 0) { $rows[$j] } else { 'none' } }
"WI 6-FL-I=$(Name (Find-Row 'WI 6-FL-I' $rows $Aliases))"
"W16-FL~2=$(Name (Find-Row 'W16-FL~2' $rows $Aliases))"
"w16 full=$(Name (Find-Row 'w16-flac-longpath' $rows $Aliases))"
"copy label=$(Name (Find-Row 'w16-flac-longpath-copy' $rows $Aliases))"
"wi5-mp3-22k=$(Name (Find-Row 'wi5-mp3-22k' $rows $Aliases))"
"01-mp3-cbr320=$(Name (Find-Row '01-mp3-cbr320' $rows $Aliases))"
"rekordbox=$(Name (Find-Row 'rekordbox' $rows $Aliases))"
"vote w16=$(Name (Vote-Row @('WI 6-FL-I', 'WI 6-FL-I', 'WI 6-FL-I', 'WI 6-FL-I') $rows $Aliases))"
"vote w15=$(Name (Vote-Row @('wi5-mp3-22k', '0-mp3-22k', 'wi5-mp3-22k', '') $rows $Aliases))"
"vote split=$(Name (Vote-Row @('01-mp3-cbr320', '02-mp3-vbr2') $rows $Aliases))"
# the 1920x1080 screen's slip (2026-11-19): I for 1
"II-flac-24-96k=$(Name (Find-Row 'II-flac-24-96k' $rows $Aliases))"
"vote II=$(Name (Vote-Row @('', 'II-flac-24-96k', 'II-flac-24-96k', 'II-flac-24-96k') $rows $Aliases))"
# exact OCR match on 1-edit format pairs (e.g. 1ch vs 2ch, .aif vs .aiff) must still resolve uniquely
"fmt aif=$(Name (Find-Row 'fmt-aiff-44100-1ch-s16-aif' $rows $Aliases))"
"fmt aiff=$(Name (Find-Row 'fmt-aiff-44100-1ch-s16-aiff' $rows $Aliases))"
"fmt 2ch=$(Name (Find-Row 'fmt-wav-pcm-44100-2ch-s16-wav' $rows $Aliases))"
"fmt 9ch=$(Name (Find-Row 'fmt-wav-pcm-44100-9ch-s16-wav' $rows $Aliases))"
# the bridge's lines as bridge.c writes them; a refused hit is not a hit
$c = Count-Bridge @('2026-11-18 10:00:00  hit  0123456789ab (441000 samples)  [1 hits, 0 near, 0 misses]',
  '2026-11-18 10:00:01  hit  0123456789ab refused: its stems don''t add up to the chunk (a damaged or wrong entry): a miss',
  '2026-11-18 10:00:02  miss 0123456789ab (441000 samples)  [1 hits, 0 near, 1 misses]',
  '2026-11-18 10:00:03  near 0123456789ab for ba9876543210: shift 3, level 1.000 (cached 1.000)',
  '2026-11-18 10:00:04  near 0123456789ab rejected (level) for ba9876543210: shift 3')
"count=$($c.hit) $($c.refused) $($c.miss) $($c.near)"
"count none=$((Count-Bridge @()).hit)"
# the calibrated regions scaled to the screenshot (the PC at 1920x1080, 100 %, 2026-11-19)
"region full=$((Scale-Region 3840 @(530, 930, 900, 55)) -join ',')"
"region half=$((Scale-Region 1920 @(530, 930, 900, 55)) -join ',')"
"region deck=$((Scale-Region 1920 @(110, 430, 760, 70)) -join ',')"
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/match.ps1" "$here/kit" > "$tmp/m" 2>&1
  for w in 'WI 6-FL-I=w16-flac-longpath-copy' 'W16-FL~2=w16-flac-longpath-copy' 'w16 full=w16-flac-longpath' 'copy label=none' \
           'wi5-mp3-22k=w15-mp3-22k' '01-mp3-cbr320=01-mp3-cbr320' 'rekordbox=none' 'vote w16=w16-flac-longpath-copy' \
           'vote w15=w15-mp3-22k' 'vote split=none' 'count=1 1 1 1' 'count none=0' \
           'region full=530,930,900,55' 'region half=265,465,450,28' 'region deck=55,215,380,35' \
           'II-flac-24-96k=11-flac-24-96k' 'vote II=11-flac-24-96k' \
           'fmt aif=fmt-aiff-44100-1ch-s16-aif' 'fmt aiff=fmt-aiff-44100-1ch-s16-aiff' \
           'fmt 2ch=fmt-wav-pcm-44100-2ch-s16-wav' 'fmt 9ch=none'; do
    grep -qxF "$w" "$tmp/m" && ok || { no "title match: want $w"; sed 's/^/    /' "$tmp/m"; }
  done
  # the dialog watch (watch-lib.ps1, as midi-auto.ps1 dot-sources it), Session stubbed: each
  # Watch- function returns one line ('' or a STOP) and prints nothing
  cat > "$tmp/watch.ps1" << 'EOF'
$B = $args[1]; $script:said = @(); $script:attempts = @()
function Say([string]$s) { $script:said += $s; $s }
$script:q = New-Object System.Collections.Queue
function Session([string]$cmd, [int]$wait = 30, [string]$arg = '') {
    if ($cmd -eq 'shot') { return '10:00:00 shot C:\s.png' }
    if ($cmd -in 'closeupdate', 'closeinfo', 'stemsinit', 'closeagentnews') {
        $script:attempts += $(switch ($cmd) { 'closeinfo' { "info $arg" } 'stemsinit' { "init $arg" } 'closeagentnews' { "agent $arg" } default { $arg } })
        if (-not $script:q.Count) { return "unexpected $cmd" }
    }
    return $script:q.Dequeue()
}
# a simulated clock: Start-Sleep advances it (the STEMS engine wait)
$script:clock = [datetime]'2026-11-19T10:00:00'
function Get-Date { $script:clock }
function Start-Sleep([int]$Seconds) { $script:clock = $script:clock.AddSeconds($Seconds) }
. (Join-Path $args[0] 'watch-lib.ps1')
function W { $script:q.Enqueue('10:00:00 windows: ' + ($args -join ' || ')) }
function T([string]$name, $out, [string]$like) {
  $o = @($out); if ($o.Count -eq 1 -and $o[0] -is [string] -and $o[0] -like $like) { "ok $name" } else { "bad ${name}: $($o.Count) object(s): $($o -join ' ## ')" }
}
$rb = 'rekordbox | rekordbox | 1600x900 | 0x10'; $app = 'RB Stems Plus | RB Stems Plus | 800x600 | 0x20'
$desk = 'explorer | Program Manager | 3840x2160 | 0x30'; $dlg = 'rekordbox | Sign in | 500x400 | 0x40'
$q.Enqueue('10:00:00 windows: '); T 'empty list' (Watch-Start 't') ''
if ($null -ne $script:Base -and $script:Base.Count -eq 0) { 'ok empty baseline' } else { 'bad empty baseline' }
W $rb $desk; T 'start' (Watch-Start 't' -Allow -Rekordbox) ''
W $rb $desk; T 'no change' (Watch-Stop 't') ''
W $rb 'rekordbox | rekordbox | 400x300 | 0x11'; T 'same title, another window' (Watch-Stop 't') 't: STOP: new window(s): rekordbox | rekordbox | 0x11; 10:00:00 shot *'
W 'rekordbox | rekordbox | 1600x900 | 0x12'; T 'same title, replaced' (Watch-Stop 't') 't: STOP: new window(s): rekordbox | rekordbox | 0x12;*'
$q.Enqueue('no answer from the session task in 30 s'); T 'failed poll' (Watch-Stop 't') 't: STOP: the window list failed*'
W $rb 'powershell | RBSP watch test | 480x200 | 0x50'; T 'test window marked' (Watch-Stop 't') '*: powershell | RBSP watch test | 0x50 (watch test); 10:00:00 shot*'
W $rb 'powershell | RBSP watch test | 480x200 | 0x50' $dlg; T 'test window and a dialog' (Watch-Stop 't') '*rekordbox | Sign in | 0x40; 10:00:00 shot*'
W $rb 'rekordbox | a | b | 300x300 | 0x60'; T 'a title with a bar' (Watch-Stop 't') '*new window(s): rekordbox | a | b | 0x60;*'
W $rb 'rekordbox | Sign in || Continue | 500x400 | 0x41'; T 'a title with two bars' (Watch-Stop 't') 't: STOP: the window list failed*'
W $rb 'rekordbox | Sign in | 500x400 | 0xGG'; T 'bad window handle' (Watch-Stop 't') 't: STOP: the window list failed*'
W $rb 'rekordbox | Sign in | unknown | 0x41'; T 'bad window size' (Watch-Stop 't') 't: STOP: the window list failed*'
W $rb $dlg; T 'a dialog at the start' (Watch-Start 't' -Allow -Rekordbox) 't: STOP: not just*Sign in*'
W 'rekordbox | rekordbox | Sign in | 500x400 | 0x41'; T 'a title starting like the main window' (Watch-Start 't' -Allow -Rekordbox) 't: STOP: not just*Sign in*'
W; T 'no rekordbox at the start' (Watch-Start 't' -Allow -Rekordbox) 't: STOP: not just*'
W $app; T 'the app alone (prepare)' (Watch-Start 't' -Allow) ''
W $rb $app 'RB Stems Plus | RB Stems Plus | 400x200 | 0x21'; T 'two app windows' (Watch-Start 't' -Allow) 't: STOP: not just*'
W $rb $app; T 'rekordbox and the app' (Watch-Start 't' -Allow -Rekordbox) ''
Watch-Save; W $rb $app $dlg; T 'quit, run''s baseline' (Watch-Quit) 'quit: STOP: new window(s): rekordbox | Sign in | 0x40;*'
W 'rekordbox | rekordbox | 1600x900 | 0x13'; T 'quit, another rekordbox' (Watch-Quit) 'quit: STOP: new window(s)*0x13;*'
Remove-Item -LiteralPath $WatchBase; W $rb; T 'quit, no baseline' (Watch-Quit) ''
W $rb $dlg; T 'quit, no baseline, a dialog' (Watch-Quit) 'quit: STOP: not just*'
if (-not $script:said.Count) { 'ok nothing said' } else { "bad said $script:said" }
$up = 'Upmgr rekordbox | rekordbox update manager | 518x317 | 0x71'
$info = 'rekordbox | INFO | 816x647 | 0x72'
$closed = '10:00:00 closeupdate: closed; before shot one.png; 7.2.19 -> 7.2.20; UI Automation invoked Close; after shot two.png'
W $rb $up; $q.Enqueue($closed); W $rb
T 'updater at start' (Watch-Start 'start' -Allow -Rekordbox) ''
T 'update is not in baseline' ($script:Base -join '; ') 'rekordbox | rekordbox | 0x10'
$script:Base = @('rekordbox | rekordbox | 0x10')
$q.Clear()
W $rb $up; $q.Enqueue($closed); W $rb
T 'updater during run' (Watch-Stop 'run') ''
T 'close logged with versions and screenshots' ($script:said -join ' || ') "start: $closed || run: $closed"
$q.Clear()
W $rb ($up -replace '^Upmgr rekordbox', 'other')
T 'same title, different process' (Watch-Stop 'run') 'run: STOP: new window(s): other | rekordbox update manager | 0x71;*'
$q.Clear()
W $rb ($up -replace 'update manager', 'Update available')
T 'updater process, different title' (Watch-Stop 'run') 'run: STOP: new window(s): Upmgr rekordbox | rekordbox Update available | 0x71;*'
W $rb ($up -replace '^Upmgr rekordbox', 'other')
T 'look-alike before MIDI' (Watch-Start 'start' -Allow -News -BeforeMidi) 'start: STOP: new window(s): other | rekordbox update manager | 0x71;*'
T 'only exact candidates reached the task' ($script:attempts -join ' ') '0x71 0x71'
$env:RBKIT_STOP_ON_UPDATE = '1'
W $rb $up
T 'stop on verified update before MIDI' (Watch-Start 'start' -Allow -News -BeforeMidi) 'start: STOP: new window(s): Upmgr rekordbox | rekordbox update manager | 0x71;*'
T 'strict startup did not close the offer' ($script:attempts -join ' ') '0x71 0x71'
W $rb $up; $q.Enqueue($closed); W $rb
T 'strict mode still permits verified close during quit' (Watch-Quit) ''
$env:RBKIT_STOP_ON_UPDATE = ''
# rekordbox's news window (INFO): closed after the offer, by its own command; look-alikes STOP
$infoClosed = '10:00:00 closeinfo: closed; 0x72 pid 7; before shot three.png; UI Automation closed the window; after shot four.png'
$script:attempts = @(); $script:said = @()
W $rb $up $info; $q.Enqueue($closed); W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'offer and INFO closed during run' (Watch-Stop 'run') ''
T 'offer then INFO, each by its command' ($script:attempts -join ' ') '0x71 info 0x72'
T 'INFO close logged' ($script:said -join ' || ') "run: $closed || run: $infoClosed"
$q.Clear()
W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'INFO closed before MIDI' (Watch-Start 'start' -Allow -News -BeforeMidi) ''
$q.Clear()
W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'INFO closed at start' (Watch-Start 'start' -Allow -Rekordbox -News) ''
$q.Clear(); $script:attempts = @()
W $rb 'RB Stems Plus | INFO | 816x647 | 0x73'
T 'INFO of another process' (Watch-Stop 'run') 'run: STOP: new window(s): RB Stems Plus | INFO | 0x73;*'
W $rb $info 'rekordbox | INFO | 816x647 | 0x74'
T 'two INFO windows' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | INFO | 0x72; rekordbox | INFO | 0x74;*'
W $rb 'rekordbox | INFO extra | 816x647 | 0x75'
T 'INFO title look-alike' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | INFO extra | 0x75;*'
T 'look-alikes never reached the task' "$($script:attempts.Count)" '0'
W $rb $info; $q.Enqueue('10:00:00 closeinfo: the news window is still open after Close')
T 'INFO close failure propagated' (Watch-Stop 'run') 'run: STOP: 10:00:00 closeinfo: the news window is still open after Close;*'
$q.Clear()
W $rb $info; $q.Enqueue($infoClosed); W $rb $info
T 'INFO still listed after a closed reply' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | INFO | 0x72;*'
$q.Clear()
W $rb $info; $q.Enqueue($infoClosed); W $rb $dlg
T 'a dialog after INFO closed' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | Sign in | 0x40;*'
$q.Clear()
W $rb $up; $q.Enqueue('10:00:00 closeupdate: the updater window is still open after Close')
T 'close failure propagated' (Watch-Stop 'run') 'run: STOP: 10:00:00 closeupdate: the updater window is still open after Close;*'
$q.Clear()
W $rb $up; $q.Enqueue($closed); W $rb $up
$before = WinList
T 'success reply with window still listed' (Watch-Update 'run' ([ref]$before)) 'run: STOP: new window(s): Upmgr rekordbox | rekordbox update manager | 0x71;*'
$q.Clear()
W $rb $up; $q.Enqueue($closed); W $rb ($up -replace 'update manager', 'Closing')
$before = WinList
T 'success reply with retitled window still listed' (Watch-Update 'run' ([ref]$before)) 'run: STOP: new window(s): Upmgr rekordbox | rekordbox Closing | 0x71;*'
# quit closes the verified offer too, then applies its usual check (start rule or run's baseline)
$q.Clear(); Remove-Item -LiteralPath $WatchBase -ErrorAction SilentlyContinue
W $rb $up; $q.Enqueue($closed); W $rb
T 'quit, no baseline, updater closed' (Watch-Quit) ''
W $rb $up $info; $q.Enqueue($closed); W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'quit, no baseline, offer and INFO closed' (Watch-Quit) ''
$script:Base = @('rekordbox | rekordbox | 0x10'); Watch-Save; $q.Clear()
W $rb $up; $q.Enqueue($closed); W $rb
T 'quit, run''s baseline, updater closed' (Watch-Quit) ''
W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'quit, run''s baseline, INFO closed' (Watch-Quit) ''
W $rb $info $dlg; $q.Enqueue($infoClosed); W $rb $dlg
T 'quit, INFO closed, a dialog still STOPs' (Watch-Quit) 'quit: STOP: new window(s): rekordbox | Sign in | 0x40;*'
# rekordbox's "Initializing STEMS Engine" progress window: verified, waited out, never touched
$init1 = 'rekordbox | Initializing STEMS Engine | 400x120 | 0x80'; $init2 = 'rekordbox | Initializing STEMS Engine | 400x120 | 0x81'
$ok80 = '10:00:00 stemsinit: ok; 0x80 pid 7; no buttons'; $ok81 = '10:00:00 stemsinit: ok; 0x81 pid 7; no buttons'
T 'the wait cap (owner: usually 5-10 s)' "$InitWait" '60'
$q.Clear(); $script:attempts = @(); $script:said = @()
W $rb $init1 $init2; $q.Enqueue($ok80); $q.Enqueue($ok81); W $rb $init1 $init2; W $rb
T 'two progress windows waited out at start' (Watch-Start 'start' -Allow -Rekordbox -News) ''
T 'each progress window verified once' ($script:attempts -join ' ') 'init 0x80 init 0x81'
T 'the wait logged' ($script:said -join ' || ') "start: waiting for rekordbox's STEMS engine: $ok80 || start: waiting for rekordbox's STEMS engine: $ok81 || start: rekordbox's STEMS engine progress window(s) gone"
$q.Clear(); $script:attempts = @()
W $rb $init1; $q.Enqueue($ok80); W $rb $init1 $init2; $q.Enqueue($ok81); W $rb
T 'a progress window during run, another during the wait' (Watch-Stop 'run') ''
T 'the later one verified too' ($script:attempts -join ' ') 'init 0x80 init 0x81'
$q.Clear(); $InitWait = 6
W $rb $init1; $q.Enqueue($ok80); W $rb $init1; W $rb $init1; W $rb $init1; W $rb $init1
T 'still there after the wait' (Watch-Stop 'run') 'run: STOP: rekordbox still initializing its STEMS engine after 6 s: rekordbox | Initializing STEMS Engine | 0x80;*'
$q.Clear(); $InitWait = 60; $script:attempts = @()
W $rb $init1; $q.Enqueue('10:00:00 stemsinit: the progress window has buttons: ''Cancel''')
T 'a failed check STOPs' (Watch-Stop 'run') 'run: STOP: 10:00:00 stemsinit: the progress window has buttons*'
$q.Clear(); $script:attempts = @()
W $rb 'RB Stems Plus | Initializing STEMS Engine | 400x120 | 0x82'
T 'a look-alike STOPs' (Watch-Stop 'run') 'run: STOP: new window(s): RB Stems Plus | Initializing STEMS Engine | 0x82;*'
W $rb 'rekordbox | Initializing STEMS Engine... | 400x120 | 0x83'
T 'a title look-alike STOPs' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | Initializing STEMS Engine... | 0x83;*'
T 'look-alikes never checked' "$($script:attempts.Count)" '0'
$q.Clear()
W $rb $init1; $q.Enqueue($ok80); W $rb $init1 $dlg; W $rb $dlg
T 'a dialog during the wait still STOPs' (Watch-Stop 'run') 'run: STOP: new window(s): rekordbox | Sign in | 0x40;*'
$q.Clear()
W $rb $init1; $q.Enqueue($ok80); W $rb $info; $q.Enqueue($infoClosed); W $rb
T 'the progress window, then INFO closed' (Watch-Stop 'run') ''
$q.Clear()
W $rb $init1; $q.Enqueue($ok80); W $rb
T 'quit waits it out' (Watch-Quit) ''
# rekordboxAgent's news window: closed by its own command wherever it blocks (quit included)
$agentWin = 'rekordboxAgent | rekordboxAgent | 566x375 | 0x90'
$agentClosed = '10:00:00 closeagentnews: closed; 0x90 pid 9; before shot five.png; UI Automation closed the window; after shot six.png'
$q.Clear(); $script:attempts = @()
W $rb $agentWin; $q.Enqueue($agentClosed); W $rb
T 'quit, run''s baseline, agent news closed' (Watch-Quit) ''
Remove-Item -LiteralPath $WatchBase
W $rb $agentWin; $q.Enqueue($agentClosed); W $rb
T 'quit, no baseline, agent news closed' (Watch-Quit) ''
W $rb $agentWin; $q.Enqueue($agentClosed); W $rb
T 'agent news closed at start' (Watch-Start 'start' -Allow -Rekordbox -News) ''
T 'agent news closed by its command' ($script:attempts -join ' ') 'agent 0x90 agent 0x90 agent 0x90'
$q.Clear(); $script:attempts = @()
W $rb 'rekordboxAgent | Sign in | 500x400 | 0x91'
T 'another agent window STOPs' (Watch-Stop 'run') 'run: STOP: new window(s): rekordboxAgent | Sign in | 0x91;*'
W $rb $agentWin 'rekordboxAgent | rekordboxAgent | 566x375 | 0x92'
T 'two agent news windows STOP' (Watch-Stop 'run') 'run: STOP: new window(s): rekordboxAgent | rekordboxAgent | 0x90; rekordboxAgent | rekordboxAgent | 0x92;*'
T 'those never reached the task' "$($script:attempts.Count)" '0'
W $rb $agentWin; $q.Enqueue('10:00:00 closeagentnews: the agent window has an owner window (a dialog)')
T 'agent close failure propagated' (Watch-Stop 'run') 'run: STOP: 10:00:00 closeagentnews: the agent window has an owner window*'
$q.Clear(); $script:attempts = @(); $script:Base = @('rekordbox | rekordbox | 0x10'); Watch-Save
W $rb $init1 $up $info $agentWin; $q.Enqueue($ok80); W $rb $up $info $agentWin; $q.Enqueue($closed); W $rb $info $agentWin
$q.Enqueue($infoClosed); W $rb $agentWin; $q.Enqueue($agentClosed); W $rb
T 'quit: progress waited, offer, INFO and agent news closed' (Watch-Quit) ''
T 'in that order' ($script:attempts -join ' ') 'init 0x80 0x71 info 0x72 agent 0x90'
$q.Clear()
W $rb $init1 $init2; $q.Enqueue($ok80); $q.Enqueue('10:00:00 stemsinit: gone; 0x81'); W $rb
T 'a progress window gone before its check (live 2026-11-19)' (Watch-Stop 'run') ''
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/watch.ps1" "$here/kit" "$tmp" > "$tmp/w" 2>&1
  n="$(grep -c '^ok ' "$tmp/w")"; pass=$((pass + n))
  [ "$n" = 80 ] && ! grep -qv '^ok ' "$tmp/w" || { no "the dialog watch ($n of 80 OK):"; grep -v '^ok ' "$tmp/w" | sed 's/^/    /'; }
  # The real appuninstall mailbox dispatch, with Win32 and UI Automation modelled.
  mkdir "$tmp/app-uninstall"
  cp "$here/kit/watch-lib.ps1" "$tmp/app-uninstall/"
  cat > "$tmp/app-uninstall/kit-lib.ps1" << 'EOF'
$B = $PSScriptRoot
function Say($line) { Write-Host $line }
EOF
  cat > "$tmp/app-uninstall.ps1" << 'EOF'
param($source, $fixture)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Linq;
public static class RbspWin {
  public static string Mode, Trace;
  public static int Step;
  public static string Apps;
  public static bool OrdinaryDesktop() { return true; }
  public static int Pid(IntPtr h) { return 42; }
  public static string[] List() {
    var main = "RB Stems Plus | RB Stems Plus | 640x480 | 0x10";
    if (Step == 0) return new[] { main };
    return new[] { main, "RB Stems Plus | RB Stems Plus | 400x300 | 0x" + (16 + Step).ToString("X") };
  }
  public static string Title() {
    if (Step == 1) return "Uninstall RB Stems Plus?";
    if (Step == 2 && Mode == "unrelated") return "Sign in";
    return Step == 2 ? "Stems Cache from another account" : "RB Stems Plus is uninstalled";
  }
  public static string[] Buttons() {
    if (Step == 1) return new[] { "Uninstall", "Cancel" };
    if (Step == 2 && Mode != "unrelated")
      return new[] { Mode == "wrong buttons" ? "Cancel" : "Remove", "Keep" };
    return new[] { "Done" };
  }
}
namespace Windows.Automation {
  public enum ControlType { Window, Button, TitleBar, MenuItem }
  public enum TreeScope { Children, Descendants }
  public class ElementNotAvailableException : Exception {}
  public class PropertyCondition {
    public PropertyCondition(object property, object value) {}
  }
  public class TreeWalker {
    public static TreeWalker ControlViewWalker = new TreeWalker();
    public AutomationElement GetParent(AutomationElement e) {
      return new AutomationElement { ControlType = e.Name == "Close" ? ControlType.TitleBar : ControlType.Window };
    }
  }
  public class AutomationElement {
    public static object ControlTypeProperty = new object();
    public AutomationElement Current { get { return this; } }
    public string Name;
    public bool IsEnabled = true, IsOffscreen = false;
    public int ProcessId = 42, NativeWindowHandle;
    public ControlType ControlType;
    public static AutomationElement FromHandle(IntPtr h) {
      return new AutomationElement { Name = RbspWin.Title(), NativeWindowHandle = (int)h.ToInt64(),
        ControlType = ControlType.Window };
    }
    public AutomationElement[] FindAll(TreeScope scope, PropertyCondition cond) {
      if (scope == TreeScope.Children) return new AutomationElement[0];
      return RbspWin.Buttons().Concat(new[] { "Close" })
        .Select(name => new AutomationElement { Name = name, ControlType = ControlType.Button }).ToArray();
    }
    public bool TryGetCurrentPattern(object kind, out InvokePattern pattern) {
      pattern = new InvokePattern { Name = Name };
      return true;
    }
  }
  public class ExpandCollapsePattern { public static object Pattern = new object(); }
  public class InvokePattern {
    public static object Pattern = new object();
    public string Name;
    public void Expand() {}
    public void Invoke() {
      RbspWin.Trace += " " + Name;
      if (Name == "menu" || Name == "Uninstall" || Name == "Keep") RbspWin.Step++;
      if (Name == "Done") {
        System.IO.Directory.Delete(RbspWin.Apps);
        RbspWin.Step = 4;
      }
    }
  }
}
'@
$stub = @'
function Uia {}
function Restore-App {}
function Get-App {
    $app = [pscustomobject]@{ Id = 42; MainWindowHandle = [IntPtr]16 }
    $app | Add-Member ScriptMethod Refresh {}
    $app | Add-Member ScriptProperty HasExited { [RbspWin]::Step -eq 4 }
    return $app
}
function Find-El($app, $name, $type, $secs, [switch]$Enabled) {
    if ($name -like 'Uninstall RB Stems Plus*') { $name = 'menu' }
    if ($type -eq [Windows.Automation.ControlType]::MenuItem -or
        ([RbspWin]::Step -gt 0 -and $name -in [RbspWin]::Buttons())) {
        return [Windows.Automation.AutomationElement]@{ Name = $name }
    }
}
function Start-Sleep {
    if (++$script:polls -gt 10) { throw 'fixture stalled waiting for a dialog to be answered' }
}
try {
'@
$env:LOCALAPPDATA = $fixture
[RbspWin]::Apps = Join-Path $fixture 'Programs\RB Stems Plus'
$task = Join-Path $fixture 'session-task.ps1'
(Get-Content -LiteralPath $source -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
function Case($mode, $trace, $answer, $keepLog = '') {
    [void][IO.Directory]::CreateDirectory([RbspWin]::Apps)
    [RbspWin]::Mode = $mode; [RbspWin]::Step = 0; [RbspWin]::Trace = ''
    $script:polls = 0
    $reply = Join-Path $fixture 'task-out.txt'
    Remove-Item -LiteralPath $reply -ErrorAction SilentlyContinue
    Set-Content -LiteralPath (Join-Path $fixture 'task-cmd.txt') -Value @('appuninstall', '', 'request')
    $log = & $task 6>&1
    $out = @(Get-Content -LiteralPath $reply)
    if ([RbspWin]::Trace -ceq " menu Uninstall$trace" -and $out.Count -eq 2 -and $out[0] -eq 'request' -and
        $out[1].EndsWith(" appuninstall: $answer") -and "$log" -ceq $keepLog) { "ok $mode" }
    else { "bad ${mode}: $out (invoked:$([RbspWin]::Trace), log: $log)" }
}
Case 'keep' ' Keep Done' 'ok; removed by its own uninstall' "appuninstall: chose Keep on 'Stems Cache from another account'"
Case 'unrelated' '' 'unexpected app dialog during uninstall'
Case 'wrong buttons' '' 'unexpected app dialog buttons during uninstall'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/app-uninstall.ps1" "$here/kit/session-task.ps1" "$tmp/app-uninstall" > "$tmp/app-uninstall-out" 2>&1
  n="$(grep -c '^ok ' "$tmp/app-uninstall-out")"; pass=$((pass + n))
  [ "$n" = 3 ] && ! grep -qv '^ok ' "$tmp/app-uninstall-out" || { no "appuninstall ($n of 3 OK):"; grep -v '^ok ' "$tmp/app-uninstall-out" | sed 's/^/    /'; }
  # the real session task's closeagent dispatch, with the desktop APIs and screenshot stubbed
  mkdir "$tmp/agent"
  cat > "$tmp/agent/kit-lib.ps1" << 'EOF'
function Add-Type {}   # the test already defined the interop types
function Get-Process { [pscustomobject]@{ MainWindowHandle = 16; MainWindowTitle = 'rekordboxAgent' } }
EOF
  cat > "$tmp/agent-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
public static class RbspWin { public static string[] Windows; public static string[] List() { return Windows; } }
namespace Rbsp {
  public static class Wm {
    public static string Trace;
    public static bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l) {
      Trace += " close " + h + " " + m;
      return true;
    }
  }
}
'@
$stub = @'
function Shot {
    [Rbsp.Wm]::Trace += ' shot'
    if ($failShot) { throw 'shot failed' }
    'shot fixture.png'
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$agent = 'rekordboxAgent | rekordboxAgent | 400x300 | 0x10'
function Case($name, [string[]]$windows, [string]$trace, [string]$answer, [bool]$failShot = $false) {
    [RbspWin]::Windows = $windows; [Rbsp.Wm]::Trace = ''
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    $id = [guid]::NewGuid().ToString()
    Set-Content -LiteralPath $cmdFile -Value @('closeagent', '', $id)
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([Rbsp.Wm]::Trace -eq $trace -and $out.Count -eq 2 -and $out[0] -eq $id -and $out[1] -like "* $answer") { "ok $name" }
    else { "bad ${name}: $out (trace:$([Rbsp.Wm]::Trace))" }
}
Case 'zero' @('other | rekordboxAgent | 400x300 | 0x11') '' 'closeagent: 0 window(s) asked to close'
Case 'one' @($agent) ' shot close 16 16' 'closeagent: 1 window(s) asked to close; shot fixture.png'
Case 'two' @($agent, 'rekordboxAgent | rekordboxAgent | 400x300 | 0x11') '' 'closeagent: 2 rekordboxAgent windows: none closed'
Case 'failed shot' @($agent) ' shot' 'error: shot failed' $true
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/agent-test.ps1" "$here/kit/session-task.ps1" "$tmp/agent" > "$tmp/a" 2>&1
  n="$(grep -c '^ok ' "$tmp/a")"; pass=$((pass + n))
  [ "$n" = 4 ] && ! grep -qv '^ok ' "$tmp/a" || { no "closeagent ($n of 4 OK):"; grep -v '^ok ' "$tmp/a" | sed 's/^/    /'; }
  # Run the real updater dispatch with a desktop, UIA controls and OCR text in place of Windows.
  mkdir "$tmp/update"
  cat > "$tmp/update/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process([int]$Id) {
    @{ Id = $Id; ProcessName = 'Upmgr rekordbox'; SessionId = $sid; Path = [RbspWin]::Image }
}
EOF
  cat > "$tmp/update/ocr-lib.ps1" << 'EOF'
function Read-ScreenText($png, $x, $y, $w, $h, [switch]$Plain) {
    if ($png -ne 'before.png' -or "$x $y $w $h" -ne '710 378 500 275' -or -not $Plain) { throw 'wrong OCR crop' }
    if ([RbspWin]::Mode -eq 'other text') { return 'Other offer. 7.2.19 7.2.20' }
    'The following update packages were found. Current Version Update Version rekordbox 7.2.19 7.2.20'
}
EOF
  cat > "$tmp/update-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public static string Mode, Trace, Image;
  public static string[] Windows;
  public static bool Open;
  public static string[] List() { return Windows; }
  public static int Pid(IntPtr h) { return h.ToInt64() == 113 ? 42 : 99; }
  public static bool OrdinaryDesktop() { return Mode != "secure desktop"; }
  public static IntPtr GetWindow(IntPtr h, uint relation) { return IntPtr.Zero; }
  public static bool IsWindow(IntPtr h) { return Open; }
  public static object UncoveredClient(IntPtr h) { return new Rect(710, 378, 1210, 653); }
}
public struct Rect {
  public int L, T, R, B;
  public Rect(int l, int t, int r, int b) { L = l; T = t; R = r; B = b; }
  public int Left { get { return L; } }
  public int Top { get { return T; } }
  public int Right { get { return R; } }
  public int Bottom { get { return B; } }
}
namespace System.Windows.Forms {
  public static class SystemInformation {
    public static Rect VirtualScreen { get { return new Rect(0, 0, 1920, 1080); } }
  }
}
namespace Windows.Automation {
  public enum TreeScope { Descendants }
  public enum ControlType { Button, TitleBar, Window }
  public class PropertyCondition {
    public object Value;
    public PropertyCondition(object property, object value) { Value = value; }
  }
  public class TreeWalker {
    public static TreeWalker ControlViewWalker = new TreeWalker();
    public AutomationElement GetParent(AutomationElement e) { return e.Parent; }
  }
  public class AutomationElement {
    public static object ControlTypeProperty = new object();
    public AutomationElement Current { get { return this; } }
    public AutomationElement Parent;
    public ControlType ControlType;
    public string Name, AutomationId, FrameworkId = "JUCE";
    public int ProcessId = 42, NativeWindowHandle;
    public bool IsOffscreen, IsEnabled = true;
    public static AutomationElement FromHandle(IntPtr h) {
      // as the PC's JUCE updater: named after the component, not the Win32 title
      var name = RbspWin.Mode == "title as UIA name" ? "rekordbox update manager" : "Upmgr rekordbox";
      return new AutomationElement { Name = name, AutomationId = name, ProcessId = RbspWin.Pid(h),
        FrameworkId = RbspWin.Mode == "not JUCE" ? "Win32" : "JUCE",
        NativeWindowHandle = RbspWin.Mode == "other UIA handle" ? 0 : (int)h.ToInt64(),
        ControlType = ControlType.Window };
    }
    public AutomationElement[] FindAll(TreeScope scope, PropertyCondition cond) {
      if (!Equals(cond.Value, ControlType.Button)) throw new InvalidOperationException("expected Button lookup");
      var caption = new AutomationElement { ControlType = ControlType.TitleBar, Parent = this };
      var start = new AutomationElement { Name = "Start", AutomationId = "Start.<empty>.Upmgr rekordbox", Parent = this, ControlType = ControlType.Button };
      var close = new AutomationElement { Name = "Close", AutomationId = "Close.<empty>.Upmgr rekordbox", Parent = this, ControlType = ControlType.Button };
      var systemClose = new AutomationElement { Name = "Close", AutomationId = "Close", Parent = caption, ControlType = ControlType.Button };
      var nestedClose = new AutomationElement { Name = "Close", AutomationId = "Close.<empty>.<empty>.Upmgr rekordbox", Parent = this, ControlType = ControlType.Button };
      if (RbspWin.Mode == "nested Close only") return new[] { start, nestedClose, systemClose };
      if (RbspWin.Mode == "no Start") return new[] { close, systemClose };
      if (RbspWin.Mode == "caption Close only") return new[] { start, systemClose };
      return new[] { start, close, systemClose };
    }
    public bool TryGetCurrentPattern(object pattern, out InvokePattern invoke) {
      invoke = new InvokePattern { Button = Name };
      return RbspWin.Mode != "no Invoke";
    }
  }
  public class InvokePattern {
    public static object Pattern = new object();
    public string Button;
    public void Invoke() {
      RbspWin.Trace += " invoke " + Button;
      if (RbspWin.Mode != "sticky") RbspWin.Open = false;
    }
  }
}
'@
$stub = @'
function Uia {}
function Screen-Dpi {}
function Shot {
    if ([RbspWin]::Trace -eq '') { [RbspWin]::Trace += ' before'; return 'shot before.png' }
    [RbspWin]::Trace += ' after'; return 'shot after.png'
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:ProgramFiles = Join-Path $B 'Program Files'
$install = Join-Path $env:ProgramFiles 'rekordbox'
$versionDir = Join-Path $install 'rekordbox 7.2.19'
[void][IO.Directory]::CreateDirectory($versionDir)
New-Item -ItemType File -Path (Join-Path $versionDir 'rekordbox.exe') | Out-Null
New-Item -ItemType File -Path (Join-Path $B 'rekordbox.exe') | Out-Null
$image = "$install\rekordbox 7.2.19\Upmgr rekordbox.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$up = 'Upmgr rekordbox | rekordbox update manager | 518x317 | 0x71'
$info = 'rekordbox | INFO | 816x647 | 0x72'
function Case($mode, $row, $trace, $answer, [string]$path = $image) {
    [RbspWin]::Windows = @($row, $info); [RbspWin]::Mode = $mode; [RbspWin]::Image = $path
    [RbspWin]::Trace = ''; [RbspWin]::Open = $true
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('closeupdate', '0x71', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspWin]::Trace -eq $trace -and $out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -like "* closeupdate: $answer") { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspWin]::Trace))" }
}
Case 'close only' $up ' before invoke Close after' 'closed;*owner 0x0; before shot before.png; 7.2.19 -> 7.2.20; UI Automation invoked Close; after shot after.png'
Case 'no Start' $up ' before' 'expected one Start and one enabled Close button*'
Case 'caption Close only' $up ' before' 'expected one Start and one enabled Close button*'
Case 'nested Close only' $up ' before' 'expected one Start and one enabled Close button*'
Case 'title as UIA name' $up ' before' "the UI Automation dialog did not match the window (name 'rekordbox update manager'*"
Case 'not JUCE' $up ' before' 'the UI Automation dialog did not match the window*'
Case 'other UIA handle' $up ' before' 'the UI Automation dialog did not match the window*'
Case 'wrong process at dispatch' ($up -replace '^Upmgr rekordbox', 'rekordbox') '' 'the updater process name or window title did not match*'
Case 'wrong title at dispatch' ($up -replace 'update manager', 'Update available') '' 'the updater process name or window title did not match*'
Case 'wrong path' $up '' 'the window process image is outside the rekordbox install folders*' "$B\Upmgr rekordbox.exe"
Case 'other text' $up ' before' 'the updater text did not contain "update packages"*'
Case 'secure desktop' $up '' 'ordinary input desktop unavailable*'
Case 'no Invoke' $up ' before after' 'Close has no Invoke pattern;*before shot before.png; 7.2.19 -> 7.2.20; after shot after.png'
Case 'sticky' $up ' before invoke Close after' 'the updater window is still open after Close;*before shot before.png; 7.2.19 -> 7.2.20; UI Automation invoked Close; after shot after.png'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/update-test.ps1" "$here/kit/session-task.ps1" "$tmp/update" > "$tmp/u" 2>&1
  n="$(grep -c '^ok ' "$tmp/u")"; pass=$((pass + n))
  [ "$n" = 14 ] && ! grep -qv '^ok ' "$tmp/u" || { no "closeupdate ($n of 14 OK):"; grep -v '^ok ' "$tmp/u" | sed 's/^/    /'; }
  # The real closeinfo dispatch (rekordbox's news window, INFO), the PC's JUCE structure in place of Windows.
  mkdir "$tmp/info"
  cat > "$tmp/info/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process([int]$Id) { @{ Id = $Id; ProcessName = 'rekordbox'; SessionId = $sid; Path = [RbspWin]::Image } }
EOF
  cat > "$tmp/info-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public static string Mode, Trace, Image;
  public static string[] Windows;
  public static bool Open, Visible, Listed;
  // on the PC the type exists only once Get-DesktopWindows has run (live PC, 2026-11-19)
  static void Defined() { if (!Listed) throw new InvalidOperationException("Unable to find type [RbspWin]."); }
  public static string[] List() { Listed = true; return Windows; }
  public static int Pid(IntPtr h) { Defined(); return h.ToInt64() == 114 ? 42 : 99; }
  public static bool OrdinaryDesktop() { Defined(); return Mode != "secure desktop"; }
  public static bool IsWindow(IntPtr h) { return Open; }
  public static bool IsWindowVisible(IntPtr h) { return Visible; }
}
namespace Windows.Automation {
  public enum TreeScope { Descendants }
  public enum ControlType { Button, Window }
  public class PropertyCondition {
    public object Value;
    public PropertyCondition(object property, object value) { Value = value; }
  }
  public class AutomationElement {
    public static object ControlTypeProperty = new object();
    public AutomationElement Current { get { return this; } }
    public ControlType ControlType;
    public string Name, AutomationId, FrameworkId = "JUCE";
    public int ProcessId = 42, NativeWindowHandle;
    public bool IsOffscreen;
    // as the PC's JUCE news window: "INFO", its tabs "<tab>.<empty>.<empty>.INFO"
    public static AutomationElement FromHandle(IntPtr h) {
      return new AutomationElement { Name = RbspWin.Mode == "other UIA name" ? "rekordbox" : "INFO", AutomationId = "INFO",
        FrameworkId = RbspWin.Mode == "not JUCE" ? "Win32" : "JUCE", ProcessId = RbspWin.Pid(h),
        NativeWindowHandle = (int)h.ToInt64(), ControlType = ControlType.Window };
    }
    static AutomationElement Tab(string name, string id) { return new AutomationElement { Name = name, AutomationId = id, ControlType = ControlType.Button }; }
    public AutomationElement[] FindAll(TreeScope scope, PropertyCondition cond) {
      if (!Equals(cond.Value, ControlType.Button)) throw new InvalidOperationException("expected Button lookup");
      var info = Tab("INFORMATION", "INFORMATION.<empty>.<empty>.INFO");
      var note = Tab("NOTIFICATION", "NOTIFICATION.<empty>.<empty>.INFO");
      var account = Tab("someone", "someone.<empty>.<empty>.INFO");
      if (RbspWin.Mode == "no NOTIFICATION tab") return new[] { info, account };
      if (RbspWin.Mode == "INFORMATION elsewhere") return new[] { Tab("INFORMATION", "INFORMATION.<empty>.INFO"), note, account };
      return new[] { info, note, account };
    }
    public bool TryGetCurrentPattern(object pattern, out object p) {
      p = new WindowPattern();
      return RbspWin.Mode != "no window pattern";
    }
  }
  public class WindowPattern {
    public static object Pattern = new object();
    public void Close() {
      RbspWin.Trace += " close";
      if (RbspWin.Mode == "hidden by Close") RbspWin.Visible = false;
      else if (RbspWin.Mode != "sticky") RbspWin.Open = false;
    }
  }
}
'@
$stub = @'
function Uia {}
function Screen-Dpi {}
function Shot {
    if ([RbspWin]::Trace -eq '') { [RbspWin]::Trace += ' before'; return 'shot before.png' }
    [RbspWin]::Trace += ' after'; return 'shot after.png'
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:ProgramFiles = Join-Path $B 'Program Files'
$image = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordbox.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$info = 'rekordbox | INFO | 816x647 | 0x72'
function Case($mode, $row, $trace, $answer, [string]$path = $image) {
    [RbspWin]::Windows = @('rekordbox | rekordbox | 1936x1048 | 0x10', $row); [RbspWin]::Mode = $mode; [RbspWin]::Image = $path
    [RbspWin]::Trace = ''; [RbspWin]::Open = $true; [RbspWin]::Visible = $true; [RbspWin]::Listed = $false
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('closeinfo', '0x72', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspWin]::Trace -eq $trace -and $out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -like "* closeinfo: $answer") { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspWin]::Trace))" }
}
Case 'close' $info ' before close after' "closed; 0x72 pid 42 ($image); before shot before.png; UI Automation closed the window; after shot after.png"
Case 'hidden by Close' $info ' before close after' 'closed;*UI Automation closed the window; after shot after.png'
Case 'sticky' $info ' before close after' 'the news window is still open after Close;*before shot before.png; UI Automation closed the window; after shot after.png'
Case 'other process' ($info -replace '^rekordbox', 'RB Stems Plus') '' 'the news window process name or title did not match'
Case 'other title' ($info -replace 'INFO', 'INFORMATION') '' 'the news window process name or title did not match'
Case 'other handle' ($info -replace '0x72', '0x73') '' 'the news window process name or title did not match'
Case 'wrong path' $info '' 'the window process image is outside the rekordbox install folders' (Join-Path $B 'rekordbox.exe')
Case 'updater image' $info '' 'the window process image is outside the rekordbox install folders' ($image -replace 'rekordbox\.exe$', 'Upmgr rekordbox.exe')
Case 'other UIA name' $info '' "the UI Automation window did not match (name 'rekordbox'*"
Case 'not JUCE' $info '' 'the UI Automation window did not match*'
Case 'no NOTIFICATION tab' $info '' 'expected one NOTIFICATION tab in the news window*'
Case 'INFORMATION elsewhere' $info '' 'expected one INFORMATION tab in the news window*'
Case 'no window pattern' $info '' 'the news window has no UI Automation window pattern*'
Case 'secure desktop' $info '' 'ordinary input desktop unavailable'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/info-test.ps1" "$here/kit/session-task.ps1" "$tmp/info" > "$tmp/i" 2>&1
  n="$(grep -c '^ok ' "$tmp/i")"; pass=$((pass + n))
  [ "$n" = 14 ] && ! grep -qv '^ok ' "$tmp/i" || { no "closeinfo ($n of 14 OK):"; grep -v '^ok ' "$tmp/i" | sed 's/^/    /'; }
  # The real stemsinit dispatch (rekordbox's STEMS engine progress window): read only, never an action.
  mkdir "$tmp/init"
  cat > "$tmp/init/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process([int]$Id) { @{ Id = $Id; ProcessName = 'rekordbox'; SessionId = $sid; Path = [RbspWin]::Image } }
EOF
  cat > "$tmp/init-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public static string Mode, Image;
  public static string[] Windows;
  public static bool Listed;
  static void Defined() { if (!Listed) throw new InvalidOperationException("Unable to find type [RbspWin]."); }
  public static string[] List() { Listed = true; return Windows; }
  public static int Pid(IntPtr h) { Defined(); return 42; }
  public static bool Checked;
  public static bool IsWindow(IntPtr h) { Defined(); return Mode != "gone" && !(Mode == "closed during the check" && Checked); }
  public static bool IsWindowVisible(IntPtr h) { Defined(); return true; }
}
namespace Windows.Automation {
  public enum TreeScope { Descendants }
  public enum ControlType { Button, TitleBar, Window, Pane }
  public class PropertyCondition {
    public object Value;
    public PropertyCondition(object property, object value) { Value = value; }
  }
  public class TreeWalker {
    public static TreeWalker ControlViewWalker = new TreeWalker();
    public AutomationElement GetParent(AutomationElement e) { return e.Parent; }
  }
  public class AutomationElement {
    public static object ControlTypeProperty = new object();
    public AutomationElement Current { get { return this; } }
    public AutomationElement Parent;
    public ControlType ControlType;
    public string Name, AutomationId, FrameworkId = "JUCE";
    public int ProcessId = 42, NativeWindowHandle;
    public static AutomationElement FromHandle(IntPtr h) {
      RbspWin.Checked = true;
      bool stale = RbspWin.Mode == "other UIA handle" || RbspWin.Mode == "closed during the check";
      return new AutomationElement { Name = "Initializing STEMS Engine", AutomationId = "Initializing STEMS Engine",
        NativeWindowHandle = stale ? 0 : (int)h.ToInt64(), ControlType = ControlType.Window };
    }
    public AutomationElement[] FindAll(TreeScope scope, PropertyCondition cond) {
      if (!Equals(cond.Value, ControlType.Button)) throw new InvalidOperationException("expected Button lookup");
      var caption = new AutomationElement { ControlType = ControlType.TitleBar, Parent = this };
      var captionClose = new AutomationElement { Name = "Close", Parent = caption, ControlType = ControlType.Button };
      var cancel = new AutomationElement { Name = "Cancel", Parent = this, ControlType = ControlType.Button };
      if (RbspWin.Mode == "caption buttons") return new[] { captionClose };
      if (RbspWin.Mode == "a Cancel button") return new[] { cancel, captionClose };
      return new AutomationElement[0];
    }
    public bool TryGetCurrentPattern(object pattern, out object p) { throw new InvalidOperationException("no pattern may be used"); }
  }
}
'@
$stub = @'
function Uia {}
function Shot { throw 'no screenshot expected' }
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:ProgramFiles = Join-Path $B 'Program Files'
$image = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordbox.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$init = 'rekordbox | Initializing STEMS Engine | 400x120 | 0x72'
function Case($mode, $row, $answer, [string]$path = $image) {
    [RbspWin]::Windows = @('rekordbox | rekordbox | 1936x1048 | 0x10', $row); [RbspWin]::Mode = $mode; [RbspWin]::Image = $path; [RbspWin]::Listed = $false; [RbspWin]::Checked = $false
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('stemsinit', '0x72', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ($out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -like "* stemsinit: $answer") { "ok $mode" } else { "bad ${mode}: $out" }
}
Case 'progress' $init "ok; 0x72 pid 42 ($image); UIA * 'Initializing STEMS Engine' id 'Initializing STEMS Engine' framework 'JUCE', no buttons"
Case 'caption buttons' $init 'ok; 0x72 pid 42 *no buttons'
Case 'a Cancel button' $init "the progress window has buttons: 'Cancel'"
Case 'other process' ($init -replace '^rekordbox', 'rekordboxAgent') 'the progress window process name or title did not match'
Case 'other title' ($init -replace 'Engine', 'Engine...') 'the progress window process name or title did not match'
Case 'other handle' ($init -replace '0x72', '0x73') 'the progress window process name or title did not match'
Case 'gone' 'rekordbox | rekordbox | 1936x1048 | 0x11' 'gone; 0x72'
Case 'wrong path' $init 'the window process image is outside the rekordbox install folders' (Join-Path $B 'rekordbox.exe')
Case 'other UIA handle' $init 'the UI Automation window did not match'
Case 'closed during the check' $init 'gone; 0x72'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/init-test.ps1" "$here/kit/session-task.ps1" "$tmp/init" > "$tmp/s" 2>&1
  n="$(grep -c '^ok ' "$tmp/s")"; pass=$((pass + n))
  [ "$n" = 10 ] && ! grep -qv '^ok ' "$tmp/s" || { no "stemsinit ($n of 10 OK):"; grep -v '^ok ' "$tmp/s" | sed 's/^/    /'; }
  # The real closeagentnews dispatch (rekordboxAgent's news window), the PC's Electron structure in place of Windows.
  mkdir "$tmp/agentnews"
  cat > "$tmp/agentnews/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process([int]$Id) { @{ Id = $Id; ProcessName = 'rekordboxAgent'; SessionId = $sid; Path = [RbspWin]::Image } }
EOF
  cat > "$tmp/agentnews-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public static string Mode, Trace, Image;
  public static string[] Windows;
  public static bool Open, Visible, Listed;
  static void Defined() { if (!Listed) throw new InvalidOperationException("Unable to find type [RbspWin]."); }
  public static string[] List() { Listed = true; return Windows; }
  public static int Pid(IntPtr h) { Defined(); return 42; }
  public static bool OrdinaryDesktop() { Defined(); return Mode != "secure desktop"; }
  public static IntPtr GetWindow(IntPtr h, uint relation) { Defined(); return Mode == "owned dialog" ? new IntPtr(16) : IntPtr.Zero; }
  public static bool IsWindow(IntPtr h) { return Open; }
  public static bool IsWindowVisible(IntPtr h) { return Visible; }
}
namespace Windows.Automation {
  public enum TreeScope { Descendants }
  public enum ControlType { Button, Window }
  public class PropertyCondition {
    public object Value;
    public PropertyCondition(object property, object value) { Value = value; }
  }
  public class AutomationElement {
    public static object ControlTypeProperty = new object();
    public AutomationElement Current { get { return this; } }
    public ControlType ControlType;
    public string Name, ClassName;
    public int ProcessId = 42, NativeWindowHandle;
    // as the PC's rekordboxAgent window: Electron, its page not exposed to UI Automation
    public static AutomationElement FromHandle(IntPtr h) {
      return new AutomationElement { Name = RbspWin.Mode == "other UIA name" ? "Sign in" : "rekordboxAgent",
        ClassName = RbspWin.Mode == "not Electron" ? "#32770" : "Chrome_WidgetWin_1",
        NativeWindowHandle = (int)h.ToInt64(), ControlType = ControlType.Window };
    }
    public AutomationElement[] FindAll(TreeScope scope, PropertyCondition cond) {
      if (!Equals(cond.Value, ControlType.Button)) throw new InvalidOperationException("expected Button lookup");
      if (RbspWin.Mode == "a page with buttons") return new[] { new AutomationElement { Name = "Log in", ControlType = ControlType.Button } };
      return new AutomationElement[0];
    }
    public bool TryGetCurrentPattern(object pattern, out object p) {
      p = new WindowPattern();
      return RbspWin.Mode != "no window pattern";
    }
  }
  public class WindowPattern {
    public static object Pattern = new object();
    public void Close() {
      RbspWin.Trace += " close";
      if (RbspWin.Mode == "hidden by Close") RbspWin.Visible = false;
      else if (RbspWin.Mode != "sticky") RbspWin.Open = false;
    }
  }
}
'@
$stub = @'
function Uia {}
function Screen-Dpi {}
function Shot {
    if ([RbspWin]::Trace -eq '') { [RbspWin]::Trace += ' before'; return 'shot before.png' }
    [RbspWin]::Trace += ' after'; return 'shot after.png'
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:ProgramFiles = Join-Path $B 'Program Files'
$image = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordboxAgent-win32-x64\rekordboxAgent.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$agent = 'rekordboxAgent | rekordboxAgent | 566x375 | 0x72'
function Case($mode, $row, $trace, $answer, [string]$path = $image) {
    [RbspWin]::Windows = @('rekordbox | rekordbox | 1936x1048 | 0x10', $row); [RbspWin]::Mode = $mode; [RbspWin]::Image = $path
    [RbspWin]::Trace = ''; [RbspWin]::Open = $true; [RbspWin]::Visible = $true; [RbspWin]::Listed = $false
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('closeagentnews', '0x72', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspWin]::Trace -eq $trace -and $out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -like "* closeagentnews: $answer") { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspWin]::Trace))" }
}
Case 'close' $agent ' before close after' "closed; 0x72 pid 42 ($image); before shot before.png; UI Automation closed the window; after shot after.png"
Case 'hidden by Close' $agent ' before close after' 'closed;*after shot after.png'
Case 'sticky' $agent ' before close after' 'the agent window is still open after Close;*UI Automation closed the window; after shot after.png'
Case 'other title' ($agent -replace '\| rekordboxAgent \| 566', '| Sign in | 566') '' 'the agent window process name or title did not match'
Case 'other process' ($agent -replace '^rekordboxAgent', 'rekordbox') '' 'the agent window process name or title did not match'
Case 'wrong path' $agent '' 'the window process image is outside the rekordbox install folders' (Join-Path $B 'rekordboxAgent.exe')
Case 'owned dialog' $agent '' 'the agent window has an owner window (a dialog)'
Case 'other UIA name' $agent '' "the UI Automation window did not match (name 'Sign in'*"
Case 'not Electron' $agent '' "the UI Automation window did not match*class '#32770')*"
Case 'a page with buttons' $agent '' "the agent window has buttons: 'Log in'*"
Case 'no window pattern' $agent '' 'the agent window has no UI Automation window pattern*'
Case 'secure desktop' $agent '' 'ordinary input desktop unavailable'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/agentnews-test.ps1" "$here/kit/session-task.ps1" "$tmp/agentnews" > "$tmp/an" 2>&1
  n="$(grep -c '^ok ' "$tmp/an")"; pass=$((pass + n))
  [ "$n" = 12 ] && ! grep -qv '^ok ' "$tmp/an" || { no "closeagentnews ($n of 12 OK):"; grep -v '^ok ' "$tmp/an" | sed 's/^/    /'; }
  # The real front dispatch: our app minimized through UIA, rekordbox's verified window focused and in front.
  mkdir "$tmp/front"
  cp "$here/kit/winlist.ps1" "$tmp/front/"
  cat > "$tmp/front/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-RbProcess($SessionId) { @{ Id = 42 } }
function Get-Process([int]$Id) {
    if ($Id -eq 43) { @{ Id = 43; ProcessName = [RbspWin]::AppName; SessionId = $sid; Path = 'C:\app\RB Stems Plus.exe' } }
    else { @{ Id = 42; ProcessName = 'rekordbox'; SessionId = $sid; Path = [RbspWin]::Image } }
}
EOF
  cat > "$tmp/front-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public static string Mode, Trace, Image, AppName;
  public static string[] Windows;
  public static bool Listed, AppIconic;
  public static IntPtr Foreground;
  static void Defined() { if (!Listed) throw new InvalidOperationException("Unable to find type [RbspWin]."); }
  public static string[] List() { Listed = true; return Windows; }
  public static int Pid(IntPtr h) { Defined(); return h.ToInt64() == 32 ? 43 : 42; }
  public static bool OrdinaryDesktop() { Defined(); return Mode != "secure desktop"; }
  public static IntPtr GetForegroundWindow() { Defined(); return Foreground; }
  public static bool IsIconic(IntPtr h) { Defined(); return h.ToInt64() == 32 && AppIconic; }
  public static IntPtr[] HandlesForProcess(int pid) { Defined(); return new[] { new IntPtr(16), new IntPtr(17) }; }
  public static bool IsWindowVisible(IntPtr h) { return true; }
  public static bool IsWindowEnabled(IntPtr h) { return true; }
  public static bool? IsCloaked(IntPtr h) { return false; }
  public static string WindowClass(IntPtr h) {
    return h == new IntPtr(21) ? "Static" : Mode == "crash behind app" && h == new IntPtr(17) ? "#32770" : "JUCE_abcd";
  }
  public static int GetDlgCtrlID(IntPtr h) { return -1; }
  public static int GetWindowLongW(IntPtr h, int index) { return 0; }
  public static string DialogControlText(IntPtr h, IntPtr child, int pid, int id) {
    return "Unexpected application error. Unable to read C:\\Private Folder\\My Song.flac.";
  }
  public static IntPtr[] DialogChildren(IntPtr h, int pid) { return new[] { new IntPtr(21) }; }
  public static string WindowCaption(IntPtr h) { return Mode == "analysis dialog" && h == new IntPtr(17) ? "" : "rekordbox"; }
  public static IntPtr GetWindow(IntPtr h, uint relation) { return IntPtr.Zero; }
  public struct RECT { public int L, T, R, B; }
  public static bool GetWindowRect(IntPtr h, out RECT r) { r = new RECT { L = 759, T = 335, R = 1161, B = 697 }; return true; }
}
namespace System.Windows.Forms {
  public class Area { public int Left, Top, Width, Height; }
  public static class SystemInformation { public static Area VirtualScreen = new Area { Width = 1920, Height = 1080 }; }
}
namespace Windows.Automation {
  public enum WindowVisualState { Normal, Maximized, Minimized }
  public class AutomationElement {
    public IntPtr Handle;
    public static AutomationElement FromHandle(IntPtr h) { return new AutomationElement { Handle = h }; }
    public bool TryGetCurrentPattern(object pattern, out object p) {
      p = new WindowPattern { Handle = Handle };
      return !(RbspWin.Mode == "no app pattern" && Handle.ToInt64() == 32);
    }
    public void SetFocus() {
      RbspWin.Trace += " focus 0x" + Handle.ToString("X");
      if (RbspWin.Mode != "focus refused") RbspWin.Foreground = Handle;
    }
  }
  public class WindowPattern {
    public static object Pattern = new object();
    public IntPtr Handle;
    public void SetWindowVisualState(WindowVisualState s) {
      RbspWin.Trace += " " + s.ToString().ToLower() + " 0x" + Handle.ToString("X");
      if (RbspWin.Mode != "app stays") RbspWin.AppIconic = true;
    }
  }
}
'@
$stub = @'
function Uia {}
function Screen-Dpi {}
function Shot { 'shot ' + (Join-Path $B 'analysis.png') }
function Read-ScreenText($png, $x, $y, $w, $h, [switch]$Plain) {
    if ($png -ceq (Join-Path $B 'analysis.png') -and "$x,$y,$w,$h" -eq '759,335,402,51' -and -not $Plain) { 'Analysis  Setting' }
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:ProgramFiles = Join-Path $B 'Program Files'
$image = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordbox.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$rb = 'rekordbox | rekordbox | 1936x1048 | 0x10'; $app = 'RB Stems Plus | RB Stems Plus | 1920x1032 | 0x20'
function Case($mode, [string[]]$rows, $trace, $answer, [string]$path = $image, [string]$appName = 'RB Stems Plus', [int]$fg = 32) {
    [RbspWin]::Windows = $rows; [RbspWin]::Mode = $mode; [RbspWin]::Image = $path; [RbspWin]::AppName = $appName
    [RbspWin]::Trace = ''; [RbspWin]::Listed = $false; [RbspWin]::AppIconic = $false; [RbspWin]::Foreground = [IntPtr]$fg
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('front', '', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspWin]::Trace -eq $trace -and $out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -like "* front: $answer") { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspWin]::Trace))" }
}
Case 'front' @($app, $rb) ' minimized 0x20 focus 0x10' "ok; our app's window(s) minimized: 1; rekordbox 0x10 in front"
Case 'already in front' @($rb) '' "ok; our app's window(s) minimized: 0; rekordbox 0x10 in front" -fg 16
Case 'focus refused' @($app, $rb) ' minimized 0x20 focus 0x10' "rekordbox's window 0x10 is not the foreground window"
Case 'app stays' @($app, $rb) ' minimized 0x20' "our app's window did not minimize"
Case 'no app pattern' @($app, $rb) '' "our app's window has no UI Automation window pattern"
Case 'app window of another process' @($app, $rb) '' "our app's window belongs to another process" -appName 'other'
Case 'two rekordbox windows' @($rb, 'rekordbox | rekordbox | 800x600 | 0x11') '' 'expected one rekordbox main window, found 2'
Case 'crash behind app' @($app, $rb, 'rekordbox | rekordbox | 400x200 | 0x11') '' 'rekordbox #32770 message box: 0x11: unrecognised text (76 chars, sha256:6bb931063ef4)'
Case 'no rekordbox' @($app) ' minimized 0x20' 'expected one rekordbox main window, found 0'
Case 'wrong path' @($rb) '' 'the rekordbox window process image is outside the rekordbox install folders' (Join-Path $B 'rekordbox.exe')
Case 'secure desktop' @($app, $rb) '' 'ordinary input desktop unavailable'
# OI-80: rekordbox's Analysis Setting dialog is named before anything is focused or minimized
Case 'analysis dialog' @($app, $rb) '' 'rekordbox Analysis Setting dialog: 0x11'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/front-test.ps1" "$here/kit/session-task.ps1" "$tmp/front" > "$tmp/fr" 2>&1
  n="$(grep -c '^ok ' "$tmp/fr")"; pass=$((pass + n))
  [ "$n" = 12 ] && ! grep -qv '^ok ' "$tmp/fr" || { no "front ($n of 12 OK):"; grep -v '^ok ' "$tmp/fr" | sed 's/^/    /'; }
  # OI-80: rekordbox's Analysis Setting dialog recognised read-only: a visible, unowned JUCE window
  # of rekordbox's with no caption whose title band reads exactly "Analysis Setting".
  cat > "$tmp/analysis-dialog-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
public static class RbspWin {
  public struct RECT { public int L, T, R, B; }
  public static int[] Handles = new int[0];
  public static IntPtr[] HandlesForProcess(int pid) { return Array.ConvertAll(Handles, n => new IntPtr(n)); }
  public static int Pid(IntPtr h) { return h.ToInt64() == 0x36 ? 999 : 42; }
  public static bool IsWindowVisible(IntPtr h) { return h.ToInt64() != 0x33; }
  public static bool IsIconic(IntPtr h) { return false; }
  public static bool? IsCloaked(IntPtr h) { return false; }
  public static string WindowClass(IntPtr h) { return h.ToInt64() == 0x35 ? "#32770" : "JUCE_1a203c46bbd"; }
  public static string WindowCaption(IntPtr h) { return h.ToInt64() == 0x31 ? "rekordbox" : ""; }
  public static IntPtr GetWindow(IntPtr h, uint relation) { return relation == 4 && h.ToInt64() == 0x34 ? new IntPtr(0x31) : IntPtr.Zero; }
  public static bool GetWindowRect(IntPtr h, out RECT r) {
    switch (h.ToInt64()) {
      case 0x31: r = new RECT { L = -8, T = -8, R = 1928, B = 1040 }; break;
      case 0x32: r = new RECT { L = 100, T = 100, R = 500, B = 500 }; break;
      case 0x37: r = new RECT { L = 10, T = 10, R = 150, B = 60 }; break;
      case 0x38: r = new RECT { L = 600, T = 100, R = 1000, B = 450 }; break;
      case 0x39: r = new RECT { L = 1000, T = 500, R = 1400, B = 850 }; break;
      case 0x3A: r = new RECT { L = -500, T = 10, R = -100, B = 400 }; break;
      default: r = new RECT { L = 759, T = 335, R = 1161, B = 697 }; break;
    }
    return true;
  }
}
namespace System.Windows.Forms {
  public class Area { public int Left, Top, Width, Height; }
  public static class SystemInformation { public static Area VirtualScreen = new Area { Width = 1920, Height = 1080 }; }
}
'@
$ast = [System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$null, [ref]$null)
foreach ($name in 'Get-RbAnalysisSettingDialogs', 'Assert-RbNoAnalysisSetting') {
    $fn = $ast.Find({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name }, $true)
    . ([scriptblock]::Create($fn.Extent.Text))
}
$sid = 5
$script:processes = @(@{ Id = 42 })
function Get-RbProcess($SessionId) { if ($SessionId -ne 5) { throw 'wrong session' }; $script:processes }
function Initialize-RbspWin {}
function Screen-Dpi { $script:dpi++ }
function Shot { $script:shots++; 'shot C:\shots\auto.png' }
function Read-ScreenText($png, $x, $y, $w, $h, [switch]$Plain) {
    $script:reads += "$x,$y,$w,$h,$Plain"
    switch ("$x,$y") {
        '759,335' { if (-not $Plain) { "Analysis`r`n Setting " } }
        '100,100' { 'Preferences' }
        '600,100' { if ($Plain) { 'Analysis Setting' } else { '' } }
        '1000,500' { 'analysis setting' }
    }
}
function T($name, [string[]]$got, [string]$want) { $g = $got -join ' # '; if ($g -ceq $want) { "ok $name" } else { "bad ${name}: '$g'" } }
$script:dpi = 0; $script:shots = 0; $script:reads = @()
[RbspWin]::Handles = @(0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A)
T 'only the verified dialogs' @(Get-RbAnalysisSettingDialogs) '0x30 # 0x38'
T 'one screenshot' "$script:shots $script:dpi" '1 1'
T 'title bands read' $script:reads '759,335,402,51,False # 100,100,400,56,False # 100,100,400,56,True # 600,100,400,49,False # 600,100,400,49,True # 1000,500,400,49,False # 1000,500,400,49,True'
T 'named STOP' @(try { Assert-RbNoAnalysisSetting; 'no throw' } catch { $_.Exception.Message }) 'rekordbox Analysis Setting dialog: 0x30, 0x38'
$script:dpi = 0; $script:shots = 0; $script:reads = @()
[RbspWin]::Handles = @(0x31, 0x33, 0x34, 0x35, 0x37)
T 'no candidate, no screenshot' "$(@(Get-RbAnalysisSettingDialogs).Count) $script:shots $script:dpi $($script:reads.Count)" '0 0 1 0'
T 'nothing to name' @(try { Assert-RbNoAnalysisSetting; 'no throw' } catch { $_.Exception.Message }) 'no throw'
$script:processes = @()
[RbspWin]::Handles = @(0x30)
T 'rekordbox not running' "$(@(Get-RbAnalysisSettingDialogs).Count) $script:shots" '0 0'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/analysis-dialog-test.ps1" "$here/kit/session-task.ps1" > "$tmp/adt" 2>&1
  n="$(grep -c '^ok ' "$tmp/adt")"; pass=$((pass + n))
  [ "$n" = 7 ] && ! grep -qv '^ok ' "$tmp/adt" || { no "Analysis Setting dialog ($n of 7 OK):"; grep -v '^ok ' "$tmp/adt" | sed 's/^/    /'; }
  # Drive the real format task and reply with scripted foreground changes and a virtual clock.
  mkdir "$tmp/format-focus"
  cat > "$tmp/format-focus/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process([int]$Id) {
    $name = switch ($Id) { 42 { 'rekordbox' }; 43 { 'explorer' }; 44 { 'browser' }; 45 { 'RB Stems Plus' } }
    $main = if ($Id -eq 45 -and [RbspWin]::Mode -ne 'companion secondary window') { [IntPtr]49 } else { [IntPtr]::Zero }
    $path = if ($Id -eq 43 -and [RbspWin]::Mode -ne 'unverified explorer image') { Join-Path $env:SystemRoot 'explorer.exe' } else { $image }
    @{ Id = $Id; ProcessName = $name; SessionId = $sid; Path = $path; MainWindowHandle = $main }
}
function Get-Date { $clock.Now }
function Start-Sleep([int]$Milliseconds) {
    $clock.Now = $clock.Now.AddMilliseconds($Milliseconds)
    if ($clock.Now -gt $clock.Cutoff) { throw 'formats: fixture exceeded virtual time limit' }
    if ([RbspWin]::Mode -in 'delayed', 'companion foreground', 'explorer foreground') {
        if ([RbspWin]::Foreground.ToInt64() -eq 0) { [RbspWin]::Foreground = [IntPtr]32 }
        elseif ([RbspFormatInput]::Target -ne [IntPtr]::Zero) { [RbspWin]::Foreground = [RbspFormatInput]::Target }
    }
    if ([RbspWin]::Mode -eq 'update during wait') { [RbspWin]::UpdateVisible = $true }
    if ([RbspWin]::Mode -eq 'tooltip clears') { [RbspWin]::ExtraWindows = @('rekordbox | menu | 300x100 | 0x19') }
}
EOF
  cat > "$tmp/wpf-rect.cs" << 'CS'
namespace Windows {
    public struct Point {
        public double X { get; set; }
        public double Y { get; set; }
        public Point(double x, double y) : this() { X = x; Y = y; }
    }

    public struct Rect {
        private double x, y, width, height;

        public Rect(double x, double y, double width, double height) {
            if (width < 0 || height < 0) throw new System.ArgumentException("Negative rectangle extent");
            this.x = x;
            this.y = y;
            this.width = width;
            this.height = height;
        }

        public static Rect Empty {
            get {
                return new Rect {
                    x = double.PositiveInfinity, y = double.PositiveInfinity,
                    width = double.NegativeInfinity, height = double.NegativeInfinity
                };
            }
        }

        public double Left { get { return x; } }
        public double Top { get { return y; } }
        public double Right { get { return IsEmpty ? double.NegativeInfinity : x + width; } }
        public double Bottom { get { return IsEmpty ? double.NegativeInfinity : y + height; } }
        public double Width { get { return width; } }
        public double Height { get { return height; } }
        public bool IsEmpty { get { return width < 0; } }

        public bool Contains(Point point) {
            return !IsEmpty && point.X >= Left && point.X <= Right &&
                point.Y >= Top && point.Y <= Bottom;
        }

        public bool Contains(Rect rect) {
            return !IsEmpty && !rect.IsEmpty && rect.Left >= Left && rect.Right <= Right &&
                rect.Top >= Top && rect.Bottom <= Bottom;
        }

        public override string ToString() {
            return IsEmpty ? "Empty" : string.Join(",", x, y, width, height);
        }
    }
}
CS
  cat > "$tmp/format-focus-test.ps1" << 'EOF'
param($source, $formatSource, $B)
$ErrorActionPreference = 'Stop'
$uiTypes = @'
using System;
public static class RbspWin {
    public static string Mode, Trace;
    public static bool Minimized, DialogVisible, UpdateVisible;
    public static IntPtr Foreground;
    public static string DialogTitle = "Import Playlist", DialogClass = "#32770";
    public static int DialogOwner = 16, HelperOwner;
    public static string[] ExtraWindows = new string[0];
    public static string[] List() { return List(false); }
    public static string[] List(bool allSizes) {
        var rows = new System.Collections.Generic.List<string> { "rekordbox | rekordbox | 1936x1048 | 0x10" };
        if (Foreground.ToInt64() == 32) rows.Add("explorer | Private shell title | 1920x40 | 0x20");
        if (Foreground.ToInt64() == 48) rows.Add("browser | Private browser title | 1000x700 | 0x30");
        if (Foreground.ToInt64() == 50) rows.Add("explorer | Private folder title | 800x600 | 0x32");
        if (DialogVisible && Mode != "picker omitted" && Mode != "picker takes focus during scan")
            rows.Add("rekordbox | " + DialogTitle + " | 800x600 | 0x11");
        if (Mode == "picker takes focus during scan") {
            DialogVisible = true;
            Foreground = new IntPtr(17);
        }
        if (UpdateVisible) rows.Add("Upmgr rekordbox | rekordbox update manager | 800x600 | 0x51");
        rows.AddRange(ExtraWindows);
        return rows.ToArray();
    }
    public static int Pid(IntPtr h) { return h == IntPtr.Zero ? 0 : h.ToInt64() == 32 || h.ToInt64() == 50 ? 43 : h.ToInt64() == 48 ? 44 : h.ToInt64() == 49 ? 45 : 42; }
    public static uint GetWindowThreadProcessId(IntPtr h, out uint pid) {
        pid = h == IntPtr.Zero ? 0u : (uint)Pid(h);
        return pid == 0 ? 0u : pid + 100;
    }
    public static bool OrdinaryDesktop() { return true; }
    public static IntPtr GetForegroundWindow() { return Foreground; }
    public static bool IsIconic(IntPtr h) { return h.ToInt64() == 16 && Minimized; }
    public static IntPtr GetWindow(IntPtr h, uint relation) {
        if (relation != 4) throw new InvalidOperationException("fixture expected GW_OWNER");
        if (h.ToInt64() == 17) return new IntPtr(DialogOwner);
        if (h.ToInt64() == 27) return new IntPtr(HelperOwner);
        if (h.ToInt64() == 24) return new IntPtr(48);
        return IntPtr.Zero;
    }
}
public static class RbspFormatInput {
    public static IntPtr Target;
    public static bool ShowWindowAsync(IntPtr h, int command) {
        RbspWin.Trace += " restore " + h.ToInt64() + "/" + command;
        RbspWin.Minimized = false;
        return true;
    }
    public static bool SetForegroundWindow(IntPtr h) {
        RbspWin.Trace += " front " + h.ToInt64();
        if (RbspWin.Mode == "foreground call fails") throw new InvalidOperationException("fixture failed in SetForegroundWindow");
        Target = h;
        return true;
    }
    public static uint GetCurrentThreadId() { return 101; }
    public static bool AttachThreadInput(uint thread, uint foregroundThread, bool attach) {
        RbspWin.Trace += (attach ? " attach " : " detach ") + thread + "/" + foregroundThread;
        return true;
    }
    public static bool BringWindowToTop(IntPtr h) { RbspWin.Trace += " top " + h.ToInt64(); return true; }
    public static bool IsWindowVisible(IntPtr h) { return true; }
    public static string WindowClass(IntPtr h) {
        long handle = h.ToInt64();
        if (handle == 17) return RbspWin.DialogClass;
        if (handle == 28) return "JUCE_1a1fe50d55e";
        if (handle == 49) return "HwndWrapper";
        if (handle == 50) return "CabinetWClass";
        return handle == 32 ? "Shell_TrayWnd" : (handle >= 18 && handle <= 27 && handle != 22) ? "JUCE_1a1fe50d55e" : "BrowserWidget";
    }
    public static string WindowTitle(IntPtr h) { return h.ToInt64() == 17 ? RbspWin.DialogTitle : h.ToInt64() == 49 ? "RB Stems Plus" : "Private browser title"; }
    public static string WindowSize(IntPtr h) { return h.ToInt64() == 27 ? "1x1" : "800x600"; }
    public static bool IsNonActivating(IntPtr h) { return h.ToInt64() != 26 && h.ToInt64() != 27; }
    public static bool ParkCursor(IntPtr h) { RbspWin.Trace += " park " + h.ToInt64(); return true; }
    public static int FileMenuPoint(IntPtr h, out int x, out int y) { x = 40; y = 30; return 1; }
    public static bool Click(IntPtr expected, IntPtr target, int x, int y, bool right, out IntPtr foreground) {
        foreground = RbspWin.Foreground;
        if (foreground != expected || target.ToInt64() != 16 || x != 40 || y != 30 || right) return false;
        RbspWin.Trace += " click " + target.ToInt64();
        return true;
    }
}
namespace Windows.Automation {
    public sealed class ControlType {
        public static readonly ControlType Window = new ControlType(), MenuItem = new ControlType(),
            Menu = new ControlType(), Pane = new ControlType(), Custom = new ControlType(),
            Button = new ControlType(), Text = new ControlType(), ProgressBar = new ControlType();
    }
    public static class Automation {
        public static bool Compare(AutomationElement left, AutomationElement right) {
            return left.NativeWindowHandle == right.NativeWindowHandle;
        }
    }
    public class AutomationElement {
        public AutomationElement Current { get { return this; } }
        public int ProcessId { get { return 42; } }
        public int NativeWindowHandle;
        public string Name { get { return NativeWindowHandle == 16 || NativeWindowHandle == 28 ? "rekordbox" : NativeWindowHandle == 17 ? RbspWin.DialogTitle : NativeWindowHandle == 25 ? "menu" :
            NativeWindowHandle == 29 ? (RbspWin.Mode == "unexpected progress button" ? "OK" : "Cancel") : ""; } }
        public string AutomationId { get { return NativeWindowHandle == 29 ? "Cancel.<empty>" : NativeWindowHandle == 31 ? ".<empty>" : ""; } }
        public string ClassName { get { return NativeWindowHandle <= 17 ? "#32770" : RbspFormatInput.WindowClass(new IntPtr(NativeWindowHandle)); } }
        public string FrameworkId { get { return NativeWindowHandle >= 18 ? "JUCE" : ""; } }
        public bool IsOffscreen { get { return NativeWindowHandle == 27 && RbspWin.Mode != "onscreen helper"; } }
        public Windows.Rect BoundingRectangle { get { return IsOffscreen ? Windows.Rect.Empty : NativeWindowHandle == 28 ? new Windows.Rect(0, 0, 350, 194) : new Windows.Rect(0, 0, 1, 1); } }
        public ControlType ControlType { get { return NativeWindowHandle <= 17 || NativeWindowHandle == 28 ? ControlType.Window : NativeWindowHandle == 25 ? ControlType.Menu : NativeWindowHandle == 27 ? ControlType.Custom :
            NativeWindowHandle == 29 ? ControlType.Button : NativeWindowHandle == 30 ? ControlType.Text : NativeWindowHandle == 31 ? ControlType.ProgressBar : ControlType.Pane; } }
        public static AutomationElement FromHandle(IntPtr h) { return new AutomationElement { NativeWindowHandle = h.ToInt32() }; }
    }
    public class TreeWalker {
        public static readonly TreeWalker ControlViewWalker = new TreeWalker();
        public AutomationElement GetFirstChild(AutomationElement el) {
            return el.NativeWindowHandle == 28 ? AutomationElement.FromHandle(new IntPtr(29)) : null;
        }
        public AutomationElement GetNextSibling(AutomationElement el) {
            return el.NativeWindowHandle == 29 || el.NativeWindowHandle == 30 ? AutomationElement.FromHandle(new IntPtr(el.NativeWindowHandle + 1)) : null;
        }
    }
}
'@
Add-Type -TypeDefinition ($uiTypes + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wpf-rect.cs')))
$stub = @'
function Uia {}
function Screen-Dpi {}
function Add-Type {}
function Close-RbUpdate {
    [RbspWin]::Trace += ' close update'
    [RbspWin]::UpdateVisible = $false
    'closeupdate: closed; fixture'
}
function Set-RbFront {
    if ([RbspWin]::Foreground.ToInt64() -notin 49, 50) { throw 'fixture expected the companion or Explorer window' }
    [RbspWin]::Trace += ' handoff'
    [RbspWin]::Foreground = [IntPtr]16
    'front: ok; our app minimized'
}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $source -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
Copy-Item -LiteralPath $formatSource -Destination (Join-Path $B 'format-ui.ps1')
Set-Content -LiteralPath (Join-Path $B 'format-lib.ps1') -Value '# fixture'
Add-Content -LiteralPath (Join-Path $B 'format-ui.ps1') -Value @'
function Find-FmtControls { throw 'formats: fixture reached the File menu' }
function Invoke-FmtImport {
    [RbspWin]::Trace += ' import'
    [RbspFormatInput]::Target = [IntPtr]::Zero
    [RbspWin]::Foreground = [IntPtr]32
    $null = Get-FmtWindows 'Main' -Refocus
    try { Open-FmtExploreMenu 'File' -NoShortcut }
    catch { if ($_.Exception.Message -cne 'formats: fixture reached the File menu') { throw } }
    [RbspWin]::Trace += ' menu'
    [RbspWin]::DialogVisible = $true
    if ((Wait-FmtWindow 'Open').Current.NativeWindowHandle -ne 17 -or [RbspWin]::Foreground -ne [IntPtr]17) {
        throw 'formats: fixture reached Open without foreground'
    }
    [RbspWin]::Trace += ' dialog'
}
'@
$env:ProgramFiles = Join-Path $B 'Program Files'
$env:SystemRoot = Join-Path $B 'Windows'
$image = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordbox.exe"
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
function Case($mode, [int]$foreground, $trace, $answer, [double]$seconds, [switch]$minimized) {
    [RbspWin]::Mode = $mode; [RbspWin]::Trace = ''; [RbspWin]::Foreground = [IntPtr]$foreground
    [RbspWin]::Minimized = $minimized; [RbspWin]::DialogVisible = [RbspWin]::UpdateVisible = $false
    [RbspFormatInput]::Target = [IntPtr]::Zero
    $start = [datetime]'2026-11-21T11:52:40'
    $clock = @{ Now = $start; Cutoff = $start.AddSeconds(11) }
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('fmtimport', '', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspWin]::Trace -ceq $trace -and ($clock.Now - $start).TotalSeconds -eq $seconds -and
        $out.Count -eq 2 -and $out[0] -eq 'fixture' -and $out[1] -clike "* fmtimport: $answer" -and
        $out[1] -cnotlike '*Private * title*') { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspWin]::Trace), elapsed:$(($clock.Now - $start).TotalSeconds)s)" }
}
Case 'delayed' 0 ' restore 16/9 attach 101/143 front 16 top 16 detach 101/143 park 16 import attach 101/143 front 16 top 16 detach 101/143 menu front 17 dialog' 'OK' 0.4 -minimized
Case 'companion foreground' 49 ' handoff park 16 import attach 101/143 front 16 top 16 detach 101/143 menu front 17 dialog' 'OK' 0.3
Case 'companion secondary window' 49 '' 'STOP: formats: focus left the verified rekordbox windows* foreground pid=45 class=HwndWrapper size=800x600 handle=0x31 owner-class=none owner-pid=0 (line *)' 0
Case 'explorer foreground' 50 ' handoff park 16 import attach 101/143 front 16 top 16 detach 101/143 menu front 17 dialog' 'OK' 0.3
Case 'unverified explorer image' 50 '' 'STOP: formats: focus left the verified rekordbox windows* foreground pid=43 class=CabinetWClass size=800x600 handle=0x32 owner-class=none owner-pid=0 (line *)' 0
Case 'persistent other window' 48 '' 'STOP: formats: focus left the verified rekordbox windows* foreground pid=44 class=BrowserWidget size=800x600 handle=0x30 owner-class=none owner-pid=0 (line *)' 0
Case 'persistent taskbar' 32 (' attach 101/143 front 16 top 16 detach 101/143' * 100) 'STOP: formats: timed out waiting for verified rekordbox foreground* foreground pid=43 class=Shell_TrayWnd size=800x600 handle=0x20 owner-class=none owner-pid=0 (line *)' 10
Case 'update during wait' 32 ' attach 101/143 front 16 top 16 detach 101/143' 'STOP: formats: unexpected update, security or application window* window pid=42 class=BrowserWidget size=800x600 handle=0x51 owner-class=none owner-pid=0 (line *)' 0.1
Case 'no foreground' 0 (' front 16 top 16' * 100) 'STOP: formats: timed out waiting for verified rekordbox foreground* no window handle (line *)' 10
Case 'foreground call fails' 32 ' attach 101/143 front 16 detach 101/143' 'STOP: formats: Windows automation failed (line *)' 0

. (Join-Path $B 'kit-lib.ps1')
. $formatSource
function Get-DesktopWindows([switch]$AllSizes) { [RbspWin]::List($AllSizes) }
$fmtPid = 42; $fmtHandle = [IntPtr]16
$fmtStop = Join-Path $B 'midi-stop'
$clock = @{ Now = [datetime]'2026-11-21T11:52:40'; Cutoff = [datetime]'2026-11-21T11:52:51' }
$fmtDeadline = $clock.Cutoff
[RbspWin]::Foreground = $fmtHandle
[RbspWin]::Mode = ''; [RbspWin]::Trace = ''
$tip = 'rekordbox |  | 123x38 | 0x12'
$shadows = 'rekordbox |  | 123x14 | 0x13', 'rekordbox |  | 123x14 | 0x14',
    'rekordbox |  | 14x52 | 0x15', 'rekordbox |  | 11x500 | 0x17'
[RbspWin]::ExtraWindows = @($tip) + $shadows
try {
    $state = Get-FmtWindowState 'Main'
    if ($state.Tooltips -eq 5 -and $state.Handles.Count -eq 1 -and -not $state.Dialogs.Count) { 'ok tooltip and shadows ignored' }
    else { 'bad tooltip and shadows ignored' }
} catch { "bad tooltip and shadows ignored: $_" }

[RbspWin]::Mode = 'tooltip clears'
$start = $clock.Now
[RbspWin]::ExtraWindows += @('rekordbox | menu | 300x100 | 0x19')
try {
    $menus = @(Get-FmtWindows 'Menu')
    if ($menus.Count -eq 1 -and $menus[0].Current.NativeWindowHandle -eq 25 -and
        ($clock.Now - $start).TotalMilliseconds -eq 100) { 'ok wait for tooltips before reading menus' }
    else { 'bad wait for tooltips before reading menus' }
} catch { "bad wait for tooltips before reading menus: $_" }

function Blocked([string]$name, [string]$window) {
    [RbspWin]::ExtraWindows = @($window)
    try { $null = Get-FmtWindowState 'Main'; "bad $name" }
    catch {
        if ($_.Exception.Message -cmatch '^formats: unexpected rekordbox window owner; window pid=42 class=\S+ size=800x600 handle=0x[0-9A-F]+ owner-class=\S+ owner-pid=\d+$') {
            "ok $name"
        } else { "bad ${name}: $_" }
    }
}
Blocked 'small non-JUCE window stops' 'rekordbox |  | 123x38 | 0x16'
Blocked 'large JUCE window stops' 'rekordbox |  | 401x121 | 0x12'
Blocked 'foreign-owned JUCE window stops' 'rekordbox |  | 123x38 | 0x18'
Blocked 'activating JUCE window stops' 'rekordbox |  | 123x38 | 0x1A'
[RbspWin]::ExtraWindows = @()
function PickerCase($name, $phase, [int]$owner, [int]$helperOwner, $title, $class, $expected) {
    [RbspWin]::DialogVisible = $true
    [RbspWin]::DialogOwner = $owner
    [RbspWin]::HelperOwner = $helperOwner
    [RbspWin]::DialogTitle = $title
    [RbspWin]::DialogClass = $class
    try {
        $state = Get-FmtWindowState $phase
        $result = if ($state.Dialogs.Count -eq 1 -and $state.Dialogs[0].Current.NativeWindowHandle -eq 17) { 'accepted' }
    } catch { $result = $_.Exception.Message }
    finally { [RbspWin]::DialogVisible = $false }
    if ($result -clike $expected -and $result -cnotmatch 'Unexpected title|Private browser title') {
        "ok $name"
    } else { "bad ${name}: $result" }
}
$badOwner = 'formats: unexpected rekordbox window owner; window pid=42 class=#32770 size=800x600 handle=0x11 owner-class=* owner-pid=* caption=Import Playlist'
$badWindow = 'formats: unexpected rekordbox window; window pid=42 class=* size=800x600 handle=0x11 owner-class=* owner-pid=*'
PickerCase 'picker owned by an unowned JUCE helper' ExploreOpen 27 0 'Import Playlist' '#32770' accepted
PickerCase 'picker chain ending at the main window' Open 27 16 'Import Playlist' '#32770' accepted
PickerCase 'export picker owned by a JUCE helper during close' ClosePicker 27 0 'Export a playlist to a file for music apps (*.m3u8)' '#32770' accepted
PickerCase 'unowned picker stops' Open 0 0 'Import Playlist' '#32770' $badOwner
PickerCase 'foreign owner farther up the picker chain stops' Open 27 48 'Import Playlist' '#32770' $badOwner
PickerCase 'cyclic picker owner chain stops' Open 27 17 'Import Playlist' '#32770' $badOwner
PickerCase 'allowlisted title with a non-picker class stops' ExploreOpen 16 0 'Import Playlist' 'JUCE_1' $badWindow
PickerCase 'non-allowlisted title with the picker class stops' ExploreOpen 16 0 'Unexpected title' '#32770' $badWindow
PickerCase 'picker stops outside its allowed phase' Main 16 0 'Save As' '#32770' $badWindow
PickerCase 'Import Track is not a playlist submission target' Open 16 0 'Import Track' '#32770' $badWindow
[RbspWin]::ExtraWindows = @('rekordbox | Private helper title | 1x1 | 0x1B')
PickerCase 'offscreen JUCE helper appears alongside its picker' ExploreOpen 27 0 'Import Playlist' '#32770' accepted
[RbspWin]::Mode = 'onscreen helper'
PickerCase 'visible nonempty JUCE helper stops' ExploreOpen 27 0 'Import Playlist' '#32770' 'formats: unexpected rekordbox window owner; window pid=42 class=JUCE_1a1fe50d55e size=1x1 handle=0x1B owner-class=none owner-pid=0'
[RbspWin]::ExtraWindows = @('rekordbox | Private helper title | 0x1 | 0x1B')
PickerCase 'zero-width JUCE helper is ignored' ExploreOpen 27 0 'Import Playlist' '#32770' accepted
[RbspWin]::ExtraWindows = @()
function PickerForegroundCase($name, $phase, $mode, [int]$foreground, [int]$owner, $expected) {
    [RbspWin]::Mode = $mode
    [RbspWin]::Foreground = [IntPtr]$foreground
    [RbspWin]::DialogOwner = $owner
    [RbspWin]::DialogVisible = $true
    $start = $clock.Now
    try {
        $dialog = Get-FmtWindows $phase -Dialog
        $result = if ($dialog.Current.NativeWindowHandle -eq 17) { 'accepted' }
    } catch { $result = $_.Exception.Message }
    finally { $clock.Now = $start }
    if ($result -clike $expected) { "ok $name" } else { "bad ${name}: $result" }
}
PickerForegroundCase 'verified foreground picker missing from enumeration' Open 'picker omitted' 17 27 accepted
PickerForegroundCase 'verified picker takes focus during enumeration' Open 'picker takes focus during scan' 16 27 accepted
PickerForegroundCase 'omitted picker with foreign owner is rejected' Open 'picker omitted' 17 48 'formats: focus left*'
PickerForegroundCase 'omitted picker is rejected in the main phase' Main 'picker omitted' 17 27 'formats: focus left*'

function Invoke-FmtPickerButton($dialog, $phase, $caption, $id) {
    Assert-FmtPicker $dialog $phase
    if ($phase -cne 'ClosePicker' -or $caption -cne 'Cancel' -or $id -cne '2') { throw 'fixture expected native Cancel' }
    [RbspWin]::Trace += ' cancel 17'
    [RbspWin]::DialogVisible = $false
    [RbspWin]::Foreground = $fmtHandle
}
[RbspWin]::Mode = ''
[RbspWin]::Trace = ''
[RbspWin]::Foreground = [IntPtr]17
[RbspWin]::DialogTitle = 'Save As'
try {
    if ((Cancel-FmtPickerForClose) -ceq 'cancelled' -and [RbspWin]::Trace -ceq ' cancel 17' -and
        (Cancel-FmtPickerForClose) -ceq 'no picker') { 'ok cancel a verified Save As before close' }
    else { 'bad cancel a verified Save As before close' }
} catch { "bad cancel a verified Save As before close: $_" }
[RbspWin]::ExtraWindows = @('rekordbox | rekordbox | 350x194 | 0x1C')
try {
    $state = Get-FmtWindowState 'ImportResult'
    if ($state.ImportProgress.Count -eq 1 -and $state.Dialogs.Count -eq 0 -and $state.Handles.Count -eq 1) { 'ok import progress only allows waiting' }
    else { 'bad import progress only allows waiting' }
} catch { "bad import progress only allows waiting: $_" }
Blocked 'import progress outside import result stops' 'rekordbox | rekordbox | 350x194 | 0x1C'
[RbspWin]::Mode = 'unexpected progress button'
try { $null = Get-FmtWindowState 'ImportResult'; 'bad unknown import progress button' }
catch {
    if ($_.Exception.Message -clike 'formats: unexpected rekordbox window owner*') { 'ok unknown import progress button' }
    else { "bad unknown import progress button: $_" }
}
[RbspWin]::Mode = ''
[RbspWin]::ExtraWindows = @()
[RbspWin]::Trace = ''
try {
    if ((Open-FmtFileMenuBar) -and [RbspWin]::Trace -ceq ' click 16 park 16') { 'ok park cursor after native File click' }
    else { "bad park cursor after native File click: $([RbspWin]::Trace)" }
} catch { "bad park cursor after native File click: $_" }
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/format-focus-test.ps1" "$here/kit/session-task.ps1" "$here/kit/format-ui.ps1" "$tmp/format-focus" > "$tmp/ff" 2>&1
  n="$(grep -c '^ok ' "$tmp/ff")"; pass=$((pass + n))
  [ "$n" = 38 ] && ! grep -qv '^ok ' "$tmp/ff" || { no "format focus ($n of 38 OK):"; grep -v '^ok ' "$tmp/ff" | sed 's/^/    /'; }
  # Exercise the real format lookup, diagnostics and dump against a small UIA model.
  cat > "$tmp/format-uia-test.ps1" << 'EOF'
param($KitSource, $Work)
$ErrorActionPreference = 'Stop'
$uiTypes = @'
namespace Windows.Automation {
    public class ControlType {
        public string ProgrammaticName;
        public ControlType(string type) { ProgrammaticName = "ControlType." + type; }
        public static ControlType Button = new ControlType("Button"), RadioButton = new ControlType("RadioButton"),
            TabItem = new ControlType("TabItem"), ListItem = new ControlType("ListItem"), Custom = new ControlType("Custom"),
            TreeItem = new ControlType("TreeItem"), Edit = new ControlType("Edit"), Text = new ControlType("Text"),
            ScrollBar = new ControlType("ScrollBar"), Group = new ControlType("Group"),
            Menu = new ControlType("Menu"), MenuItem = new ControlType("MenuItem"), Tree = new ControlType("Tree"),
            DataItem = new ControlType("DataItem"), Pane = new ControlType("Pane"), Window = new ControlType("Window"),
            Header = new ControlType("Header");
    }
    public static class ExpandCollapsePattern { public static object Pattern = new object(); }
    public enum TreeScope { Descendants }
    public class PropertyCondition {
        public ControlType Type;
        public PropertyCondition(object property, ControlType type) { Type = type; }
    }
    public static class AutomationElement {
        public static object Main, Popup, Hidden, Hit, ControlTypeProperty;
        public static int PointReads;
        public static object FromPoint(Windows.Point point) {
            PointReads++;
            if (point.X != 30 || point.Y != 30) throw new System.InvalidOperationException("fixture: unexpected hit-test point");
            return Hit;
        }
        public static object FromHandle(System.IntPtr handle) {
            switch (handle.ToInt64()) {
                case 16: return Main;
                case 17: return Popup;
                case 19: return Hidden;
                default: throw new System.InvalidOperationException("fixture UIA unavailable");
            }
        }
    }
    /* as in .NET: the comparison lives on Automation, not on AutomationElement */
    public static class Automation {
        public static bool Compare(object x, object y) { return object.ReferenceEquals(x, y); }
    }
    public static class TreeWalker { public static object ControlViewWalker, RawViewWalker; }
}
public static class RbspWin {
    public static string CloseTrace;
    public static System.IntPtr GetForegroundWindow() { return new System.IntPtr(16); }
    public static bool OrdinaryDesktop() { return true; }
    public static string WindowClass(System.IntPtr handle) { return "JUCE_abcd"; }
    public static int Pid(System.IntPtr handle) { return handle.ToInt64() == 18 ? 99 : 42; }
    public static System.Collections.Generic.List<System.IntPtr> HandlesForProcess(int pid) {
        return new System.Collections.Generic.List<System.IntPtr> { new System.IntPtr(16), new System.IntPtr(17),
            new System.IntPtr(18), new System.IntPtr(19), new System.IntPtr(20) };
    }
    public static bool PostMessageW(System.IntPtr h, uint message, System.IntPtr wParam, System.IntPtr lParam) {
        CloseTrace += " key " + h.ToInt64() + "/" + message.ToString("X") + "/" + wParam.ToInt64().ToString("X") +
                      "/" + unchecked((uint)lParam.ToInt64()).ToString("X");
        return true;
    }
}
public sealed class FixtureRbProcess {
    public System.IntPtr MainWindowHandle = new System.IntPtr(16);
    public int Id = 42, SessionId;
    public string ProcessName = "rekordbox", Path;
    public bool CloseMainWindow() { RbspWin.CloseTrace += " close"; return true; }
}
'@
Add-Type -TypeDefinition ($uiTypes + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wpf-rect.cs')))
$ct = [Windows.Automation.ControlType]
function Element([string]$type, [string]$name, [string]$id, $parent, [int]$y = 0) {
    $props = [pscustomobject]@{
        ControlType = $ct::$type; Name = $name; AutomationId = $id; ProcessId = 42
        NativeWindowHandle = 16; ClassName = 'Fixture'; FrameworkId = 'JUCE'; AcceleratorKey = ''
        IsEnabled = $true; IsOffscreen = $false; BoundingRectangle = [Windows.Rect]::new(10, $y, 32, 32)
    }
    $el = [pscustomobject]@{
        Current = $props; Parent = $parent; Children = [Collections.Generic.List[object]]::new(); Next = $null
    }
    $el | Add-Member ScriptMethod FindAll {
        param($scope, $condition)
        foreach ($child in $this.Children) {
            if ($child.Current.ControlType.ProgrammaticName -ceq $condition.Type.ProgrammaticName) { $child }
            $child.FindAll($scope, $condition)
        }
    }
    $el | Add-Member ScriptMethod TryGetCurrentPattern { return $false }
    if ($parent) {
        if ($parent.Children.Count) { $parent.Children[-1].Next = $el }
        $parent.Children.Add($el)
    }
    return $el
}
$walker = [pscustomobject]@{}
$walker | Add-Member ScriptMethod GetParent { param($el) $el.Parent }
$walker | Add-Member ScriptMethod GetFirstChild { param($el) if ($el.Children.Count) { $el.Children[0] } }
$walker | Add-Member ScriptMethod GetNextSibling { param($el) $el.Next }
[Windows.Automation.TreeWalker]::ControlViewWalker = $walker
[Windows.Automation.TreeWalker]::RawViewWalker = $walker
$fmtPid = 42
$fmtMain = Element Window 'rekordbox' 'rb-main'
$fmtMain.Current.BoundingRectangle = [Windows.Rect]::new(0, 0, 1200, 800)
[Windows.Automation.AutomationElement]::Main = $fmtMain

[void](New-Item -ItemType Directory -Path $Work)
Set-Content -LiteralPath (Join-Path $Work 'kit-lib.ps1') -Value '$Kit = $B = $PSScriptRoot'
foreach ($name in 'format-lib.ps1', 'format-ui.ps1', 'winlist.ps1') {
    Copy-Item -LiteralPath (Join-Path $KitSource $name) -Destination $Work
}
$env:ProgramFiles = Join-Path $Work 'Program Files'
$rbProcess = [FixtureRbProcess]::new()
$rbProcess.Path = "$(Join-Path $env:ProgramFiles 'rekordbox')\rekordbox 7.2.19\rekordbox.exe"
$stub = @'
function Uia {}
function Get-DesktopWindows { 'rekordbox | rekordbox | 1200x800 | 0x10' }
function Get-RbProcess([Nullable[int]]$SessionId) { $rbProcess.SessionId = $SessionId; $rbProcess }
function Get-Process([int]$Id) { if ($Id -eq $rbProcess.Id) { $rbProcess.SessionId = $sid; $rbProcess } }
try {
'@
$task = Join-Path $Work 'session-task.ps1'
(Get-Content -LiteralPath (Join-Path $KitSource 'session-task.ps1') -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$env:SystemDrive = [IO.Path]::GetPathRoot($Work).TrimEnd([IO.Path]::DirectorySeparatorChar)
. (Join-Path $KitSource 'format-lib.ps1')
. (Join-Path $KitSource 'format-ui.ps1')
$clock = @{ Now = [datetime]'2026-11-21T11:52:40' }
function Get-Date { $clock.Now }
function Start-Sleep([int]$Milliseconds) { $clock.Now = $clock.Now.AddMilliseconds($Milliseconds) }
function Get-FmtWindows {}
function Check($name, [scriptblock]$test) {
    try { if (& $test) { "ok $name" } else { "bad $name" } }
    catch { "bad ${name}: $($_.Exception.Message)" }
}

Check 'format clicks check WPF bounds and accept only related hit-test controls' {
    $fmtHandle = [IntPtr]16
    $parent = Element Pane '' 'click-parent'
    $control = Element Button '' 'click-target' $parent
    $control.Current.BoundingRectangle = [Windows.Rect]::new(10.5, 20.25, 19.5, 9.75)
    $child = Element Text '' 'click-child' $control
    $sibling = Element Button '' 'click-sibling' $parent
    $point = [Windows.Point]::new(30, 30)
    $zero = [Windows.Rect]::new(30, 30, 0, 0)
    if ($zero.IsEmpty -or -not $zero.Contains($point) -or -not [Windows.Rect]::Empty.IsEmpty -or
        -not $control.Current.BoundingRectangle.Contains($zero)) { return $false }
    $clicks = [Collections.Generic.List[string]]::new()
    function Invoke-FmtMouseClick([IntPtr]$expected, [IntPtr]$target, [int]$x, [int]$y, [bool]$right, [ref]$foreground) {
        $clicks.Add("$expected/$target/$x/$y/$right")
        return $true
    }
    foreach ($el in $child, $parent) {
        [Windows.Automation.AutomationElement]::Hit = $el
        Invoke-FmtClick $control -Point $point
    }
    [Windows.Automation.AutomationElement]::Hit = $sibling
    try { Invoke-FmtClick $control -Point $point; return $false }
    catch { if ($_.Exception.Message -cne 'formats: the rekordbox control is obscured') { throw } }
    try { Invoke-FmtClick $control -Point ([Windows.Point]::new(31, 30)); return $false }
    catch { if ($_.Exception.Message -cne 'formats: the click is outside the verified control') { throw } }
    [Windows.Automation.AutomationElement]::PointReads -eq 3 -and
        ($clicks -join ',') -ceq '16/16/30/30/False,16/16/30/30/False'
}

Check 'JUCE File popup and its edge shadows are distinguished from other windows' {
    $popup = Element Window 'rekordbox' ''
    $popup.Current.ClassName = 'JUCE_12ab'
    $popup.Current.BoundingRectangle = [Windows.Rect]::new(1, 45, 228, 270)
    $import = Element MenuItem 'Import' 'import' $popup
    $import.Current.BoundingRectangle = [Windows.Rect]::new(1, 47, 228, 25)
    $shadow = Element Pane '' ''
    $shadow.Current.ClassName = $popup.Current.ClassName
    $shadow.Current.BoundingRectangle = [Windows.Rect]::new(229, 31, 14, 298)
    if (-not (Test-FmtJucePopupRoot $popup.Current) -or -not (Test-FmtJuceMenu $popup) -or
        -not (Test-FmtMenuShadow $shadow.Current @($popup))) { return $false }
    $shadow.Current.BoundingRectangle = [Windows.Rect]::new(230, 31, 14, 298)
    $null = Element Button 'Cancel' 'cancel' $popup
    -not (Test-FmtJuceMenu $popup) -and -not (Test-FmtMenuShadow $shadow.Current @($popup))
}

Check 'a JUCE menu parent must adjoin an identified submenu' {
    $menu = Element Window 'rekordbox' ''
    $submenu = Element Window 'rekordbox' ''
    $menu.Current.BoundingRectangle = [Windows.Rect]::new(1, 45, 228, 270)
    $submenu.Current.BoundingRectangle = [Windows.Rect]::new(229, 45, 137, 79)
    if (-not (Test-FmtMenuParent $menu @($submenu))) { return $false }
    $submenu.Current.BoundingRectangle = [Windows.Rect]::new(230, 45, 137, 79)
    -not (Test-FmtMenuParent $menu @($submenu))
}

$empty = '.<empty>'
$toggle = Element Button 'ShowHideTreeShortcutButton' ('ShowHideTreeShortcutButton' + $empty * 5) $fmtMain
$toggle.Current.BoundingRectangle = [Windows.Rect]::new(6, 200, 18, 20)
$column = Element Custom '' ($empty * 8) $fmtMain
$column.Current.BoundingRectangle = [Windows.Rect]::new(2, 222, 33, 344)
# Control View flattens the buttons and column into siblings, as in the dump.
$icons = @(foreach ($i in 16..0) {
    $icon = Element Button '' ($empty * 9) $fmtMain
    $icon.Current.BoundingRectangle = [Windows.Rect]::new(7, (225 + $i * 20), 18, 18)
    $icon
})
$header = Element Text 'Devices' 'source-heading' $fmtMain
$header.Current.BoundingRectangle = [Windows.Rect]::new(37, 222, 257, 12)
$pane = Element Group '' ($empty * 6) $fmtMain
$pane.Current.BoundingRectangle = [Windows.Rect]::new(37, 222, 222, 518)
$playlistSearch = Element Button 'SearchPlaylistButton' ('SearchPlaylistButton' + $empty * 6) $fmtMain
$playlistSearch.Current.BoundingRectangle = [Windows.Rect]::new(37, 222, 24, 23)
$allTracksRow = Element Custom 'All Tracks' ($empty * 11) $fmtMain
$allTracksRow.Current.BoundingRectangle = [Windows.Rect]::new(37, 278, 210, 15)
$playlistsRow = Element Custom 'Playlists' ($empty * 11) $fmtMain
$playlistsRow.Current.BoundingRectangle = [Windows.Rect]::new(37, 293, 210, 15)
$testsRow = Element Custom 'RBSP Tests (36)' ($empty * 11) $fmtMain
$testsRow.Current.BoundingRectangle = [Windows.Rect]::new(37, 308, 210, 15)
$formatRow = Element Custom 'RBSP Formats' ($empty * 11) $fmtMain
$formatRow.Current.BoundingRectangle = [Windows.Rect]::new(37, 323, 210, 15)
$privateRow = Element Custom 'PRIVATE_CUSTOM (36)' 'row.PRIVATE_CUSTOM (36)' $fmtMain
$privateRow.Current.BoundingRectangle = [Windows.Rect]::new(37, 338, 210, 15)
function Set-SourceFixture([string]$name) {
    $isPlaylists = $name -ceq 'Playlists'
    $header.Current.Name = $name
    $header.Current.IsOffscreen = $isPlaylists
    foreach ($el in $script:pane, $playlistSearch, $allTracksRow, $playlistsRow, $testsRow, $formatRow, $privateRow) { $el.Current.IsOffscreen = -not $isPlaylists }
}
Set-SourceFixture 'Devices'
$trace = [Collections.Generic.List[int]]::new()
$sourceFixture = @{ Verified = $true }
function Invoke-FmtClick($el, [switch]$Right) {
    if ($Right) { throw 'fixture: unexpected right click' }
    $y = $el.Current.BoundingRectangle.Top
    $trace.Add($y)
    switch ($y) {
        225 { Set-SourceFixture 'Collection' }
        245 { if ($sourceFixture.Verified) { Set-SourceFixture 'Playlists' } }
        default { throw 'fixture: unexpected click' }
    }
}

$collectionTree = Element Tree '' 'collection-tree' $fmtMain
$collectionRoot = Element TreeItem 'Collection' 'collection-root' $collectionTree
$all = Element TreeItem 'All' 'collection-all' $collectionRoot
$private = Element TreeItem 'PRIVATE_PLAYLIST' 'folder.PRIVATE_PLAYLIST' $all
$deep = Element TreeItem 'Genre' 'deep-genre' $all
$playlistTree = Element Tree '' 'playlist-tree' $fmtMain
$tests = Element TreeItem 'RBSP Tests' 'test-list' $playlistTree
$formats = Element TreeItem 'RBSP Formats' 'format-list' $playlistTree
$row = Element DataItem 'PRIVATE_ROW' 'row.PRIVATE_ROW' $fmtMain
$cell = Element Text 'PRIVATE_CELL' 'cell.PRIVATE_CELL.PRIVATE_ROW' $row
$anonymous = Element Text '' 'anonymous.PRIVATE_ROW' $row
$foreign = Element Button 'PRIVATE_FOREIGN' 'foreign' $fmtMain
$foreign.Current.ProcessId = 99

Check 'dump redacts UI names and name-derived metadata' {
    Set-Content -LiteralPath (Join-Path $Work 'task-cmd.txt') -Value @('fmtuidump', '', 'fixture')
    & $task
    $reply = @(Get-Content -LiteralPath (Join-Path $Work 'task-out.txt'))
    if ($reply[0] -cne 'fixture' -or $reply[1] -cnotmatch ' fmtuidump: OK (\d+) (format-uidump-[a-f0-9]{32}\.jsonl)$') { throw ($reply -join ' ') }
    $count = [int]$Matches[1]
    $dump = @(Get-Content -LiteralPath (Join-Path $Work $Matches[2]))
    $records = @($dump | ConvertFrom-Json)
    $names = @($records | ForEach-Object { $_.Name })
    $data = @($records | Where-Object { $_.ControlType -ceq 'ControlType.DataItem' })
    $count -eq $dump.Count -and -not ($dump -cmatch 'PRIVATE_|"Name":"Genre"') -and
        $names -ccontains 'Collection' -and $names -ccontains 'All' -and
        $names -ccontains 'RBSP Tests' -and $names -ccontains 'RBSP Formats' -and
        $data.Count -eq 1 -and $data[0].Name -ceq '<redacted>' -and $data[0].AutomationId -ceq '<redacted>' -and
        $data[0].Depth -eq 1 -and $data[0].FrameworkId -ceq 'JUCE' -and $data[0].ClassName -ceq 'Fixture' -and
        $data[0].IsEnabled -and -not $data[0].IsOffscreen -and $data[0].BoundingRectangle
}

Check 'main scope redacts names and name-derived IDs on UI chrome' {
    $main = Element Window 'rekordbox' 'chrome'
    foreach ($type in 'Button', 'Pane', 'Window', 'Header', 'MenuItem') {
        $null = Element $type "PRIVATE_CHROME_$type" "id.PRIVATE_CHROME_$type" $main
    }
    $null = Element Button 'SearchPlaylistButton' 'SearchPlaylistButton.fixed' $main
    $null = Element MenuItem 'File' 'file-menu' $main
    $dump = @(Get-FmtUiDump $main $walker)
    $redacted = @($dump | ConvertFrom-Json | Where-Object Name -CEQ '<redacted>')
    $redacted.Count -eq 5 -and -not ($dump -cmatch 'PRIVATE_CHROME_') -and
        @($redacted | Where-Object AutomationId -CNE '<redacted>').Count -eq 0 -and
        [bool]($dump -cmatch '"Name":"SearchPlaylistButton"') -and [bool]($dump -cmatch '"Name":"File"')
}

Check 'dump and STOP candidates allow only fixed Custom and Text names' {
    $header.Current.Name = 'Playlists'
    $dump = @(Get-FmtUiDump $fmtMain $walker)
    $records = @($dump | ConvertFrom-Json)
    try { Stop-FmtControls 'fixture' @($header, $formatRow, $privateRow) }
    catch {
        $why = $_.Exception.Message
        if ($why -cnotmatch 'candidates=3 (\[.+\])$') { throw }
        $candidates = @($Matches[1] | ConvertFrom-Json)
        $candidates[0].Name -ceq 'Playlists' -and $candidates[1].Name -ceq 'RBSP Formats' -and
            $candidates[2].Name -ceq '<redacted>' -and $candidates[2].AutomationId -ceq '<redacted>' -and
            @($records | Where-Object { $_.ControlType -ceq 'ControlType.Custom' -and $_.Name -ceq 'RBSP Formats' }).Count -eq 1 -and
            @($records | Where-Object { $_.ControlType -ceq 'ControlType.Text' -and $_.Name -ceq 'Playlists' }).Count -eq 1 -and
            -not ($dump -cmatch 'PRIVATE_CUSTOM') -and $why -cnotmatch 'PRIVATE_CUSTOM'
    }
}

Check 'dump and STOP candidates keep numeric counts only for allowlisted rows' {
    $dump = @(Get-FmtUiDump $fmtMain $walker | ConvertFrom-Json)
    try { Stop-FmtControls 'fixture' @($testsRow, $privateRow) }
    catch {
        if ($_.Exception.Message -cnotmatch 'candidates=2 (\[.+\])$') { throw }
        $candidates = @($Matches[1] | ConvertFrom-Json)
        $dump.Name -ccontains 'RBSP Tests (36)' -and $dump.Name -cnotcontains 'PRIVATE_CUSTOM (36)' -and
            $candidates[0].Name -ceq 'RBSP Tests (36)' -and $candidates[1].Name -ceq '<redacted>' -and
            $candidates[1].AutomationId -ceq '<redacted>'
    }
}

Check 'STOP dump includes the rekordbox popup and redacts its private controls' {
    $fmtHandle = [IntPtr]16
    $B = $Work
    $popup = Element Menu 'menu' ''
    $popup.Current.NativeWindowHandle = 17
    $known = Element MenuItem 'Import Playlist' 'import' $popup
    $privateItem = Element MenuItem 'PRIVATE_MENU_ITEM' 'menu.PRIVATE_MENU_ITEM' $popup
    [Windows.Automation.AutomationElement]::Popup = $popup
    function Get-DesktopWindows { 'rekordbox | menu | 80x60 | 0x11'; 'rekordbox | Other | 80x60 | 0x12' }
    $file = Save-FmtStopDump
    $dump = @(Get-Content -LiteralPath (Join-Path $B $file))
    ($dump[0] | ConvertFrom-Json).Name -ceq 'menu' -and
        $dump -cmatch '"Name":"Import Playlist"' -and $dump -cmatch '"Name":"rekordbox"' -and -not ($dump -cmatch 'PRIVATE_MENU_ITEM')
}

Check 'quit queues one Escape into the verified window before closing' {
    . (Join-Path $KitSource 'kit-lib.ps1')
    [RbspWin]::CloseTrace = ''
    $out = Join-Path $Work 'task-out.txt'
    Remove-Item -LiteralPath $out
    Set-Content -LiteralPath (Join-Path $Work 'task-cmd.txt') -Value @('quit', '', 'fixture')
    $rbProcess.MainWindowHandle = [IntPtr]17
    & $task
    $refused = @(Get-Content -LiteralPath $out)
    if ([RbspWin]::CloseTrace -or $refused[1] -cnotlike '* error: the rekordbox close target changed*') { return $false }
    Remove-Item -LiteralPath $out
    $rbProcess.MainWindowHandle = [IntPtr]16
    & $task
    $reply = @(Get-Content -LiteralPath $out)
    $reply[0] -ceq 'fixture' -and $reply[1] -cmatch $RbCloseAcceptedPattern -and
        ('10:00:00 quit: close asked: True' -cnotmatch $RbCloseAcceptedPattern) -and
        [RbspWin]::CloseTrace -ceq ' key 16/100/1B/10001 key 16/101/1B/C0010001 close'
}

Check 'quit dump includes hidden and inaccessible windows and redacts private names' {
    $popup = Element Window 'PRIVATE_CLOSE_TITLE' 'popup.PRIVATE_CLOSE_TITLE'
    $popup.Current.NativeWindowHandle = 17
    $null = Element Button 'PRIVATE_CLOSE_BUTTON' 'button.PRIVATE_CLOSE_BUTTON' $popup
    [Windows.Automation.AutomationElement]::Popup = $popup
    $hidden = Element Window 'PRIVATE_HIDDEN_TITLE' 'hidden.PRIVATE_HIDDEN_TITLE'
    $hidden.Current.NativeWindowHandle = 19
    $hidden.Current.IsOffscreen = $true
    [Windows.Automation.AutomationElement]::Hidden = $hidden
    Remove-Item -LiteralPath (Join-Path $Work 'task-out.txt')
    Set-Content -LiteralPath (Join-Path $Work 'task-cmd.txt') -Value @('quitdump', '', 'fixture')
    & $task
    $reply = @(Get-Content -LiteralPath (Join-Path $Work 'task-out.txt'))
    if ($reply[0] -cne 'fixture' -or $reply[1] -cnotmatch ' quitdump: OK (format-uidump-[a-f0-9]{32}\.jsonl)$') { throw ($reply -join ' ') }
    $dump = @(Get-Content -LiteralPath (Join-Path $Work $Matches[1]))
    $records = @($dump | ConvertFrom-Json)
    $roots = @($records | Where-Object { $_.Depth -eq 0 })
    ($roots.Capture -join ',') -ceq 'close-0x10,close-0x11,close-0x13,close-0x14' -and
        $roots[1].Name -ceq '<redacted>' -and $roots[2].IsOffscreen -and $roots[3].Status -ceq 'UI Automation unavailable' -and
        -not ($dump -cmatch 'PRIVATE_')
}

Check 'dump caps the traversal at 12 levels below the root and 3000 records' {
    $base = Element Window 'rekordbox' 'bounds'
    $node = $base
    foreach ($depth in 1..14) { $node = Element Pane '' "depth-$depth" $node }
    foreach ($i in 1..3000) { $leaf = Element Pane '' "leaf-$i" $base }
    $dump = @(Get-FmtUiDump $base $walker)
    $dump.Count -eq 3000 -and -not ($dump -cmatch '"Depth":1[34]') -and
        ($dump[12] | ConvertFrom-Json).Depth -eq 12 -and ($dump[-1] | ConvertFrom-Json).AutomationId -ceq 'leaf-2987'
}

Check 'STOP logs bounded offscreen candidates without private names' {
    $dialog = Element Window 'rekordbox' 'dialog'
    foreach ($i in 1..11) {
        $edit = Element Edit "PRIVATE_EDIT_$i" "field-$i" $dialog
        $edit.Current.IsOffscreen = $true
        $edit.Current.IsEnabled = $false
    }
    try { $null = Get-FmtControl $dialog $ct::Edit @() 'missing'; return $false }
    catch {
        $why = $_.Exception.Message
        if ($why -cnotmatch 'matches=0\); candidates=11 (\[.+\])$') { throw }
        $records = @($Matches[1] | ConvertFrom-Json)
        $records.Count -eq 10 -and $records[0].ControlType -ceq 'ControlType.Edit' -and
            $records[0].AutomationId -ceq 'field-1' -and $records[9].AutomationId -ceq 'field-10' -and
            $records[0].Name -ceq '<redacted>' -and -not $records[0].IsEnabled -and $records[0].IsOffscreen -and
            $why -cnotmatch 'PRIVATE_EDIT'
    }
}

Check 'anonymous icons select Collection then confirm headerless Playlists by its pane rows' {
    Set-SourceFixture 'Devices'
    $trace.Clear()
    $layout = Get-FmtSourceLayout
    $source = Open-FmtPlaylistsSource $layout
    ($trace -join ',') -ceq '225,245' -and $header.Current.IsOffscreen -and
        [Windows.Automation.Automation]::Compare($source, $pane) -and
        [Windows.Automation.Automation]::Compare((Get-FmtExplorePlaylist $layout $source), $testsRow)
}

Check 'source row is unavailable if the Playlists icon did not switch the view' {
    Set-SourceFixture 'Collection'
    $trace.Clear()
    $sourceFixture.Verified = $false
    try { $null = Open-FmtPlaylistsSource (Get-FmtSourceLayout); return $false }
    catch {
        $_.Exception.Message -clike 'formats: could not verify the Playlists source after icon selection; candidates=*' -and
            ($trace -join ',') -ceq '245'
    } finally { $sourceFixture.Verified = $true }
}

Check 'unnamed source rows require the full visible caption in the captured row slots' {
    Set-SourceFixture 'Playlists'
    $playlistSearch.Current.IsOffscreen = $true
    $playlistsRow.Current.Name = $testsRow.Current.Name = ''
    $printed = @{ Tests = 'RBSP Tests (36)' }
    function Read-FmtClientText($r) {
        switch ($r.Top) {
            293 { 'Playlists' }
            308 { $printed.Tests }
            default { throw 'fixture: read outside the known unnamed rows' }
        }
    }
    try {
        $layout = Get-FmtSourceLayout
        $source = Find-FmtSource $layout 'Playlists'
        if (-not [Windows.Automation.Automation]::Compare((Get-FmtExplorePlaylist $layout $source), $testsRow)) { return $false }
        $printed.Tests = 'My RBSP Tests (36)'
        try { $null = Get-FmtExplorePlaylist $layout $source; return $false }
        catch { $_.Exception.Message -clike 'formats: expected one enabled RBSP Tests playlist*' }
    } finally {
        $playlistSearch.Current.IsOffscreen = $false
        $playlistsRow.Current.Name = 'Playlists'
        $testsRow.Current.Name = 'RBSP Tests (36)'
    }
}

Check 'the verified search button identifies Playlists without a root caption' {
    Set-SourceFixture 'Playlists'
    $playlistsRow.Current.Name = ''
    function Read-FmtClientText { '' }
    try {
        $layout = Get-FmtSourceLayout
        if (-not [Windows.Automation.Automation]::Compare((Find-FmtSource $layout 'Playlists'), $pane)) { return $false }
        $playlistSearch.Current.BoundingRectangle = [Windows.Rect]::new(40, 222, 24, 23)
        -not (Find-FmtSource $layout 'Playlists')
    } finally {
        $playlistSearch.Current.BoundingRectangle = [Windows.Rect]::new(37, 222, 24, 23)
        $playlistsRow.Current.Name = 'Playlists'
    }
}

Check 'source lookup rejects a shifted icon before acting' {
    Set-SourceFixture 'Collection'
    $trace.Clear()
    $icons[15].Current.BoundingRectangle = [Windows.Rect]::new(7, 249, 18, 18)
    try { $null = Open-FmtPlaylistsSource (Get-FmtSourceLayout); return $false }
    catch {
        $_.Exception.Message -clike 'formats: rekordbox source icon positions changed; candidates=*' -and -not $trace.Count
    } finally {
        $icons[15].Current.BoundingRectangle = [Windows.Rect]::new(7, 245, 18, 18)
    }
}

Check 'explore captures menus and cancels pickers without activating library commands' {
    Set-SourceFixture 'Playlists'
    $B = $Work
    $fmtProgress = @{}
    $fmtExplore = @{ File = ''; Count = 0 }
    $state = @{ Stage = 'main'; Trace = [Collections.Generic.List[string]]::new() }
    $menus = @{}
    foreach ($stage in 'file', 'import', 'context', 'export') { $menus[$stage] = Element Menu 'menu' $stage }
    $menus.file.Current.NativeWindowHandle = 17
    $menus.import.Current.NativeWindowHandle = 18
    $menus.context.Current.NativeWindowHandle = 19
    $menus.export.Current.NativeWindowHandle = 20
    $null = Element MenuItem 'Import' 'import' $menus.file
    $null = Element MenuItem ('Import Playlist' + [char]0x2026) 'import-playlist' $menus.import
    $trackItem = Element MenuItem 'Import Track' 'import-track' $menus.import
    $trackItem.Current.AcceleratorKey = 'Ctrl+O'
    $null = Element MenuItem 'Export a playlist to a file' 'export-parent' $menus.context
    foreach ($caption in 'Export', 'Remove', 'Delete Playlist') {
        $null = Element MenuItem $caption 'unsafe' $menus.context
    }
    $null = Element MenuItem 'Export a playlist to a file for music apps (*.m3u8)' 'export-leaf' $menus.export
    $picker = Element Window 'Open' 'open'
    $picker.Current.NativeWindowHandle = 21
    $picker.Current.ClassName = '#32770'
    $null = Element Button 'Cancel' '2' $picker
    $null = Element Button 'Open' '1' $picker
    $null = Element Button 'OK' 'ok' $picker
    function Get-FmtWindows([string]$phase, [switch]$Dialog) {
        if ($state.Stage -eq 'main') { return }
        if ($phase -eq 'ExploreOpen' -and $Dialog -and $state.Stage -cin 'playlist-dialog', 'track-dialog') { return $picker }
        if ($phase -eq 'Menu' -and $state.Stage -ceq 'import') { return $menus.file, $menus.import }
        if ($phase -eq 'Menu' -and $state.Stage -ceq 'export') { return $menus.context, $menus.export }
        if ($phase -eq 'Menu' -and $menus.ContainsKey($state.Stage)) { return $menus[$state.Stage] }
        throw "fixture: wrong phase $phase for $($state.Stage)"
    }
    function Open-FmtFileMenuBar {
        if (-not $state.Native) { return $false }
        $null = Get-FmtWindows 'Main'
        $state.Trace.Add('native File')
        $state.Stage = 'file'
        return $true
    }
    function Send-FmtKeys([string]$phase, [string]$keys) {
        $null = Get-FmtWindows $phase
        $state.Trace.Add($keys)
        switch -CaseSensitive ($keys) {
            '%f' { $state.Stage = 'file' }
            '^o' { $state.Stage = 'track-dialog' }
            '{ESC}' { $state.Stage = 'main' }
            default { throw 'fixture: unsafe key' }
        }
    }
    function Invoke-FmtClick($el, [switch]$Right, [IntPtr]$Popup = [IntPtr]::Zero, [switch]$Hover) {
        if ($Hover) {
            $null = Get-FmtWindows 'Menu'
            $state.Trace.Add("hover $($el.Current.Name)")
            switch -CaseSensitive ($el.Current.Name) {
                'Import' { $state.Stage = 'import' }
                'Export a playlist to a file' { $state.Stage = 'export' }
                default { throw 'fixture: unexpected hover' }
            }
        } else {
            $null = Get-FmtWindows 'Main'
            if (-not [Windows.Automation.Automation]::Compare($el, $testsRow)) { throw 'fixture: wrong playlist row' }
            if ($Right) { $state.Trace.Add('right RBSP Tests'); $state.Stage = 'context' }
            else { $state.Trace.Add('select RBSP Tests') }
        }
    }
    function Invoke-El($el) {
        $caption = Get-FmtMenuCaption $el.Current.Name
        $state.Trace.Add("invoke $caption")
        switch -CaseSensitive ($caption) {
            'Import Playlist' {
                if ($state.Stage -cne 'import') { throw 'fixture: wrong import menu' }
                $state.Stage = 'playlist-dialog'
            }
            'Cancel' {
                $null = Get-FmtWindows 'ExploreOpen' -Dialog
                $state.Stage = 'main'
            }
            default { throw "fixture: activated unsafe item $caption" }
        }
        return $true
    }
    $reply = Invoke-FmtExplore
    if ($reply -cnotmatch '^fmtuidump: OK (\d+) (format-uidump-[a-f0-9]{32}\.jsonl)$') { throw $reply }
    $count = [int]$Matches[1]
    $dump = @(Get-Content -LiteralPath (Join-Path $B $Matches[2]) | ConvertFrom-Json)
    $captures = @($dump | Where-Object Result)
    $expected = '%f|hover Import|invoke Import Playlist|invoke Cancel|^o|invoke Cancel|select RBSP Tests|right RBSP Tests|hover Export a playlist to a file|{ESC}'
    if ($count -ne $dump.Count -or $state.Stage -cne 'main' -or
        ($captures.Capture -join '|') -cne 'file-menu|import-menu|import-playlist-dialog|ctrl-o-dialog|playlist-context-menu|playlist-export-menu' -or
        ($captures.Result -join '|') -cne 'opened|opened|standard-open|standard-open|opened|opened') { return $false }
    $state.Stage = 'context'
    foreach ($caption in 'Remove', 'Delete Playlist', 'Export a playlist to a file for music apps (*.m3u8)', 'OK') {
        try { Open-FmtExploreMenu $caption @($menus.context); return $false }
        catch { if ($_.Exception.Message -cne 'formats: menu action is outside the explore allowlist') { throw } }
    }
    if (($state.Trace -join '|') -cne $expected) { return $false }

    $state.Stage = 'main'
    $state.Trace.Clear()
    $state.Native = $true
    $trackItem.Current.AcceleratorKey = ''
    $fmtExplore.File = ''
    $fmtExplore.Count = 0
    $null = Invoke-FmtExplore
    $captures = @(Get-Content -LiteralPath (Join-Path $B $fmtExplore.File) | ConvertFrom-Json | Where-Object Result)
    $expected = $expected.Replace('%f', 'native File').Replace('|^o|invoke Cancel', '')
    ($state.Trace -join '|') -ceq $expected -and
        ($captures | Where-Object Capture -CEQ 'ctrl-o-dialog').Result -ceq 'binding-unverified'
}

Check 'a failed format step closes the menu that would stop quit' {
    $B = $Work
    . (Join-Path $KitSource 'watch-lib.ps1')
    $menu = Element Menu 'menu' ''
    $state = @{ Open = $true; Captured = $false; Escapes = 0 }
    function Uia {}
    function Screen-Dpi {}
    function Add-Type([string[]]$AssemblyName, [string]$TypeDefinition) {
        if ($TypeDefinition) { Microsoft.PowerShell.Utility\Add-Type -TypeDefinition $TypeDefinition }
        elseif (($AssemblyName -join ',') -cne 'System.Windows.Forms,WindowsBase') { throw 'fixture: unexpected assemblies' }
    }
    function Get-RbMain([switch]$FormatMenu) {
        if (-not $FormatMenu) { throw 'fixture: expected format step with a menu' }
        return [IntPtr]16, 42
    }
    function Get-FmtMainElement([IntPtr]$handle, [int]$processId) {
        if ($handle -ne [IntPtr]16 -or $processId -ne 42) { throw 'fixture: wrong main window' }
        return $fmtMain
    }
    function Invoke-FmtStep([string]$action) { throw 'formats: fixture step failed' }
    function Save-FmtStopDump([switch]$Step) {
        $state.Captured = $Step -and $state.Open
        return 'fixture-menu.jsonl'
    }
    function Get-FmtWindows([string]$phase, [switch]$Refocus, [switch]$Dialog) {
        if ($phase -cne 'Menu' -or $Refocus -or $Dialog) { throw 'fixture: unexpected window phase' }
        if ($state.Open) { $menu }
    }
    function Send-FmtKeys([string]$phase, [string]$keys) {
        if ($phase -cne 'Menu' -or $keys -cne '{ESC}' -or -not $state.Open) { throw 'fixture: unexpected input' }
        $state.Escapes++
        $state.Open = $false
    }
    function Session([string]$cmd, [int]$wait = 30, [string]$arg = '') {
        if ($cmd -ceq 'shot') { return '11:52:40 shot menu.png' }
        if ($cmd -cne 'windows') { throw 'fixture: unexpected mailbox command' }
        $line = '11:52:40 windows: rekordbox | rekordbox | 1200x800 | 0x10'
        if ($state.Open) { $line += ' || rekordbox | menu | 200x100 | 0x11' }
        return $line
    }
    if ((Watch-Quit) -cnotmatch 'STOP:.*\| menu \| 0x11') { return $false }
    try { $null = Invoke-RbFormatTask 'fmtstep' 'click import'; return $false }
    catch {
        $_.Exception.Message -ceq 'formats: fixture step failed; step capture: fixture-menu.jsonl' -and
            $state.Captured -and $state.Escapes -eq 1 -and -not (Watch-Quit)
    }
}

EOF
  TEMP="$tmp" LOCALAPPDATA="$tmp" APPDATA="$tmp" pwsh -NoProfile -NonInteractive -File "$tmp/format-uia-test.ps1" "$here/kit" "$tmp/format-uia" > "$tmp/fu" 2>&1
  n="$(grep -c '^ok ' "$tmp/fu")"; pass=$((pass + n))
  [ "$n" = 19 ] && ! grep -qv '^ok ' "$tmp/fu" || { no "format UIA ($n of 19 OK):"; grep -v '^ok ' "$tmp/fu" | sed 's/^/    /'; }
  # Exercise the import picker and installed helper's JSON contracts without rekordbox.
  cat > "$tmp/format-import-test.ps1" << 'PS'
param($KitSource, $Work)
$ErrorActionPreference = 'Stop'
. (Join-Path $KitSource 'format-lib.ps1')
. (Join-Path $KitSource 'format-library.ps1')
. (Join-Path $KitSource 'format-ui.ps1')
$env:SystemDrive = [IO.Path]::GetPathRoot($Work).TrimEnd([IO.Path]::DirectorySeparatorChar)
$env:TEMP = $Work
$Kit = Join-Path $Work ("tracks " + [char]0x00e9)
$B = Join-Path $Work 'session'
$corpus = Join-Path $Kit 'corpus'
$dir = Join-Path $corpus 'formats'
$export = Join-Path $B 'formats-library.m3u8'
$statePath = Join-Path $B 'format-import-state.json'
$fmtProgress = @{}
[void][IO.Directory]::CreateDirectory($B)
[void][IO.Directory]::CreateDirectory($dir)
[IO.File]::WriteAllText((Join-Path $dir 'RBSP-FORMATS'), 'RBSP format subset v1')
$first = Join-Path $dir 'fmt-a.wav'
$second = Join-Path $dir 'fmt-b.m4a'
$fixture = @{}
function Write-Subset([string]$version) {
    [IO.File]::WriteAllText($first, $version)
    [IO.File]::WriteAllText($second, 'fixture')
    $lines = foreach ($p in $second, $first) {
        (Get-FileHash -LiteralPath $p).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($p)
    }
    [IO.File]::WriteAllLines((Join-Path $dir 'SHA256SUMS'), [string[]]$lines)
}
function Reset-Fixture {
    foreach ($p in $export, $statePath) { [IO.File]::Delete($p) }
    Write-Subset 'original'
    $fixture.Live = $false
    $fixture.Duplicate = $false
    $fixture.OtherHeading = ''
    $fixture.UnrelatedTracks = $false
    $fixture.ExtraFile = ''
    $fixture.Clock = [datetime]'2026-11-21T11:00:00'
    $fixture.Trace = [Collections.Generic.List[string]]::new()
    $fixture.Readback = @($second)
}
function Get-Date { $fixture.Clock }
function Start-Sleep { $fixture.Clock = $fixture.Clock.AddMilliseconds(500) }
function Get-FmtWindowState([string]$phase) {
    if ($phase -cne 'ImportResult') { throw 'fixture: invalid result phase' }
    @{ Dialogs = @(); ImportProgress = @() }
}
function Complete-FmtOperation([string]$phase) { $fixture.Trace.Add($phase) }
function Open-FmtExploreMenu([string]$name, [object[]]$within, [switch]$NoShortcut) {
    $allowed = switch ($name) {
        'File' { $NoShortcut -and -not $within }
        'Import' { ($within -join '|') -ceq 'file popup' }
        'Import Playlist' { ($within -join '|') -ceq 'import popup' }
    }
    if (-not $allowed) { throw 'fixture: unexpected import menu action' }
    $fixture.Trace.Add($name)
}
function Get-FmtExploreMenuSurfaces($popups) { $popups }
function Wait-FmtExploreMenu([object[]]$previous) {
    if ($fixture.Trace[-1] -ceq 'File' -and -not $previous) { return 'file popup' }
    if ($fixture.Trace[-1] -ceq 'Import' -and ($previous -join '|') -ceq 'file popup') { return 'import popup' }
    throw 'fixture: unexpected menu transition'
}
function Submit-FmtImportPicker([string]$value) {
    $fixture.InputName = [IO.Path]::GetFileName($value)
    $fixture.InputPath = $value
    $fixture.InputBytes = [IO.File]::ReadAllBytes($value)
    $fixture.Name = (Get-FormatImportState $dir).name
    $fixture.Trace.Add('Open')
    $fixture.Live = $true
}
function Find-InstalledPrepare { @{ FullName = 'installed rbsp-prepare.exe' } }
function Invoke-FormatLibraryHelper([string]$exe, [string]$arguments, [datetime]$deadline) {
    if ($exe -cne 'installed rbsp-prepare.exe') { throw 'fixture: wrong executable' }
    $fixture.Trace.Add($arguments)
    if ($arguments -ceq '--playlists') {
        '{"playlist":"opaque-folder","parent":"root","name":"Other folder","kind":"folder","tracks":0}' | ConvertFrom-Json
        '{"playlist":"nested","parent":"opaque-folder","name":"RBSP Test Folder","kind":"folder","tracks":0}' | ConvertFrom-Json
        '{"playlist":"outside","parent":"root","name":"Private playlist","kind":"playlist","tracks":99}' | ConvertFrom-Json
        if ($fixture.Live) {
            [pscustomobject]@{ playlist = 'opaque-3'; parent = 'root'; name = $fixture.Name; kind = 'playlist'; tracks = 10 }
        }
        if ($fixture.Duplicate) {
            [pscustomobject]@{ playlist = 'opaque-4'; parent = 'root'; name = $fixture.Name; kind = 'playlist'; tracks = 10 }
        }
        if ($fixture.OtherHeading) {
            [pscustomobject]@{ playlist = 'opaque-5'; parent = 'opaque-folder'; name = $fixture.OtherHeading; kind = 'playlist'; tracks = 10 }
        }
    } elseif ($arguments -cin '--tracks opaque-3', '--tracks opaque-4') {
        if ($fixture.UnrelatedTracks) {
            [pscustomobject]@{ file = 'https://example.invalid/stream' }
            [pscustomobject]@{ file = (Join-Path $Work 'outside/fmt-b.m4a') }
        }
        if ($fixture.ExtraFile) { [pscustomobject]@{ file = $fixture.ExtraFile } }
        foreach ($p in $fixture.Readback) { [pscustomobject]@{ file = $p } }
    } else { throw 'fixture: requested tracks outside RBSP' }
}
function Check($name, [scriptblock]$test) {
    try { if (& $test) { "ok $name" } else { "bad $name" } }
    catch { "bad ${name}: $($_.Exception.Message)" }
}

Check 'submits a unique BOM-free M3U8 only after recording the expected name' {
    Reset-Fixture
    Invoke-FmtImport
    $expected = [Text.Encoding]::UTF8.GetBytes("#EXTM3U`r`n$first`r`n$second`r`n")
    $fixture.InputName -ceq ($fixture.Name + '.m3u8') -and
        $fixture.Name -cmatch '^RBSP Formats [a-f0-9]{16}-[a-f0-9]{12}$' -and
        [Convert]::ToBase64String($fixture.InputBytes) -ceq [Convert]::ToBase64String($expected) -and
        ($fixture.Trace -join '|') -ceq 'File|Import|Import Playlist|Open|Refusal' -and
        -not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($fixture.InputPath))
}

Check 'library check filters unrelated rows and retains sibling positions' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.UnrelatedTracks = $true
    $fixture.Trace.Clear()
    $summary = Join-Path $B (Save-FormatLibrary $corpus)
    $json = [IO.File]::ReadAllText($summary)
    $list = @(ConvertFrom-Json $json)
    $list[0].name -ceq 'RBSP Test Folder' -and ($list[0].treePath -join ',') -ceq '1,1' -and
        $list[1].id -ceq 'opaque-3' -and $list[1].parent -ceq 'root' -and $list[1].order -eq 3 -and
        ($list[1].corpusTracks -join ',') -ceq 'fmt-b' -and
        ($fixture.Trace -join '|') -ceq '--playlists|--tracks opaque-3' -and
        $json -cnotmatch 'Private|Other folder|https|outside|"tracks"\s*:\s*99\b'
}

Check 'library verification classifies refusals and writes the ordered export' {
    Reset-Fixture
    Invoke-FmtImport
    $entry = Confirm-FormatLibraryImport $corpus
    $staged = Get-FormatFiles $dir -Verify
    $outcomes = @(Get-FormatImportOutcomes $staged $entry.corpusTracks)
    ($outcomes -join '|') -ceq 'fmt-a IMPORT_REFUSED|fmt-b IMPORT_ADDED' -and
        (Get-FormatImportState $dir).playlist.id -ceq 'opaque-3' -and
        ([IO.File]::ReadAllLines($export) -join '|') -ceq "#EXTM3U|$second"
}

Check 'verification records other format headings from the same listing' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.OtherHeading = 'RBSP Formats 1111111111111111-111111111111'
    $entry = Confirm-FormatLibraryImport $corpus
    $entry.id -ceq 'opaque-3' -and
        ((Get-FormatImportState $dir).playlist.otherFormatNames -join ',') -ceq $fixture.OtherHeading
}

Check 'verification counts folders and playlists in the full listing' {
    Reset-Fixture
    Invoke-FmtImport
    $entry = Confirm-FormatLibraryImport $corpus
    $entry.listedPlaylistRows -eq 4 -and (Get-FormatImportState $dir).playlist.listedPlaylistRows -eq 4
}

Check 'recorded verification rejects even one non-corpus row' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.ExtraFile = Join-Path $Work 'outside/fmt-b.m4a'
    try { $null = Confirm-FormatLibraryImport $corpus; return $false }
    catch {
        $_.Exception.Message -ceq 'formats: library playlist has an unexpected or repeated corpus row' -and
            -not (Get-FormatImportState $dir).playlist -and -not [IO.File]::Exists($export)
    }
}

Check 'recorded lookup rejects a repeated staged row' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.Readback = @($second, $second)
    try { $null = Find-FormatLibraryPlaylist $corpus $fixture.Name (Get-FormatFiles $dir -Verify); return $false }
    catch { $_.Exception.Message -ceq 'formats: library playlist has an unexpected or repeated corpus row' }
}

Check 'reuse finds a complete kit playlist without a saved import state' {
    Reset-Fixture
    Invoke-FmtImport
    [IO.File]::Delete($statePath)
    $fixture.UnrelatedTracks = $false
    $fixture.Readback = @($second, $first)
    $entry = Restore-FormatLibraryImport $corpus
    $entry.id -ceq 'opaque-3' -and (Get-FormatImportState $dir).playlist.name -ceq $fixture.Name -and
        ([IO.File]::ReadAllLines($export) -join '|') -ceq "#EXTM3U|$second|$first"
}

Check 'reuse skips incomplete, repeated, unrelated and other-subset entries' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.UnrelatedTracks = $false
    if (Restore-FormatLibraryImport $corpus) { return $false }
    $fixture.Readback = @($first, $first)
    if (Restore-FormatLibraryImport $corpus) { return $false }
    $fixture.Readback = @($first, $second)
    $fixture.UnrelatedTracks = $true
    if (Restore-FormatLibraryImport $corpus) { return $false }
    $fixture.UnrelatedTracks = $false
    Write-Subset 'new contents'
    -not (Restore-FormatLibraryImport $corpus) -and -not [IO.File]::Exists($export)
}

Check 'verification stops when the expected playlist never appeared or is duplicated' {
    Reset-Fixture
    Invoke-FmtImport
    $fixture.Live = $false
    try { $null = Confirm-FormatLibraryImport $corpus; return $false }
    catch { if ($_.Exception.Message -cnotlike 'formats: expected exactly one library playlist*') { throw } }
    $fixture.Live = $true
    $fixture.Duplicate = $true
    try { $null = Confirm-FormatLibraryImport $corpus; return $false }
    catch {
        $_.Exception.Message -clike 'formats: expected exactly one library playlist*' -and
            -not (Get-FormatImportState $dir).playlist -and -not [IO.File]::Exists($export)
    }
}

Check 'a recheck fails if the recorded playlist membership changed' {
    Reset-Fixture
    Invoke-FmtImport
    $null = Confirm-FormatLibraryImport $corpus
    $fixture.Readback = @($first)
    try { $null = Confirm-FormatLibraryImport $corpus; return $false }
    catch {
        $_.Exception.Message -ceq 'formats: library playlist identity or ordered corpus changed since import' -and
            ([IO.File]::ReadAllLines($export) -join '|') -ceq "#EXTM3U|$second"
    }
}

Check 'submitting twice makes distinct names without removing old playlists' {
    Reset-Fixture
    Invoke-FmtImport
    $firstName = $fixture.Name
    $fixture.Trace.Clear()
    Invoke-FmtImport
    $fixture.Name -cne $firstName -and ($fixture.Trace -join '|') -ceq 'File|Import|Import Playlist|Open|Refusal'
}
PS
  pwsh -NoProfile -NonInteractive -File "$tmp/format-import-test.ps1" "$here/kit" "$tmp/format-import" > "$tmp/fi" 2>&1
  n="$(grep -c '^ok ' "$tmp/fi")"; pass=$((pass + n))
  [ "$n" = 12 ] && ! grep -qv '^ok ' "$tmp/fi" || { no "format import ($n of 12 OK):"; grep -v '^ok ' "$tmp/fi" | sed 's/^/    /'; }
  cat > "$tmp/statusdump-test.ps1" << 'PS'
param($ScriptPath, $Work)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections;
namespace Windows.Automation {
    [Flags] public enum TreeScope { Children = 2, Descendants = 4 }
    public class AutomationProperty {}
    public abstract class Condition {}
    public class PropertyCondition : Condition {
        public PropertyCondition(AutomationProperty property, object value) {}
    }
    public class OrCondition : Condition {
        public OrCondition(params Condition[] conditions) {}
    }
    public class ControlType {
        public static readonly ControlType TabItem = new ControlType(), List = new ControlType(), DataGrid = new ControlType();
    }
    public class AutomationElementCollection : IEnumerable {
        public IEnumerator GetEnumerator() { return (new AutomationElement[0]).GetEnumerator(); }
    }
    public class AutomationElement {
        public static AutomationElement RootElement { get { return new AutomationElement(); } }
        public static readonly AutomationProperty ProcessIdProperty = new AutomationProperty(), ControlTypeProperty = new AutomationProperty();
        public AutomationElementCollection FindAll(TreeScope scope, Condition condition) {
            if (scope != TreeScope.Children || !(condition is PropertyCondition)) throw new InvalidOperationException("unexpected fixture query");
            return new AutomationElementCollection();
        }
    }
}
'@
[void][IO.Directory]::CreateDirectory($Work)
Set-Content -LiteralPath (Join-Path $Work 'kit-lib.ps1') -Value '$Kit = $B = $PSScriptRoot'
$stub = @'
function Uia {}
function Get-App { [pscustomobject]@{ Id = 43 } }
function Find-El($app, [string]$name, $type, [int]$secs, [switch]$Enabled) {
    if ($app.Id -ne 43 -or $name -cne 'Status' -or $type -ne [Windows.Automation.ControlType]::TabItem -or
        $secs -ne 30 -or $Enabled) { throw 'unexpected status tab lookup' }
    return [Windows.Automation.AutomationElement]::RootElement
}
function Select-El($e) { return $e -is [Windows.Automation.AutomationElement] }
function Start-Sleep([int]$Seconds) {}
try {
    $sid = [Diagnostics.Process]::GetCurrentProcess().SessionId
'@
$task = Join-Path $Work 'session-task.ps1'
$source = [IO.File]::ReadAllText($ScriptPath)
$source -replace '(?m)^try \{\r?\n    \$sid = \[Diagnostics\.Process\]::GetCurrentProcess\(\)\.SessionId', $stub |
    Set-Content -LiteralPath $task
Set-Content -LiteralPath (Join-Path $Work 'task-cmd.txt') -Value @('statusdump', '', 'status-fixture')
Push-Location $Work
try { & $task } finally { Pop-Location }
$answer = @(Get-Content -LiteralPath (Join-Path $Work 'task-out.txt'))
if ($answer.Count -ne 2 -or $answer[0] -cne 'status-fixture' -or $answer[1] -cnotmatch '^\d\d:\d\d:\d\d statusdump: 0 element\(s\): $') {
    throw "unexpected status reply: $answer"
}
PS
  if pwsh -NoProfile -NonInteractive -File "$tmp/statusdump-test.ps1" "$here/kit/session-task.ps1" "$tmp/statusdump" > "$tmp/statusdump-out" 2>&1
  then ok; else no "statusdump did not write its mailbox reply"; sed 's/^/    /' "$tmp/statusdump-out"; fi
  # Our app's window, minimized by the front step, restored through UIA before its own UI is driven.
  mkdir "$tmp/restore"
  cat > "$tmp/restore/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process { [pscustomobject]@{ Id = 43; ProcessName = 'RB Stems Plus'; MainWindowHandle = [IntPtr]32 } }
EOF
  cat > "$tmp/restore-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public static class RbspApp { public static string Mode, Trace; public static int State; }
namespace Windows.Automation {
  public enum WindowVisualState { Normal, Maximized, Minimized }
  public class AutomationElement {
    public IntPtr Handle;
    public static AutomationElement FromHandle(IntPtr h) { return new AutomationElement { Handle = h }; }
    public bool TryGetCurrentPattern(object pattern, out object p) {
      p = new WindowPattern { Handle = Handle };
      return RbspApp.Mode != "no pattern";
    }
  }
  public class WindowPatternInfo { public WindowVisualState WindowVisualState { get { return (WindowVisualState)RbspApp.State; } } }
  public class WindowPattern {
    public static object Pattern = new object();
    public IntPtr Handle;
    public WindowPatternInfo Current { get { return new WindowPatternInfo(); } }
    public void SetWindowVisualState(WindowVisualState s) {
      RbspApp.Trace += " " + s.ToString().ToLower() + " 0x" + Handle.ToString("X");
      if (RbspApp.Mode != "stays") RbspApp.State = (int)s;
    }
  }
}
'@
$stub = @'
function Uia {}
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
function Case($mode, [int]$state, $trace) {
    [RbspApp]::Mode = $mode; [RbspApp]::State = $state; [RbspApp]::Trace = ''
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('app', '', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspApp]::Trace -eq $trace -and $out.Count -eq 2 -and $out[1] -like '* app: running, its window up (pid 43)') { "ok $mode" }
    else { "bad ${mode}: $out (trace:$([RbspApp]::Trace))" }
}
Case 'minimized' 2 ' normal 0x20'
Case 'normal' 0 ''
Case 'maximized' 1 ''
Case 'no pattern' 2 ''
Case 'stays' 2 ' normal 0x20'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/restore-test.ps1" "$here/kit/session-task.ps1" "$tmp/restore" > "$tmp/ra" 2>&1
  n="$(grep -c '^ok ' "$tmp/ra")"; pass=$((pass + n))
  [ "$n" = 5 ] && ! grep -qv '^ok ' "$tmp/ra" || { no "app restore ($n of 5 OK):"; grep -v '^ok ' "$tmp/ra" | sed 's/^/    /'; }
  # Auto Analysis off (owner decision 2026-11-19): only the check box of the "Auto Analysis" section
  # (the PC's Preferences > Analysis layout), unticked and read back; nothing else touched.
  mkdir "$tmp/aa"
  printf '%s\n' '$Kit = $PSScriptRoot; $B = $Kit' > "$tmp/aa/kit-lib.ps1"
  cat > "$tmp/aa-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
$uiTypes = @'
using System; using System.Collections.Generic;
public static class RbspAa { public static string Mode, Trace; }
namespace Windows.Automation {
  public enum TreeScope { Descendants }
  public class Condition { public static Condition TrueCondition = new Condition(); }
  public class ControlType { public static ControlType Text = new ControlType(), CheckBox = new ControlType(), Button = new ControlType(); }
  public class TogglePattern { public static object Pattern = new object(); }
  public class ToggleInfo { public AutomationElement E; public string ToggleState { get { return E.State; } } }
  public class Toggler {
    public AutomationElement E;
    public ToggleInfo Current { get { return new ToggleInfo { E = E }; } }
    public void Toggle() { RbspAa.Trace += " toggle " + E.Id; if (RbspAa.Mode != "stays on") E.State = E.State == "On" ? "Off" : "On"; }
  }
  public class Info { public string Name; public ControlType ControlType; public Windows.Rect BoundingRectangle; }
  public class AutomationElement {
    public string Id, State; public Info Current;
    public static AutomationElement El(string id, string name, ControlType t, double left, double top, double h, string state) {
      return new AutomationElement { Id = id, State = state, Current = new Info { Name = name, ControlType = t, BoundingRectangle = new Windows.Rect(left, top, 20, h) } };
    }
    public bool TryGetCurrentPattern(object pattern, out object p) { p = new Toggler { E = this }; return State != null; }
    public static List<AutomationElement> Items = new List<AutomationElement>();
    public AutomationElement[] FindAll(TreeScope s, Condition c) { return Items.ToArray(); }
  }
}
'@
Add-Type -TypeDefinition ($uiTypes + "`n" + [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'wpf-rect.cs')))
$stub = @'
function Uia {}
function Shot { 'shot s.png' }
function Open-RbPrefs { if ([RbspAa]::Mode -eq 'prefs not open') { 'prefsopen: no Preferences window in 10 s' } else { 'prefsopen: ok; 0x90' } }
function Select-RbPrefsTab($n) { "prefstab: ok; $n" }
function Close-RbPrefs { [RbspAa]::Trace += ' close'; 'prefsclose: ok; closed' }
function Get-RbPrefsEl { [Windows.Automation.AutomationElement]::new() }
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$A = [Windows.Automation.AutomationElement]; $CT = [Windows.Automation.ControlType]
function Case($mode, $trace, $answer) {
    [RbspAa]::Mode = $mode; [RbspAa]::Trace = ''
    $auto = if ($mode -eq 'off') { 'Off' } else { 'On' }; $name = if ($mode -eq 'off') { 'Disable' } else { 'Enable' }
    $items = @(
        $A::El('quality', 'Track Analysis Setting', $CT::Text, 776, 389, 20, $null)
        $A::El('key', 'KEY', $CT::CheckBox, 834, 449, 19, 'Off'))
    if ($mode -ne 'no label') { $items += $A::El('label', 'Auto Analysis', $CT::Text, 776, 549, 20, $null) }
    $items += $A::El('auto', $name, $CT::CheckBox, 808, 583, 19, $auto)
    if ($mode -eq 'two boxes') { $items += $A::El('extra', 'Enable', $CT::CheckBox, 808, 605, 19, 'On') }
    $items += @($A::El('cloudlabel', 'Cloud Analysis', $CT::Text, 776, 629, 20, $null), $A::El('cloud', 'Enable', $CT::CheckBox, 808, 663, 19, 'On'),
                $A::El('radarlabel', 'Radar Analysis', $CT::Text, 776, 711, 20, $null), $A::El('radar', 'Enable', $CT::CheckBox, 808, 745, 19, 'On'))
    $A::Items.Clear(); foreach ($i in $items) { $A::Items.Add($i) }
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('autoanalysisoff', '', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspAa]::Trace -eq $trace -and $out.Count -eq 2 -and $out[1] -like "* autoanalysis: $answer") { "ok $mode" } else { "bad ${mode}: $out (trace:$([RbspAa]::Trace))" }
}
Case 'on' ' toggle auto close' 'ok; prefsopen: ok; 0x90; prefstab: ok; Analysis; Auto Analysis was On; before shot s.png; after shot s.png; now Off; prefsclose: ok; closed'
Case 'off' ' close' 'ok; *Auto Analysis was Off; prefsclose: ok; closed'
Case 'stays on' ' toggle auto' 'the Auto Analysis check box reads On after unticking;*'
Case 'no label' '' "expected one 'Auto Analysis' label, found 0;*"
Case 'two boxes' '' 'expected one Enable/Disable check box in the Auto Analysis section, found 2;*'
Case 'prefs not open' '' 'Preferences did not open; prefsopen: no Preferences window in 10 s'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/aa-test.ps1" "$here/kit/session-task.ps1" "$tmp/aa" > "$tmp/aa-out" 2>&1
  n="$(grep -c '^ok ' "$tmp/aa-out")"; pass=$((pass + n))
  [ "$n" = 6 ] && ! grep -qv '^ok ' "$tmp/aa-out" || { no "autoanalysisoff ($n of 6 OK):"; grep -v '^ok ' "$tmp/aa-out" | sed 's/^/    /'; }
  # Our app closed by its own Exit (owner decision 2026-11-19): the menu, else its window's close;
  # gone within 30 s or a STOP; never killed; only this session's copy.
  mkdir "$tmp/ax"
  cat > "$tmp/ax/kit-lib.ps1" << 'EOF'
$Kit = $PSScriptRoot; $B = $Kit
function Get-Process { @([RbspAx]::Procs | Where-Object { $_ }) }
EOF
  cat > "$tmp/ax-test.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $args[1]
Add-Type -TypeDefinition @'
using System;
public class FakeProc {
  public int SessionId; public IntPtr MainWindowHandle = (IntPtr)32; public bool HasExited;
  public void Refresh() { if (RbspAx.Asked && RbspAx.Mode != "stays") HasExited = true; }
}
public static class RbspAx { public static string Mode, Trace; public static bool Asked; public static FakeProc[] Procs; }
namespace Windows.Automation {
  public class ControlType { public static ControlType MenuItem = new ControlType(); }
  public class ExpandCollapsePattern { public static object Pattern = new object(); public void Expand() { RbspAx.Trace += " expand"; } }
  public class WindowPattern { public static object Pattern = new object(); public void Close() { RbspAx.Trace += " close"; RbspAx.Asked = true; } }
  public class AutomationElement {
    public string Name;
    public static AutomationElement FromHandle(IntPtr h) { return new AutomationElement { Name = "window" }; }
    public bool TryGetCurrentPattern(object pattern, out object p) {
      if (pattern == ExpandCollapsePattern.Pattern) { p = new ExpandCollapsePattern(); return true; }
      p = new WindowPattern(); return true;
    }
  }
}
'@
$stub = @'
function Uia {}
function Shot { 'shot x.png' }
function Restore-App($app) { [RbspAx]::Trace += ' restore' }
function Close-OurDialog($app) { if ([RbspAx]::Mode -eq 'dialog') { [RbspAx]::Trace += ' cancel'; 'Install Demucs v4?' } else { '' } }
function Find-El($app, [string]$name, $type, [int]$secs, [switch]$Enabled) {
    if ([RbspAx]::Mode -eq 'no menu') { return $null }
    [RbspAx]::Trace += " find $name"; return [Windows.Automation.AutomationElement]@{ Name = $name }
}
function Invoke-El($e) { [RbspAx]::Trace += " invoke $($e.Name)"; [RbspAx]::Asked = $true; return $true }
try {
'@
$task = Join-Path $B 'session-task.ps1'
(Get-Content -LiteralPath $args[0] -Raw) -replace '(?m)^try \{$', $stub | Set-Content -LiteralPath $task
$cmdFile = Join-Path $B 'task-cmd.txt'; $replyFile = Join-Path $B 'task-out.txt'
$mine = [Diagnostics.Process]::GetCurrentProcess().SessionId
function Case($mode, $trace, $answer) {
    [RbspAx]::Mode = $mode; [RbspAx]::Trace = ''; [RbspAx]::Asked = $false
    [RbspAx]::Procs = switch ($mode) { 'not running' { @() } 'other session' { @([FakeProc]@{ SessionId = $mine + 1 }) } default { @([FakeProc]@{ SessionId = $mine }) } }
    if (Test-Path -LiteralPath $replyFile) { Remove-Item -LiteralPath $replyFile }
    Set-Content -LiteralPath $cmdFile -Value @('appexit', '', 'fixture')
    & $task
    $out = @(Get-Content -LiteralPath $replyFile)
    if ([RbspAx]::Trace -eq $trace -and $out.Count -eq 2 -and $out[1] -like "* appexit: $answer") { "ok $mode" } else { "bad ${mode}: $out (trace:$([RbspAx]::Trace))" }
}
Case 'menu' ' restore find RB Stems Plus expand find Exit invoke Exit' "ok; closed by the menu's RB Stems Plus > Exit in * s"
Case 'no menu' ' restore close' "ok; closed by its window's close (no Exit menu item in UI Automation) in * s"
Case 'not running' '' 'ok; not running'
Case 'other session' '' 'ok; not running'
Case 'stays' ' restore find RB Stems Plus expand find Exit invoke Exit' "still running 30 s after the menu's RB Stems Plus > Exit; shot x.png"
Case 'dialog' ' restore cancel find RB Stems Plus expand find Exit invoke Exit' "ok; closed by its open 'Install Demucs v4?' cancelled, then the menu's RB Stems Plus > Exit in * s"
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/ax-test.ps1" "$here/kit/session-task.ps1" "$tmp/ax" > "$tmp/ax-out" 2>&1
  n="$(grep -c '^ok ' "$tmp/ax-out")"; pass=$((pass + n))
  [ "$n" = 6 ] && ! grep -qv '^ok ' "$tmp/ax-out" || { no "appexit ($n of 6 OK):"; grep -v '^ok ' "$tmp/ax-out" | sed 's/^/    /'; }
  cat > "$tmp/inputdirector-test.ps1" << 'EOF'
param($ScriptPath)
$ErrorActionPreference = 'Stop'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$null, [ref]$null)
$node = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Close-TestInputDirector' }, $false)
. ([scriptblock]::Create($node.Extent.Text))
Add-Type @'
using System;
public class InputDirectorProcess {
    public int Id, SessionId;
    public bool HasExited, CloseWorks;
    public IntPtr Handle = new IntPtr(1);
    public string Trace = "";
    public bool CloseMainWindow() { Trace += "close "; HasExited = CloseWorks; return CloseWorks; }
    public bool WaitForExit(int ms) { Trace += "wait:" + ms + " "; return HasExited; }
}
'@
function Get-Process {
    [CmdletBinding()] param([string]$Name)
    if ($Name -cne 'InputDirector' -or $ErrorActionPreference -ne 'SilentlyContinue') { throw 'wrong process query' }
    $script:inputs
}
function Get-CimInstance([string]$ClassName, [string]$Filter) {
    if ($ClassName -cne 'Win32_Process' -or $Filter -cnotmatch "^ProcessId = (\d+) AND SessionId = 5 AND Name = 'InputDirector.exe'$" -or $Matches[1] -in '3', '4') {
        throw "unexpected CIM query: $ClassName $Filter"
    }
    [pscustomobject]@{ ProcessId = [int]$Matches[1] }
}
function Invoke-CimMethod($InputObject, [string]$MethodName) {
    if ($MethodName -cne 'GetOwner') { throw 'wrong owner lookup' }
    $user = if ($InputObject.ProcessId -eq 5) { 'other' } else { 'rbtest' }
    $rc = if ($InputObject.ProcessId -eq 6) { 2 } else { 0 }
    [pscustomobject]@{ ReturnValue = $rc; User = $user; Domain = 'TESTPC' }
}
function Stop-Process { throw 'InputDirector must never be terminated' }
$env:COMPUTERNAME = 'TESTPC'
$env:USERNAME = 'rbtest'
$script:inputs = @(
    [InputDirectorProcess]@{ Id = 1; SessionId = 5; CloseWorks = $true }
    [InputDirectorProcess]@{ Id = 3; SessionId = 4 }
    [InputDirectorProcess]@{ Id = 4; SessionId = 0 }
    [InputDirectorProcess]@{ Id = 5; SessionId = 5 }
)
$answer = @(Close-TestInputDirector 5)
if ($answer.Count -ne 1 -or $answer[0] -cne 'rbtest InputDirector: 1 closed normally' -or
    $inputs[0].Trace -cne 'close wait:10000 ' -or ($inputs[1..3].Trace -join '') -ne '') {
    throw 'incorrect close order, isolation or log'
}
'ok close and session/owner isolation'
if (@(Close-TestInputDirector 5).Count) { throw 'a repeated pre-run should return no log when no owned process remains' }
'ok repeat pre-run'
$script:inputs = @([InputDirectorProcess]@{ Id = 2; SessionId = 5 })
try { Close-TestInputDirector 5; throw 'expected a normal-close timeout' }
catch {
    if ($_.Exception.Message -cne 'InputDirector did not close normally in the rbtest session after 10 s' -or
        $inputs[0].Trace -cne 'close wait:10000 ' -or $inputs[0].HasExited) { throw }
}
'ok timeout leaves InputDirector running'
$script:inputs = @([InputDirectorProcess]@{ Id = 6; SessionId = 5 })
$env:USERNAME = 'other'
if (@(Close-TestInputDirector 5).Count -or $inputs[0].Trace) { throw 'a different account reached the close' }
'ok different account'
$env:USERNAME = 'rbtest'
try { Close-TestInputDirector 5; throw 'expected owner verification to fail' }
catch { if ($_.Exception.Message -cne 'could not verify the InputDirector process owner' -or $inputs[0].Trace) { throw } }
'ok owner verification failure'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/inputdirector-test.ps1" "$here/kit/session-task.ps1" > "$tmp/inputdirector-out" 2>&1
  n="$(grep -c '^ok ' "$tmp/inputdirector-out")"; pass=$((pass + n))
  [ "$n" = 5 ] && ! grep -qv '^ok ' "$tmp/inputdirector-out" || { no "InputDirector pre-run ($n of 5 OK):"; grep -v '^ok ' "$tmp/inputdirector-out" | sed 's/^/    /'; }
  # The DirectML bench's parsing: the timing build's lines, deck 1's readouts, the advance check.
  cat > "$tmp/bench-test.ps1" << 'EOF'
. $args[0]
function T($name, $got, $want) { if ("$got" -eq "$want") { "ok $name" } else { "bad ${name}: '$got', expected '$want'" } }
$err = @(
  'DirectML: C:\Program Files\rekordbox\rekordbox 7.2.19\DirectML.dll version 1.13.1',
  'rbsp-timing chunk at=0 chunk=0 version=0 kind=full extract_ns=1 key_ns=2 model_ns=300000000 ort_ns=250000000 post_ns=3 write_ns=4 chunk_ns=320000000',
  'rbsp-timing chunk at=0 chunk=1 version=0 kind=full extract_ns=1 key_ns=2 model_ns=500000000 ort_ns=450000000 post_ns=3 write_ns=4 chunk_ns=520000000',
  'rbsp-timing chunk at=0 chunk=2 version=0 kind=full extract_ns=1 key_ns=2 model_ns=400000000 ort_ns=350000000 post_ns=3 write_ns=4 chunk_ns=420000000',
  'rbsp-timing track at=0 ext=flac chunks=3 decode_ns=1 resample_ns=0 start_ns=1 end_ns=2 power=ac ep=dml',
  '{"file":"x"}')
$s = Get-BenchSummary $err
T 'chunks' $s.Chunks 3; T 'ort mean' $s.OrtMs 350; T 'ort median' $s.OrtMedianMs 350; T 'model mean' $s.ModelMs 400; T 'chunk mean' $s.ChunkMs 420
T 'ep' $s.Ep 'dml'; T 'DirectML lines' $s.DirectML.Count 1
$f = Get-BenchSummary @('DirectML: x: the model runs on the CPU', 'rbsp-timing track at=0 ext=wav chunks=0 decode_ns=1 resample_ns=0 start_ns=1 end_ns=2 power=ac ep=cpu')
T 'fallback ep' $f.Ep 'cpu'; T 'fallback no chunks' $f.Chunks 0; T 'fallback mean' "$($f.OrtMs)" ''
T 'both eps' (Get-BenchSummary @('rbsp-timing track ep=dml', 'rbsp-timing track ep=cpu')).Ep 'cpu+dml'
T 'deck elapsed' (Get-DeckSeconds '-01:23.4 00:36.6') 36.6
T 'deck elapsed first' (Get-DeckSeconds '01:02.5 -00:57.5') 62.5
T 'deck OCR slips' (Get-DeckSeconds '-01:23,4  00.36,6') 36.6
T 'deck none' "$(Get-DeckSeconds 'Not Loaded.')" ''
T 'deck oo' (Get-DeckSeconds '-00:30.0 oo:oo.o') 0
$a = Get-Advance @(@(0, 10.0), @(2, 12.0), @(4, $null), @(6, 16.1), @(8, 16.3), @(10, 18.3))
T 'advance read' $a.Read 5; T 'advance stalls' $a.Stalls 1; T 'advance worst' $a.Worst 0.1; T 'advance samples' $a.Samples 6
$b = Get-Advance @(@(0, 5.0), @(2, 7.0))
T 'steady' "$($b.Stalls) $($b.Worst)" '0 1'
T 'nothing read' "$((Get-Advance @(@(0, $null), @(2, $null))).Read)" '0'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/bench-test.ps1" "$here/kit/bench-lib.ps1" > "$tmp/bt" 2>&1
  n="$(grep -c '^ok ' "$tmp/bt")"; pass=$((pass + n))
  [ "$n" = 22 ] && ! grep -qv '^ok ' "$tmp/bt" || { no "bench-lib ($n of 22 OK):"; grep -v '^ok ' "$tmp/bt" | sed 's/^/    /'; }
  # midi-auto's Bench-Run (taken from the script itself): its return is the run's exit code only,
  # whatever it says and its tick prints on the way (the PC, 2026-11-19: the lines came back too)
  cat > "$tmp/benchrun-test.ps1" << 'EOF'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$null, [ref]$null)
$fn = $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $args[0].Name -eq 'Bench-Run' }, $true)[0]
. ([scriptblock]::Create($fn.Extent.Text))
$BenchDir = Join-Path $args[1] 'bench'; $Do = 'dmlbench'
function Say([string]$s) { "said: $s" }
function Session($c, $w, $a) {
    if ($c -eq 'benchstop') { $script:stopped++; return 'x benchstop: 1 stopped, 0 left' }
    if ($script:mode -eq 'refused') { return 'x benchstart: no bench\rbsp-prepare.exe' }
    if ($script:mode -eq 'slow') { return "x benchstart: $a started (pid 5)" }
    Set-Content -LiteralPath (Join-Path $BenchDir "$a.exit") -Value 7; "x benchstart: $a started (pid 5)"
}
function T($name, $got, $want) { $g = @($got); if ($g.Count -eq 1 -and "$($g[0])" -eq "$want") { "ok $name" } else { "bad ${name}: $($g -join ' | ')" } }
$bp = [pscustomobject]@{ Model = 'm'; Ort = 'o'; Rb = 'r' }
$script:mode = ''
T 'exit code only' (Bench-Run 'cpu' @('a.wav') (Join-Path $args[1] 'c') $bp 30 $null) 7
T 'with a tick' (Bench-Run 'stall' @('a.wav') (Join-Path $args[1] 'c') $bp 30 { 'tick output' }) 7
$script:mode = 'refused'
T 'not started' (Bench-Run 'dml' @('a.wav') (Join-Path $args[1] 'c') $bp 30 $null) 98
$script:mode = 'slow'; $script:stopped = 0
T 'past its time' (Bench-Run 'stall' @('a.wav') (Join-Path $args[1] 'c') $bp 1 $null) 97
T 'past its time: stopped' $script:stopped 1
$spec = @(Get-Content -LiteralPath (Join-Path $BenchDir 'spec-stall.txt'))
T 'stall spec' ($spec -join ' ') ('--model m --cache ' + (Join-Path $args[1] 'c') + ' --kept-dir ' + (Join-Path $BenchDir 'kept-stall') + ' --no-keep --ort o --rekordbox r --dml a.wav')
EOF
  mkdir -p "$tmp/br"; pwsh -NoProfile -NonInteractive -File "$tmp/benchrun-test.ps1" "$here/kit/midi-auto.ps1" "$tmp/br" > "$tmp/brt" 2>&1
  n="$(grep -c '^ok ' "$tmp/brt")"; pass=$((pass + n))
  [ "$n" = 6 ] && ! grep -q '^bad ' "$tmp/brt" || { no "Bench-Run ($n of 6 OK):"; cat "$tmp/brt" | sed 's/^/    /'; }
  cat > "$tmp/say-stream.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$null, [ref]$null)
$fn = $ast.Find({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $args[0].Name -eq 'Say' }, $true)
. ([scriptblock]::Create($fn.Extent.Text))
$Log = $args[1]
$returned = @(Say 'diagnostic')
if ($returned.Count -or -not (Select-String -LiteralPath $Log -SimpleMatch 'diagnostic')) { exit 1 }
EOF
  if pwsh -NoProfile -NonInteractive -File "$tmp/say-stream.ps1" "$here/kit/kit-lib.ps1" "$tmp/say.log" > /dev/null; then ok
  else no 'Say diagnostic polluted the success stream'; fi

  # kit-lib's Assert-SystemDrivePath: the kit writes only to the system drive (never the owner's
  # backup disk), here with / as the system drive's root
  cat > "$tmp/sysdrive-test.ps1" << 'EOF'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($args[0], [ref]$null, [ref]$null)
$fn = $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $args[0].Name -eq 'Assert-SystemDrivePath' }, $true)[0]
. ([scriptblock]::Create($fn.Extent.Text))
function T($name, [scriptblock]$b, [string]$want) { try { $g = & $b } catch { $g = 'throw ' + $_.Exception.Message }; if ($g -like $want) { "ok $name" } else { "bad ${name}: $g" } }
$env:SystemDrive = '/tmp'
T 'on the system drive' { Assert-SystemDrivePath '/tmp/rbst/bench/cache-cpu' } '/tmp/rbst/bench/cache-cpu'
T 'another drive' { Assert-SystemDrivePath '/Volumes/Backups/cache' } 'throw refused: /Volumes/Backups/cache is not on the system drive /tmp/*'
T 'a prefix that is not the drive' { Assert-SystemDrivePath '/tmpx/cache' } 'throw refused:*'
T 'a climb out of it' { Assert-SystemDrivePath '/tmp/../Volumes/x' } 'throw refused: /Volumes/x*'
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/sysdrive-test.ps1" "$here/kit/kit-lib.ps1" > "$tmp/sdt" 2>&1
  n="$(grep -c '^ok ' "$tmp/sdt")"; pass=$((pass + n))
  [ "$n" = 4 ] && ! grep -qv '^ok ' "$tmp/sdt" || { no "Assert-SystemDrivePath ($n of 4 OK):"; sed 's/^/    /' "$tmp/sdt"; }
  # An auto plan's preconditions (pre-lib.ps1): what each plan puts right itself, what stops it.
  cat > "$tmp/pre-test.ps1" << 'EOF'
. $args[0]
function T($name, [string[]]$got, [string]$want) { $g = $got -join ' # '; if ($g -like $want) { "ok $name" } else { "bad ${name}: '$g'" } }
function F([hashtable]$o) { $f = @{ Session = $true; Ort = 'pioneer'; Probe = $false; AutoAnalysis = '0'; Model = 'ours'; App = 'x appstate: not running'; RbRunning = $false; Server = 'up https://mac.test:8766'; SystemDrive = 'C:'; CacheDir = 'C:\Users\t\AppData\Local\rbstemsplus\Cache' }; foreach ($k in $o.Keys) { $f[$k] = $o[$k] }; $f }
$v = Get-PlanVerdict (Get-PlanFindings 'session2' (F @{}))
T 'clean' $v 'preconditions: all met (0 put right by the plan itself) # 0'
T 'clean lines' (Get-PlanFindings 'prepare' (F @{})) 'OK *# OK *# OK *# OK *# OK *# OK our app not running*# OK rekordbox closed'
$s2 = F @{ AutoAnalysis = '1'; Model = 'rekordbox'; App = 'x appstate: running pid 5; minimized True; tabs 0: ' }
T 'session2 fixes' (Get-PlanVerdict (Get-PlanFindings 'session2' $s2)) 'preconditions: all met (3 put right by the plan itself) # 0'
T 'black app' (Get-PlanFindings 'prepare' $s2) "*FIX our app running, minimized, no tabs in UI Automation (its window drawn black?)*"
T 'record needs ours' (Get-PlanVerdict (Get-PlanFindings 'record' $s2)) "preconditions: 1 failed: the stems model: rekordbox's own; record's T2 and hits need Demucs v4*# 1"
T 'dumps notes' (Get-PlanVerdict (Get-PlanFindings 'dumps' $s2)) 'preconditions: all met (0 put right by the plan itself) # 0'
T 'auto analysis noted' (Get-PlanFindings 'dumps' $s2) "*NOTE rekordbox's Auto Analysis on (only session2 needs it off)*"
$all = F @{ Session = $false; Ort = 'other 33d96af6'; Probe = $true; AutoAnalysis = '?'; Model = 'missing'; App = '' }
$va = Get-PlanVerdict (Get-PlanFindings 'session2' $all)
T 'every failure in one list' $va "preconditions: 5 failed: the test account's session task does not answer* | rekordbox's onnxruntime.dll is not Pioneer's (other 33d96af6)* | the research probe's C:\rbtest is there* | rekordbox's Auto Analysis not read* | rekordbox's model file hdemucs.onnx is missing # 1"
T 'app unread' (Get-PlanVerdict (Get-PlanFindings 'prepare' (F @{ App = '' }))) "preconditions: 1 failed: our app's state not read # 1"
T 'app odd' (Get-PlanVerdict (Get-PlanFindings 'prepare' (F @{ App = 'x appstate: no answer' }))) "preconditions: 1 failed: our app's state not understood* # 1"
T 'app fine' (Get-PlanFindings 'prepare' (F @{ App = 'x appstate: running pid 5; minimized False; tabs 2: Manage, Prepare' })) '*OK our app running, its tabs in UI Automation*'
T 'rekordbox running' (Get-PlanFindings 'record' (F @{ RbRunning = $true })) '*NOTE rekordbox already running*'
T 'server down' (Get-PlanVerdict (Get-PlanFindings 'prepare' (F @{ Model = 'rekordbox'; Server = 'down https://mac.test:8766' }))) "preconditions: 1 failed: the stems model: rekordbox's own, and Demucs v4's install downloads from the Mac's TEST server, which doesn't answer (https://mac.test:8766): start it first # 1"
T 'cache on another drive' (Get-PlanVerdict (Get-PlanFindings 'record' (F @{ CacheDir = 'F:\RB Stems Plus Cache' }))) "preconditions: 1 failed: the Stems Cache folder F:\RB Stems Plus Cache is not on the system drive C:* # 1"
T 'cache unknown' (Get-PlanVerdict (Get-PlanFindings 'record' (F @{ CacheDir = '' }))) 'preconditions: all met* # 0'
T 'server unknown' (Get-PlanVerdict (Get-PlanFindings 'session2' (F @{ Model = 'rekordbox'; Server = '' }))) '*its address not in the app*s log*# 1'
T 'bench helper left' (Get-PlanVerdict (Get-PlanFindings 'prepare' (F @{ Bench = 1 }))) "preconditions: 1 failed: 1 of the kit's bench helpers still running (they hold our app's ORT): pc-session.sh run benchstop first # 1"
T 'bench helpers uncounted' (Get-PlanVerdict (Get-PlanFindings 'session2' (F @{ Bench = -1 }))) "preconditions: 1 failed: the kit's bench helpers not counted # 1"
T 'no bench helper' (Get-PlanVerdict (Get-PlanFindings 'record' (F @{ Bench = 0 }))) 'preconditions: all met* # 0'
# OI-80: preflight reads whether rekordbox's profile asks for its Analysis Setting dialog
T 'ask analysis off' (Get-RbAskAnalysisRow '<VALUE name="AutoAnalysis" val="0"/>  <VALUE name="askAnalysisSettingWindow" val="0"/>') "True # rekordbox's profile asks for no Analysis Setting dialog (askAnalysisSettingWindow=0)"
T 'ask analysis missing' (Get-RbAskAnalysisRow '<VALUE name="AutoAnalysis" val="0"/>') "False # rekordbox's profile shows its Analysis Setting dialog on a Load (askAnalysisSettingWindow missing), where the kit stops: with rekordbox closed, set askAnalysisSettingWindow=0 in rekordbox3.settings*"
T 'ask analysis on' (Get-RbAskAnalysisRow '<VALUE name="askAnalysisSettingWindow" val="1"/>') '*False # *(askAnalysisSettingWindow=1)*'
T 'ask analysis twice' (Get-RbAskAnalysisRow '<VALUE name="askAnalysisSettingWindow" val="0"/><VALUE name="askAnalysisSettingWindow" val="1"/>') '*False # *askAnalysisSettingWindow is in rekordbox3.settings 2 times'
T 'settings unread' (Get-RbAskAnalysisRow '') "False # rekordbox's Analysis Setting dialog: rekordbox3.settings not read"
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/pre-test.ps1" "$here/kit/pre-lib.ps1" > "$tmp/pt" 2>&1
  n="$(grep -c '^ok ' "$tmp/pt")"; pass=$((pass + n))
  [ "$n" = 24 ] && ! grep -qv '^ok ' "$tmp/pt" || { no "pre-lib ($n of 24 OK):"; grep -v '^ok ' "$tmp/pt" | sed 's/^/    /'; }
  # Run the actual T5 phases against local storage stubs.
  cat > "$tmp/t5-test.ps1" << 'EOF'
param($kit, $scratch)
$ErrorActionPreference = 'Stop'
$env:USERPROFILE = $scratch
function T($name, $got, $want) {
    if ("$got" -ceq "$want") { "ok $name" } else { "bad ${name}: got '$got', expected '$want'" }
}
function D($n, $model = 'Samsung Portable SSD T5', $serial = 'T5SN', $bus = 'USB') {
    [pscustomobject]@{ Number = $n; FriendlyName = $model; SerialNumber = $serial; BusType = $bus; UniqueId = 'SHARED' }
}
function Get-Disk { $global:T5Inventory }
function Get-Partition {
    param($DiskNumber, $ErrorAction)
    if ($global:T5Unreadable) { throw 'unreadable' }
    [pscustomobject]@{ DriveLetter = 'D'; Label = $global:T5Labels[$DiskNumber] }
}
function Get-Volume {
    param([Parameter(ValueFromPipeline = $true)]$p)
    process { [pscustomobject]@{ FileSystemLabel = $p.Label } }
}
$global:T5HashCalls = 0
function Get-FileHash {
    param($LiteralPath, $Algorithm)
    $global:T5HashCalls++
    if ($global:T5SwapOnProbe) { $global:T5Inventory = @(D 2 -serial 'OTHER') }
    [pscustomobject]@{ Hash = 'bad-hash' }
}
function B64($s) { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s)) }
function Check($name, $phase, $code) {
    $Phase = $phase
    $global:LASTEXITCODE = 0
    & $kit > $null
    T $name $LASTEXITCODE $code
}
. $kit
# Map the drive letter to a temp directory for the folder operations.
$volume = Join-Path $scratch 't5-volume'
$null = New-Item -ItemType Directory $volume, (Join-Path $scratch 'rbsp-kit-admin')
$null = New-PSDrive -Name D -PSProvider FileSystem -Root $volume
$own = 'D:\rbsp-test'
$companion = 'D:\._rbsp-test'
Set-Content -LiteralPath $companion -Value 'outside our folder'
Set-Content -LiteralPath $Saved -Value @(Get-Listing 'D:\')
$TestUser = 'rbtest'
$rog = D 1 'ROG ESD-S1C' 'TESTSERIAL01'
$t5 = D 2
$T5Serial = 'T5SN'
$global:T5Labels = @{ 1 = 'backup'; 2 = 'T5' }
$rogValue = B64 'TESTSERIAL01|ROG ESD-S1C'
$Protected = @($rogValue)
$global:T5Inventory = @($rog, $t5)
Check 'T5 accepts a matching drive beside a shared UniqueId' check 0
$global:T5Inventory = @(D 2 -serial 'OTHER')
Check 'stage rejects a same-model substitute with a different serial' check 3
$global:T5Inventory = @(D 2 -model 'Other')
Check 'stage rejects a different model with the pinned serial' check 3
$global:T5Inventory = @(D 2 -bus 'SATA')
Check 'stage rejects the pinned drive on a different bus' check 3
$global:T5Inventory = @($t5, (D 3))
Check 'stage rejects an ambiguous T5' check 3
$global:T5Inventory = @($t5)
$T5Serial = ''
Check 'stage requires a pinned serial' check 4
$T5Serial = 'T5SN'
$global:T5Labels[2] = 'Acronis backups'
Check 'rows refuse an Acronis volume' rows 4
T 'rows did not start on an Acronis volume' $global:T5HashCalls 0
$global:T5Labels[2] = 'T5'
$global:T5Unreadable = $true
T 'volumes that cannot be read are refused' (Test-Refused $t5) "its volumes can't be read (built-in refusal)"
$global:T5Unreadable = $false
T 'the ROG is refused independently of its label and protection list' (Test-HardRefused $rog) 'the ROG ESD-S1C (built-in refusal)'
$Protected = @($rogValue, (B64 'T5SN|Samsung Portable SSD T5'))
Check 'a T5 in the second protected slot is refused' finish 4
$Protected = @()
Check 'remote prep refuses an empty list' prep 4
$Protected = @($rogValue, 'invalid!')
Check 'remote finish refuses a malformed list' finish 4
$Protected = @($rogValue)
$system = D 3
$system | Add-Member -NotePropertyName IsSystem -NotePropertyValue $true
T 'a system disk is refused' (Test-Refused $system) 'a boot or system disk'
T 'the ROG protection value uses the serial and model' (Get-Key $rog) 'VEVTVFNFUklBTDAxfFJPRyBFU0QtUzFD'

Check 'prep creates the kit folder' prep 0
T 'prep created the marker' (Test-Path -LiteralPath (Join-Path $own $Marker)) $true
Set-Content -LiteralPath (Join-Path $own $Marker) -Value 'existing marker'
Check 'prep can be repeated' prep 0
T 'repeat prep leaves the existing marker untouched' (Get-Content -LiteralPath (Join-Path $own $Marker)) 'existing marker'
$before = Get-Listing 'D:\'
Set-Content -LiteralPath (Join-Path $own 'test.txt') -Value 'our file'
T 'the listing excludes the kit folder' (Get-Sha (Get-Listing 'D:\')) (Get-Sha $before)
Check 'finish removes the kit folder' finish 0
T 'finish leaves an adjacent file alone' (Get-Content -LiteralPath $companion) 'outside our folder'
$null = New-Item -ItemType Directory $own
Check 'finish refuses an unmarked folder' finish 4
Remove-Item -LiteralPath $own
$null = New-Item -ItemType SymbolicLink -Path $own -Target $scratch
Set-Content -LiteralPath (Join-Path $scratch $Marker) -Value 'outside our folder'
Check 'prep refuses a linked kit folder' prep 4
Remove-Item -LiteralPath $own
$global:T5SwapOnProbe = $true
Check 'rows refuse to clean up after the drive changes' rows 3
T 'rows leave the folder alone after the drive changes' (Test-Path -LiteralPath (Join-Path $own $Marker)) $true
Remove-Item -LiteralPath $own -Recurse -Force
Remove-Item -LiteralPath $companion -Force
$global:T5Inventory = @($t5)
Check 'prep works on an empty volume' prep 0
Check 'finish compares empty listings' finish 0
EOF
  USERPROFILE="$tmp" pwsh -NoProfile -NonInteractive -File "$tmp/t5-test.ps1" "$here/kit/t5-app.ps1" "$tmp" > "$tmp/t5t" 2>&1
  n="$(grep -c '^ok ' "$tmp/t5t")"; pass=$((pass + n))
  [ "$n" = 28 ] && ! grep -qvE '^(ok |T5: refused:|T5: expected )' "$tmp/t5t" || { no "t5 phases ($n of 28 OK):"; sed 's/^/    /' "$tmp/t5t"; }
  # chosen.ps1: CORE_STEP_SET_CHOSEN's bytes (state_chosen_text), the model flag kept, put back at cleanup
  ch="$tmp/chosen"; mkdir -p "$ch/la/rbstemsplus" "$ch/tmp"
  chosen() { LOCALAPPDATA="$ch/la" TEMP="$ch/tmp" pwsh -NoProfile -NonInteractive -File "$here/kit/chosen.ps1" -Mode "$1" > /dev/null 2>&1; }
  printf 'model=1\ncache=0\n' > "$ch/la/rbstemsplus/installed"
  chosen set && [ "$(od -An -c "$ch/la/rbstemsplus/installed" | tr -d ' \n')" = 'model=1\ncache=1\n' ] && ok || no "chosen set: not state_chosen_text's bytes"
  chosen restore && [ "$(cat "$ch/la/rbstemsplus/installed")" = "$(printf 'model=1\ncache=0')" ] && [ ! -e "$ch/tmp/rbst-kit-chosen.txt" ] && ok || no "chosen restore: not put back"
  rm "$ch/la/rbstemsplus/installed"; chosen set && chosen restore && [ ! -e "$ch/la/rbstemsplus/installed" ] && ok || no "chosen restore: the kit's file left behind"
  chosen set; printf 'model=0\ncache=0\n' > "$ch/la/rbstemsplus/installed"; chosen restore && [ "$(cat "$ch/la/rbstemsplus/installed")" = "$(printf 'model=0\ncache=0')" ] && ok || no "chosen restore: overwrote the app's own change"
  # Stub Windows interfaces, retaining the M12 checks and their ordering.
  cat > "$tmp/m12.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$Kit = $args[0]; $B = $env:TEMP; $env:USERPROFILE = $B
. (Join-Path $Kit 'm12.ps1')
function Case($name, [scriptblock]$code, $expected) {
    $actual = try { & $code } catch { $_.Exception.Message }
    if ($actual -like $expected) { "ok $name" } else { "bad ${name}: $actual" }
}
function M12-Desktop {}
function Start-Sleep {}
function Start-Process { $script:launched++; return @{ Id = 41 } }
function Get-DesktopWindows {
    if ($script:launched) { $script:window }
}
# A named -Stream parameter is needed for the alternate stream check.
function Get-Content([string]$LiteralPath, [string]$Stream, [switch]$Raw) {
    if ($Stream -eq 'Zone.Identifier') { "ZoneId=$script:zone"; return }
    if ($LiteralPath -like '*m12-kept.txt') { Join-Path $B 'rbsp-uninstall-0123456789abcdef'; return }
    'a' * 64
}
function M12-Sha { 'a' * 64 }
$script:window = 'smartscreen | Windows protected your PC | 600x400 | 0x11'
$script:zone = 3; $script:launched = 0
Case 'SmartScreen observed' { M12-SmartScreen 'fixture.exe' } 'SmartScreen observed; smartscreen | Windows protected your PC*'
$script:zone = 0; $script:launched = 0
Case 'SmartScreen zone' { M12-SmartScreen 'fixture.exe'; $script:launched } 'fixture is missing the browser download zone'
if (-not $script:launched) { 'ok SmartScreen not launched' }
$script:zone = 3; $script:window = 'consent | UAC | 500x400 | 0x14'
Case 'unexpected security window' { M12-SmartScreen 'fixture.exe' } 'SmartScreen not seen before a new window: consent*'
function M12-Ui { '  taskbar: count=2 name=RB Stems Plus' }
Case 'two groups' { M12-Group } 'expected one taskbar group:*'
function M12-Payload($slot) {
    @{ App = '1.5.0'; Release = $(if ($slot -eq 'old') { '1.5.0' } else { '1.5.1' }) }
}
function M12-Installed { $true }
function M12-Served {}
function M12-App { @{ Id = 50 } }
function M12-Close {}
function Get-ChildItem { @{ Name = '1.5.0' } }
function Get-Item { @{ LastWriteTime = (Get-Date) } }
function Wait-M12($seconds, [scriptblock]$condition, $failure) { if (-not (& $condition)) { throw $failure } }
function M12-Ui { $script:ui }
$script:ui = 'RB Stems Plus 1.5.1 is available: choose Update'
Case 'carried must not offer' { M12-Update 'mac:8766' } 'carried payload offered an app update'
$script:ui = 'RB Stems Plus'
Case 'carried version' { M12-Update 'mac:8766' } 'carried 1.5.0 -> 1.5.1; same app 1.5.0'
# Keep the pin sequence intact and deliberately expose a shortcut to a versioned exe.
function M12-Ui([Parameter(ValueFromRemainingArguments = $true)][string[]]$argv) {
    $script:pinActions += $argv[1]
    if ($argv[1] -eq 'count') {
        if ($script:pinActions -contains 'unpin') { 'taskbar: count=0 name=RB Stems Plus' }
        else { 'taskbar: count=1 name=RB Stems Plus' }
    }
}
function New-Object([string]$ComObject) {
    $shell = [pscustomobject]@{}
    $shell | Add-Member ScriptMethod CreateShortcut { @{ TargetPath = $script:pinTarget } }
    return $shell
}
function Get-ChildItem { @{ FullName = 'RB Stems Plus.lnk' } }
function M12-App { $script:pidCounter++; @{ Id = $script:pidCounter } }
$script:pidCounter = 40; $script:pinActions = @(); $script:pinTarget = M12-VersionExe '1.5.0'
Case 'pin target' { M12-Pin } 'persisted pin does not uniquely target the stable stub'
$script:pinActions = @(); $script:pinTarget = $M12Stub
Case 'pin launch' { M12-Pin } 'persisted shell pin launched the stable stub; one taskbar group'
if (($script:pinActions -join ' ') -eq 'count pin click count unpin count') { 'ok pin shell order' }
function Get-OrtState { 'pioneer' }
function M12-Apps {}
function M12-TempCopies {}
function M12-RunOnceValue { 'pending cleanup' }
Case 'RunOnce pending' { M12-Settings $false } 'RunOnce did not remove the kept round temp copy at logon'
EOF
  mkdir -p "$tmp/m12"
  TEMP="$tmp/m12" LOCALAPPDATA="$tmp/m12" APPDATA="$tmp/m12" pwsh -NoProfile -NonInteractive -File "$tmp/m12.ps1" "$here/kit" > "$tmp/m12-out" 2>&1
  for w in 'SmartScreen observed' 'SmartScreen zone' 'SmartScreen not launched' 'unexpected security window' 'two groups' 'pin target' 'pin launch' 'pin shell order' 'carried must not offer' 'carried version' 'RunOnce pending'; do
    grep -qxF "ok $w" "$tmp/m12-out" && ok || { no "M12: $w"; sed 's/^/    /' "$tmp/m12-out"; }
  done
  cat > "$tmp/t2.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
$B = $env:TEMP
. (Join-Path $args[0] 't2-round2.ps1')
function Check-Throws($name, [scriptblock]$action, $prefix) {
    try { & $action }
    catch { if ($_.Exception.Message -like "$prefix*") { "ok $name"; return }; throw }
    "bad $name"
}
function Say($s) { $s }
function Get-Rb { @{ Version = $script:version; Dir = $B; Exe = (Join-Path $B 'rekordbox.exe') } }
function Get-FileHash { @{ Hash = $script:exeHash } }
function Get-Content([string]$LiteralPath, [switch]$Raw) {
    if ($LiteralPath -like '*rekordbox3.settings') {
        "<VALUE name=`"PartAnalysisQuality`" val=`"0`"/><VALUE name=`"AutoAnalysis`" val=`"$script:auto`"/>"
    } else { $script:probe }
}
function Copy-Item { 'evidence copied' }
$script:auto = 1
Check-Throws 'auto analysis' { Get-T2Facts } 'turn off Auto Analysis'
Check-Throws 'ambiguous quality' { Read-T2Setting 'name="PartAnalysisQuality" val="0" name="PartAnalysisQuality" val="1"' 'PartAnalysisQuality' } 'rekordbox setting'
$script:auto = 0
$env:NUMBER_OF_PROCESSORS = '12'
foreach ($name in 'libmpg123.dll', 'other.dll') { [IO.File]::WriteAllText((Join-Path $B $name), 'fixture') }
function Get-Item { @{ VersionInfo = @{ FileVersion = '1.2.3' } } }
function Copy-Item([string[]]$LiteralPath, [string]$Destination, [switch]$Force) {
    if ($Destination -like '*rb-orig') { "copied $([IO.Path]::GetFileName($LiteralPath[0]))" }
    else { 'evidence copied' }
}
$script:version = '7.2.19'
$script:exeHash = 'c9ed23e974c51c75e2ec069fbd2679a3dbe606be9e79e9cbbd13d6418ab11c37'
Get-T2Facts
$script:version = '7.2.20'
$script:exeHash = '37d2b6a57770a42d9604d8872b1d8dae4f1fffa15483189a92ba58f9dd4de9ef'
$facts720 = @(Get-T2Facts)
if ($facts720 -contains 'T2 item 2: rekordbox 7.2.20 DLLs' -and $facts720 -contains 'T2 item 2: known rekordbox executable hash: True') { 'ok T2 7.2.20' }
$script:version = '7.2.21'
Check-Throws 'unsupported T2 install' { Get-T2Facts } 'T2 inventory expects rekordbox'
$script:probe = 'MODEL 01234 model pioneer'
Check-Throws 'model evidence' { Get-T2ProbeEvidence } 'the probe never recorded'
$script:probe = @('MODEL 01234 model ours', 'AppendExecutionProvider_DML device=2', 'SetIntraOpNumThreads(8)', 'SetInterOpNumThreads(1)')
Get-T2ProbeEvidence
EOF
  TEMP="$tmp/m12" APPDATA="$tmp/m12" pwsh -NoProfile -NonInteractive -File "$tmp/t2.ps1" "$here/kit" > "$tmp/t2-out" 2>&1
  for w in 'ok auto analysis' 'ok ambiguous quality' 'T2 DLL: other.dll' 'T2 item 2: libmpg123.dll file version 1.2.3' 'T2 item 2: libfaad2.dll absent' 'copied libmpg123.dll' 'T2 item 6: NUMBER_OF_PROCESSORS=12' 'ok T2 7.2.20' 'ok unsupported T2 install' 'ok model evidence' 'T2 probe: AppendExecutionProvider_DML device=2' 'T2 probe: SetIntraOpNumThreads(8)' 'T2 probe: SetInterOpNumThreads(1)' 'evidence copied'; do
    grep -qxF "$w" "$tmp/t2-out" && ok || { no "T2: $w"; sed 's/^/    /' "$tmp/t2-out"; }
  done
  # midi-auto.ps1's quit and run loops, as written, in a folder of stub libraries (match-lib and
  # watch-lib real): presses logged; a model of the list (Up/Down/SortSong/Load) gives deck 1's
  # title; Session answers with rekordbox's window (and a dialog once one "appears"). Sleeps advance
  # the same simulated clock that the bridge deadlines read. SIM_EV: KEY=ACTION,... where KEY is a
  # press and its count (Load#2), In (quit's MIDI input opened) or said~TEXT (a line said), and ACTION
  # dialog (a new window), stop (midi-stop), write (a bridge line on each sleep for 10 simulated s),
  # lose (that press lost), diagnostic, near or silent (what Load logs), unreadable (the
  # deck OCR can't match the track); desc: the list starts Z to A
  k="$tmp/mk"; mkdir -p "$k/corpus/v1"; cp "$here/kit/midi-auto.ps1" "$here/kit/match-lib.ps1" "$here/kit/watch-lib.ps1" "$here/kit/format-lib.ps1" "$here/kit/format-run.ps1" "$here/kit/timing-lib.ps1" "$k/"
  cp "$here/kit/winlist.ps1" "$tmp/winlist-win32.cs" "$k/"
  cp "$here/kit/kit-lib.ps1" "$k/kit-lib-real.ps1"
  touch "$k/corpus/v1/alpha-one.mp3" "$k/corpus/v1/bravo-two.mp3" "$k/corpus/v1/charlie-three.mp3"
  cat > "$k/kit-lib.ps1" << 'EOF'
. (Join-Path $PSScriptRoot 'kit-lib-real.ps1')
$Kit = $PSScriptRoot; $B = Join-Path $env:SIM 'rbst'; [void][IO.Directory]::CreateDirectory((Join-Path $B 'shots'))
$Log = Join-Path $B 'midi-auto.log'; $AppLogs = $env:SIM; $Bridge = Join-Path $env:SIM 'bridge.log'; $ProbeRows = Join-Path $env:SIM 'rows.txt'; $TaskName = 't'; $Mapping = Join-Path $env:SIM 'map.csv'
if ($Do -eq 'format-hits') { $ProbeRows = Join-Path $env:SIM 'probe/rows.txt' }
function Get-Rb { @{ Dir = $env:SIM; Exe = (Join-Path $env:SIM 'rekordbox.exe'); Version = '7.2.19' } }
function Get-OrtState { 'other 1234abcd' }
function Assert-RbProcessVersion { }
$script:now = [datetime]'2026-11-18T10:00:00'; $script:writeUntil = $script:now; $script:testWindow = $false
$script:simStart = $Do -eq 'start' -and $env:SIM_EV -match '\bformat-start\b'
$script:browserReadyAt = $script:now.AddSeconds($(if ($env:SIM_EV -match '\bslow-browser\b') { 30 } else { 3 }))
$script:simUpdateOffer = $env:SIM_EV -in 'updateboot', 'updatequit'; $script:simQuit = $false
$script:simInfo = $env:SIM_EV -eq 'updatequit'   # rekordbox's news window open beside the offer
$script:fronts = 0
$script:simAnalysis = $env:SIM_EV -eq 'quit-analysis'
$crash = 'rekordbox #32770 message box: 0x41: Unexpected application error'
if ($env:SIM_EV -match '\bprivate-crash\b') {
    Add-Type -Path (Join-Path $Kit 'winlist-win32.cs')
    . (Join-Path $Kit 'winlist.ps1')
    function Initialize-RbspWin { }
    [RbspWin]::CheckMessageBox = $true
    [RbspWin]::StaticText = 'Could not load Private Song.'
    function Uia { throw 'UI Automation is unavailable' }
    function Get-SimCrashReply {
        'rekordbox #32770 message box: ' + (@(Get-RbMessageBoxes 7) -join '; ')
    }
} else {
    function Get-SimCrashReply { $crash }
}
function Ev([string]$key) {
    $r = ''
    foreach ($e in @($env:SIM_EV -split ',' | Where-Object { $_ -match '=' })) {
        $kk, $a = $e -split '=', 2
        if (-not ($kk -eq $key -or ($kk.StartsWith('said~') -and $key.StartsWith('said:') -and $key.Contains($kk.Substring(5))))) { continue }
        switch ($a) {
            'crash' { $script:simCrash = $true }
            'analysis-dialog' { $script:simAnalysis = $true }
            'dialog' { [IO.File]::WriteAllText((Join-Path $env:SIM 'dialog'), '') }
            'secret-dialog' { [IO.File]::WriteAllText((Join-Path $env:SIM 'dialog'), 'Secret Title') }
            'late-hit' { Add-Content -LiteralPath $Bridge -Value '2026-11-18 10:00:10  hit  0123456789ab (441000 samples)  [1 hits, 0 near, 0 misses]' }
            'stop' { [IO.File]::WriteAllText((Join-Path $B 'midi-stop'), '') }
            'expire' { $script:now = $script:now.AddMinutes(36) }
            'exit-rb' { $script:rbGone = $true }
            'exit-on-window-list' { $script:exitOnWindowList = $true }
            'write' { $script:writeUntil = $script:now.AddSeconds(10) }
            'lose' { $r = 'lose' }
            { $_ -in 'diagnostic', 'near', 'silent', 'unreadable', 'pad', 'rotate', 'truncate', 'late', 'late-unreadable', 'exit-settle' } { $r = $a }
        }
    }
    return $r
}
function Say([string]$s) {
    Write-Host $s
    Add-Content -LiteralPath $Log -Value $s
    if ($script:cacheTailAt -and $env:SIM_EV -match '\bcache-delay-' -and $s -match '^formats: .* CACHE_HIT ') {
        Add-Content -LiteralPath (Join-Path $env:SIM 'cache-margin-ms') -Value ([int]((Get-Date) - $script:cacheTailAt).TotalMilliseconds)
    }
    [void](Ev "said:$s")
}
function Session([string]$cmd, [int]$wait = 30, [string]$arg = '') {
    if ($cmd -eq 'shot') { $p = Join-Path $B ('shots/' + [guid]::NewGuid() + '.png'); [IO.File]::WriteAllText($p, ''); return "10:00:00 shot $p" }
    switch ($cmd) {
        'ping' { if ($env:SIM_EV -eq 'noping') { 'no answer' } else { "10:00:00 ping: rbtest's session 7" } }
        'start' {
            if ($script:simStart) {
                $script:simStarted = $true
                [RbspMidi2]::First = Get-Date
            }
            if ($env:SIM_EV -eq 'start-crash') { $script:simCrash = $true }
            if ($env:SIM_EV -eq 'watcher-start') {
                [RbspMidi2]::First = Get-Date
                '10:00:00 started rekordbox 7.2.19; RB Stems Plus watcher: pressed Remind Me Later'
            } else { '10:00:00 started rekordbox 7.2.19' }
        }
        'logoff' { '10:00:00 logoff: signing out' }
        'windows' {
            if ($script:simCrash -and $arg -in 'watch', 'watch-quit') { return "10:00:00 error: $(Get-SimCrashReply)" }
            if ($script:simAnalysis -and $arg -in 'watch', 'watch-quit') { return '10:00:00 error: rekordbox Analysis Setting dialog: 0x14052E' }
            if ($script:exitOnWindowList) { $script:rbGone = $true }
            if ($script:rbGone) { return '10:00:00 windows: ' }
            if ($script:simStart -and (Get-Date) -lt $script:browserReadyAt.AddSeconds(-2) -and
                $env:SIM_EV -notmatch '\bslow-browser\b') { return '10:00:00 windows: ' }
            $w = '10:00:00 windows: rekordbox | rekordbox | 1600x900 | 0x10'
            if ($script:simStart -and $env:SIM_EV -match '\bstartup-dialog\b') {
                $w += ' || rekordbox | private fixture caption | 500x400 | 0x40'
            }
            if ($script:simCrash) { $w += ' || rekordbox | rekordbox | 500x400 | 0x41' }
            if ((Test-Path -LiteralPath (Join-Path $env:SIM 'dialog')) -or $env:SIM_EV -match '\bprivate-window-at-start\b') {
                $caption = if ($env:SIM_EV -match '\bprivate-window\b') { 'private fixture | title 0x99' } else { 'Sign in' }
                $dialogPath = Join-Path $env:SIM 'dialog'
                if ([IO.File]::Exists($dialogPath) -and [IO.File]::ReadAllText($dialogPath)) {
                    $caption = [IO.File]::ReadAllText($dialogPath)
                }
                $w += " || rekordbox | $caption | 500x400 | 0x40"
            }
            if ($script:testWindow) { $w += ' || powershell | RBSP watch test | 480x200 | 0x50' }
            if ($script:simStart -and $env:SIM_EV -match '\blate-start-update\b' -and
                -not $script:simUpdateShown -and (Get-Date) -ge $script:browserReadyAt.AddSeconds(9)) {
                $script:simUpdateShown = $script:simUpdateOffer = $true
            }
            if ($script:simUpdateOffer) { $w += ' || Upmgr rekordbox | rekordbox update manager | 518x317 | 0x71' }
            if ($script:simInfo) { $w += ' || rekordbox | INFO | 816x647 | 0x72' }
            $w
        }
        'closeinfo' {
            if (-not $script:simInfo -or $arg -ne '0x72') { throw 'unexpected closeinfo' }
            $script:simInfo = $false
            '10:00:00 closeinfo: closed; 0x72 pid 7; before shot three.png; UI Automation closed the window; after shot four.png'
        }
        'closeagent' {
            if ($script:simStart) { "closeagent: $([int]((Get-Date) - [datetime]'2026-11-18T10:00:00').TotalMilliseconds) ms" }
            else { 'closeagent: ok' }
        }
        'closeupdate' {
            if (-not $script:simUpdateOffer -or $arg -ne '0x71') { throw 'unexpected closeupdate' }
            $script:simUpdateOffer = $false
            if ([RbspMidi2]::First -eq [DateTime]::MinValue) { [RbspMidi2]::First = Get-Date }
            '10:00:00 closeupdate: closed; before shot one.png; 7.2.19 -> 7.2.20; UI Automation invoked Close; after shot two.png'
        }
        'quit' {
            $script:simQuit = $true
            $script:closeAsked = Get-Date
            if ($env:SIM_EV -eq 'quit-crash') { $script:simCrash = $true }
            if ($env:SIM_EV -eq 'close-refused') {
                $script:rbGone = $true
                return '10:00:00 close asked: False'
            }
            '10:00:00 close asked: True'
        }
        'dismiss-rb-error' { Add-Content -LiteralPath (Join-Path $env:SIM 'presses') -Value 'dismiss-rb-error' }
        'quitdump' {
            if ($env:SIM_EV -cne 'close-timeout' -or -not $script:simQuit -or (Get-Date) -ne $script:closeAsked.AddSeconds(90)) {
                throw 'unexpected close dump'
            }
            $file = 'format-uidump-0123456789abcdef0123456789abcdef.jsonl'
            Set-Content -LiteralPath (Join-Path $B $file) -Value '{"Name":"<redacted>"}'
            "10:01:33 quitdump: OK $file"
        }
        'fmtimport' { '10:00:00 fmtimport: OK' }
        'format-verify' { '10:00:00 format-verify: OK' }
        'front' {
            # nofront: never in front; frontlater: in front for the header only
            $script:fronts++
            Add-Content -LiteralPath (Join-Path $env:SIM 'presses') -Value "front#$script:fronts"
            if ($script:simCrash) { return "10:00:00 front: $(Get-SimCrashReply)" }
            if ($script:simAnalysis) { return '10:00:00 front: rekordbox Analysis Setting dialog: 0x14052E' }
            if ($env:SIM_EV -eq 'nofront' -or ($env:SIM_EV -eq 'frontlater' -and $script:fronts -gt 1)) { "10:00:00 front: rekordbox's window 0x10 is not the foreground window" }
            else { "10:00:00 front: ok; our app's window(s) minimized: 1; rekordbox 0x10 in front" }
        }
        'testwindow' { $script:testWindow = $true; 'testwindow: ok' }
        'closetest' { $script:testWindow = $false; 'closetest: ok' }
        'app' { 'app: running' }
        'appexit' {
            Add-Content -LiteralPath (Join-Path $env:SIM 'presses') -Value 'appexit'
            if ($env:SIM_EV -eq 'appstays') { "10:00:00 appexit: still running 30 s after the menu's RB Stems Plus > Exit; 10:00:00 shot x.png" }
            else { "10:00:00 appexit: ok; closed by the menu's RB Stems Plus > Exit in 1 s" }
        }
        'prepare' { 'prepare: clicked' }
        'prepstop' { $script:prepStopped = $true; 'prepstop: ok' }
        'prepstate' {
            if ($script:prepStopped -and $env:SIM_EV -match '\bformat-stop-model\b') { 'prepstate: idle' }
            else { Start-Sleep -Seconds $wait; "prepstate: running (timeout $wait s)" }
        }
        default { "${cmd}: ok" }
    }
}
function Shot-Path([string]$answer) { $p = ($answer -split ' shot ', 2)[1]; if ($p -and (Test-Path -LiteralPath $p)) { return $p } else { return '' } }
function Test-RbRunning([Nullable[int]]$SessionId) {
    if ($null -ne $SessionId -and $SessionId -ne 7) { throw "wrong session $SessionId" }
    if ($script:rbGone) { return $false }
    if ($script:simStart) { return [bool]$script:simStarted }
    if ($env:SIM_EV -eq 'updatequit') { return -not $script:simQuit }
    $env:SIM_EV -notin 'owneronly', 'updateboot', 'silentstart', 'watcher-start', 'start-crash' -or $null -eq $SessionId
}
function Get-RbProcess([Nullable[int]]$SessionId) {
    if ($SessionId -ne 7) { return @{ SessionId = 2; Modules = @(@{ ModuleName = 'DirectML.dll' }) } }
    $pidInModel = if ($env:SIM_EV -match '\brestart-between-batches\b') { 456 } else { 123 }
    return @{ Id = $pidInModel; StartTime = [datetime]'2026-11-18T09:59:00Z'; SessionId = 7; Modules = @(@{ ModuleName = 'onnxruntime.dll' }) }
}
function Get-Date { $script:now }
function Start-Sleep([double]$Seconds = 0, [int]$Milliseconds = 0) {
    # The legacy replay uses the generic start path and skips the new probe focus pause.
    if ($Do -eq 'format-run' -and $Seconds -eq 2 -and $env:SIM_EV -match '\blegacy-focus\b') { return }
    $script:now = $script:now.AddMilliseconds($Seconds * 1000 + $Milliseconds)
    if ($env:SIM_EV -match '\bmeasure-clock\b') {
        [IO.File]::WriteAllText((Join-Path $env:SIM 'clock-ms'), [string][int]($script:now - [datetime]'2026-11-18T10:00:00').TotalMilliseconds)
    }
    if ($script:now -lt $script:writeUntil) { Add-Content -LiteralPath $Bridge -Value 'w' }
    if ($script:crashAt -and $script:now -ge $script:crashAt) { $script:simCrash = $true }
    if ($script:rbExitAt -and $script:now -ge $script:rbExitAt) { $script:rbGone = $true }
    if ($script:lateAt -and $script:now -ge $script:lateAt) {
        Add-Content -LiteralPath $Bridge -Value 'late bridge activity'
        $script:lateAt = $null
    }
    if ($script:chunkAt -and $script:now -ge $script:chunkAt) {
        Add-Content -LiteralPath $Bridge -Value $script:chunkLines
        $script:chunkAt = $null
    }
    while ($script:activityEvents.Count -and $script:now -ge $script:activityEvents.Peek().At) {
        Add-Content -LiteralPath $Bridge -Value $script:activityEvents.Dequeue().Line
    }
}
EOF
  cat > "$k/midi-lib.ps1" << 'EOF'
$RbspNote = @{ Down = 64; Load = 65; Up = 66; Play = 70; SortSong = 73; AreaLeft = 74; AreaRight = 75 }
Add-Type -TypeDefinition @'
using System; using System.Collections.Concurrent;
public static class RbspMidi2 {
  public static DateTime First = DateTime.UtcNow;
  public static ConcurrentQueue<Tuple<DateTime, int, int, int>> Got = new ConcurrentQueue<Tuple<DateTime, int, int, int>>();
  public static int[] Last = new int[128];
  public static uint midiOutClose(IntPtr h) { return 0; }
  public static uint midiInStop(IntPtr h) { return 0; }
  public static uint midiInClose(IntPtr h) { return 0; }
}
'@
[RbspMidi2]::Last[70] = 1   # deck 1 playing: quit presses Play unless it stops first
if ($env:SIM_EV -in 'updateboot', 'silentstart') { [RbspMidi2]::First = [DateTime]::MinValue }
$script:sel = if ($env:SIM_EV -match '\bbottom\b') { 171 } else { 0 }; $script:desc = @($env:SIM_EV -split ',') -contains 'desc'; $script:presses = @{}; $script:deckOcr = ''
$script:treeSel = if ($env:SIM_EV -match '\bdeep-tree\b') { 168 } else { 0 }
$script:simReadyTargets = @{}
if ($env:SIM_EV -match '\bprivate-deck\b') { $script:deckOcr = 'private fixture title' }
function Open-RbspOut { [IntPtr]::Zero }
function Open-RbspIn { [void](Ev 'In'); [IntPtr]::Zero }
function Ensure-RbspLoopback { 'loopback ready' }
$importedPlaylist = if ($env:SIM_EV -match '\bformat-model\b') {
    (Get-Content -LiteralPath (Join-Path $B 'format-import-state.json') -Raw | ConvertFrom-Json).playlist
}
function Sim-Playlist {
    if ($importedPlaylist -and $script:playlistRow -eq (1 + $importedPlaylist.treePath[0])) {
        $importedPlaylist.corpusTracks
        return
    }
    if ($env:SIM_EV -match '\b(format-model|legacy-playlist)\b') { 'alpha-one', 'bravo-two', 'charlie-three'; return }
    if ($env:SIM_EV -match '\bw16\b') {
        'alpha-one', 'bravo-two', 'charlie-three'
        if ($env:SIM_EV -match '\bcopytwice\b') { 'W16-FL~2', 'W16-FL~2' }
        elseif ($env:SIM_EV -match '\bcopyfirst\b') { 'W16-FL~2', 'w16-flac-longpath' }
        else { 'w16-flac-longpath', 'W16-FL~2' }
    } else { Rows }
}
function Press([IntPtr]$h, [string]$what, [int]$times = 1, [int]$gapMs = 250) {
    $script:presses[$what] = 1 + [int]$script:presses[$what]; $key = "$what#$($script:presses[$what])"
    Add-Content -LiteralPath (Join-Path $env:SIM 'presses') -Value "$key x$times"
    if ($env:SIM_EV -match '\bmeasure-clock\b') { Start-Sleep -Milliseconds (60 * $times + $gapMs * ($times - 1)) }
    $action = Ev $key; if ($action -eq 'lose') { return }
    if ($env:SIM_EV -eq 'silentstart' -and $what -eq 'Down') { [RbspMidi2]::First = Get-Date }   # it answers a press
    $rows = @(Sim-Playlist)
    switch ($what) {
        'AreaLeft' { $script:navRight = 0; $script:listLost = $false }
        'AreaRight' {
            $script:navRight++
            if ($script:navRight -eq 2 -and $importedPlaylist) {
                $script:sel = [math]::Min($rows.Count - 1, $script:sel)
            }
        }
        'Up' {
            if ($script:navRight -eq 1) { $script:treeSel = [math]::Max(0, $script:treeSel - $times) }
            $script:sel = [math]::Max(0, $script:sel - $times)
        }
        'Down' {
            if ($script:listLost) { return }   # load-leaves-list: the browser is off the track list
            if ($script:navRight -eq 1) { $script:playlistRow = $script:treeSel + $times }
            $script:sel = [math]::Min($rows.Count - 1, $script:sel + $times)
            if ($script:navRight -eq 2 -and $script:presses.Load -and $env:SIM_EV -match '\bselection-settling\b' -and
                -not $script:simReadyTargets.ContainsKey($script:sel)) {
                $script:simReadyTargets[$script:sel] = $true
                $script:selectionReadyAt = (Get-Date).AddMilliseconds(2500)
            }
        }
        'SortSong' { $script:desc = -not $script:desc }
        'Load' {
            if ($script:selectionReadyAt -and (Get-Date) -lt $script:selectionReadyAt) {
                # Selection has moved, but Load cannot yet read back the target.
                $script:deckOcr = 'unreadable text'
                return
            }
            $deck = $rows[$(if ($script:desc) { $rows.Count - 1 - $script:sel } else { $script:sel })]
            if ($env:SIM_EV -match '\bload-leaves-list\b') {
                # The VM (OI-82): after a Load, Browse Down leaves the track list as it is until the
                # list is re-entered; loading the deck's own track again starts nothing.
                $script:listLost = $true
                if ($deck -ceq $script:deckOcr) { return }
            }
            $script:deckOcr = if ($action -eq 'unreadable') { 'unreadable text' } else { $deck }
            if ($env:SIM_EV -match '\boff-list-siblings\b') {
                $script:deckOcr = $deck -replace '-1ch-', '-2ch-' -replace '-m4a$', '-mp4' -replace '-aif$', '-aiff'
            }
            if ($env:SIM_EV -match '\bchunk-wait\b' -and $deck -match '^fmt-speed-(\d\d)$') {
                $index = [int]$Matches[1]
                $script:chunkAt = (Get-Date).AddMilliseconds(2500)
                if ($Do -eq 'format-hits') {
                    if ($env:SIM_EV -match '\bcache-(double-tail|known-after-tail)\b') {
                        $script:chunkAt = (Get-Date).AddMilliseconds(500)
                    }
                    $hitLine = '2026-11-18 10:00:00  hit  {0:000000000000} (485100 samples)  [{1} hits, 0 near, 0 misses]'
                    $first = $hitLine -f (10 * $index + 1), 1
                    $second = $hitLine -f (10 * $index + 2), 2
                    if ($index -eq 1 -and $env:SIM_EV -match '\bchunk-repeat\b') {
                        $second = $hitLine -f (10 * $index + 1), 2
                    }
                    $silent = $hitLine -f (10 * $index + 3), 3
                    $tail = '2026-11-18 10:00:03  miss ffffffffffff (485100 samples)  [3 hits, 0 near, 1 misses]'
                    $script:chunkLines = @($second, $silent)
                    if ($env:SIM_EV -match '\bchunk-partial\b') { $script:chunkLines = @($second) }
                    $script:activityEvents = New-Object 'System.Collections.Generic.Queue[object]'
                    if ($env:SIM_EV -notmatch '\bcache-no-tail\b') {
                        $tailMs = if ($env:SIM_EV -match '\bcache-delay-(\d+)\b') { [int]$Matches[1] } else { 500 }
                        $tailAt = $script:cacheTailAt = $script:chunkAt.AddMilliseconds($tailMs)
                        $script:activityEvents.Enqueue(@{ At = $tailAt; Line = $tail })
                        if ($env:SIM_EV -match '\bcache-double-tail\b') {
                            $script:activityEvents.Enqueue(@{ At = $tailAt; Line = $tail.Replace('ffffffffffff', 'eeeeeeeeeeee') })
                        }
                        if ($env:SIM_EV -match '\bcache-known-after-tail\b') {
                            $script:activityEvents.Enqueue(@{ At = $tailAt; Line = $second })
                        }
                        if ($env:SIM_EV -match '\bchunk-late\b') {
                            $script:activityEvents.Enqueue(@{ At = $tailAt.AddMilliseconds(1000); Line = $second })
                        }
                        if ($index -eq 1) {
                            $lateLine = switch -Regex ($env:SIM_EV) {
                                '\bcache-disk-free\b' { '2026-11-18 10:00:03  cache: 123.4 GB free on its disk (GetDiskFreeSpaceEx), floor 10.0 GB (disk 100.0 GB)' }
                                '\bcache-pass-through\b' { '2026-11-18 10:00:03  cache: a Demucs run has tensors outside CPU memory (DirectML): passed through, not cached' }
                                '\bcache-rebuild-failed\b' { '2026-11-18 10:00:03  not cached ffffffffffff: the rebuild failed' }
                            }
                            if ($lateLine) {
                                $script:activityEvents.Enqueue(@{ At = $tailAt.AddMilliseconds(600); Line = $lateLine })
                            }
                        }
                    }
                } else {
                    $runMs = if ($env:SIM_EV -match '\bprobe-delay-(\d+)\b') { [int]$Matches[1] } else { 2300 }
                    $at = Get-Date
                    $script:chunkAt = $null
                    $script:activityEvents = New-Object 'System.Collections.Generic.Queue[object]'
                    for ($chunk = 1; $chunk -le 3; $chunk++) {
                        $keyLine = '2026-11-18 10:00:00 KEY {0} 17 {1:000000000000}{2} L=485100 F=476 model=0123456789ab never_cached={3}' -f
                            (1700000000100 + ($chunk - 1) * ($runMs + 600)), (10 * $index + $chunk), ('a' * 52), [int]($chunk -eq 3)
                        if ($chunk -eq 1) { $first = $keyLine }
                        else {
                            $at = $at.AddMilliseconds(600)
                            $script:activityEvents.Enqueue(@{ At = $at; Line = $keyLine })
                        }
                        $at = $at.AddMilliseconds($runMs)
                        $runLine = '2026-11-18 10:00:00 Run demucs #{0} {1} ms' -f ((($index - 1) * 3) + $chunk), $runMs
                        if ($chunk -eq 2) { $runLine += ' | detail -> output' }
                        $script:activityEvents.Enqueue(@{ At = $at; Line = $runLine })
                    }
                    if ($env:SIM_EV -match '\bchunk-late\b') {
                        $script:activityEvents.Enqueue(@{ At = $at.AddMilliseconds(1000); Line = $runLine })
                    }
                    # OI-81: other models' runs (rekordbox's embedding set) on another thread, and
                    # a stray stems run on the KEY thread with nothing pending
                    $loadAt = Get-Date
                    $extra = @(switch -Regex ($env:SIM_EV) {
                        '\bembed-other-thread\b' {
                            @{ At = $loadAt.AddMilliseconds(1000); Line = '10:00:01.000 [6376] Run cnn_embedding.onnx #2 94 ms | in signal float [1,463872] | out features float [1,64]' }
                            @{ At = $loadAt.AddMilliseconds(3000); Line = '10:00:03.000 [6376] Run model_genre.onnx #2 4 ms | in input_features float [1,66] | out output_label type 7' }
                            @{ At = $loadAt.AddMilliseconds(3100); Line = '10:00:03.100 [6376] Run model_genre.onnx #3 4 ms' }
                        }
                        '\bstray-same-thread\b' {
                            if ($index -eq 1) { @{ At = $loadAt.AddMilliseconds($runMs + 100); Line = '2026-11-18 10:00:00 Run demucs #99 2300 ms' } }
                        }
                    })
                    if ($extra.Count) {
                        $all = @($script:activityEvents.ToArray()) + $extra | Sort-Object { $_.At }
                        $script:activityEvents = New-Object 'System.Collections.Generic.Queue[object]'
                        foreach ($e in $all) { $script:activityEvents.Enqueue($e) }
                    }
                }
                if ($index -eq 1 -and $env:SIM_EV -match '\bchunk-unknown\b') {
                    $first = $first.Replace('000000000011', 'ffffffffffff')
                }
                Add-Content -LiteralPath $Bridge -Value $first
                if ($index -eq 1 -and $env:SIM_EV -match '\bchunk-cross\b') {
                    Add-Content -LiteralPath $Bridge -Value ($first.Replace('000000000011', '000000000021'))
                }
                return
            }
            $hit = '2026-11-18 10:00:00  hit  0123456789ab (441000 samples)  [1 hits, 0 near, 0 misses]'
            if ($env:SIM_EV -match '\bformat-model\b' -and $Do -ne 'format-hits') {
                if ($action -eq 'silent') { return }
                if ($action -eq 'exit-settle') {
                    $script:rbExitAt = (Get-Date).AddSeconds(2)  # after the 1.5 s deck read
                    return
                }
                $chunks = switch -Exact ($deck) {
                    'fmt-aac-lc-44100-2ch-lossy-m4a' { '1' * 64; '2' * 64 }
                    'fmt-wav-pcm-44100-2ch-s16-wav' { '5' * 64 }
                    default { '4' * 64 }
                }
                $at = Get-Date
                $script:activityEvents = New-Object 'System.Collections.Generic.Queue[object]'
                $run = 0
                foreach ($hash in $chunks) {
                    $run++
                    $keyLine = "2026-11-18 10:00:00 KEY 1700000000100 17 $hash L=485100 F=476 model=0123456789ab never_cached=0"
                    if ($run -eq 1) { Add-Content -LiteralPath $Bridge -Value $keyLine }
                    else { $script:activityEvents.Enqueue(@{ At = $at; Line = $keyLine }) }
                    $at = $at.AddMilliseconds(2300)
                    $script:activityEvents.Enqueue(@{ At = $at; Line = "2026-11-18 10:00:00 Run demucs #$run 2300 ms" })
                }
                return
            }
            if ($Do -eq 'format-hits') {
                if ($env:SIM_EV -match '\bcrash-while-waiting\b' -and $key -eq 'Load#1') {
                    $script:crashAt = (Get-Date).AddSeconds(2)
                }
                if ($env:SIM_EV -match '\bretry-late-hit\b' -and $key -eq 'Load#1') {
                    $script:deckOcr = 'unreadable text'
                    return
                }
                if ($action -in 'late', 'late-unreadable') {
                    if ($action -eq 'late-unreadable') { $script:deckOcr = 'unreadable text' }
                    $script:lateAt = (Get-Date).AddSeconds(3)
                    return
                }
                Add-Content -LiteralPath $Bridge -Value @($hit, '2026-11-18 10:00:00  timing run=hit total_ns=1000000', '2026-11-18 10:00:01  miss 0123456789ac')
                if ($env:SIM_EV -match '\bmany-misses\b') {
                    Add-Content -LiteralPath $Bridge -Value @('2026-11-18 10:00:02  miss 0123456789ad', '2026-11-18 10:00:03  miss 0123456789ae')
                }
                return
            }
            if ($action -eq 'pad') { Add-Content -LiteralPath $Bridge -Value @('w', 'w') }
            if ($action -eq 'rotate') { Add-Content -LiteralPath $Bridge -Value $hit; Move-Item -LiteralPath $Bridge -Destination "$Bridge.1" }
            if ($action -eq 'truncate') { Remove-Item -LiteralPath $Bridge }
            if ($action -eq 'diagnostic') { Add-Content -LiteralPath $Bridge -Value '2026-11-18 10:00:00  diagnostic' }
            elseif ($action -eq 'near') { Add-Content -LiteralPath $Bridge -Value '2026-11-18 10:00:00  near 0123456789ab for 0123456789ac: served' }
            elseif ($action -ne 'silent') { Add-Content -LiteralPath $Bridge -Value $hit }
        }
    }
}
EOF
  cat > "$k/ocr-lib.ps1" << 'EOF'
function Read-Deck1Titles([string]$png) {
    return $script:deckOcr, $script:deckOcr, $script:deckOcr, $script:deckOcr
}
function Read-ListHeader([string]$png, [int]$Scale = 2) {
    "RBSP Tests ($(@(Sim-Playlist).Count) Tracks)"
}
function Read-FormatListHeaders([string]$png) {
    if ($script:simStart) {
        if ((Get-Date) -ge $script:browserReadyAt) { return 'Collection (16 Tracks)', 'Collection (16 Tracks)' }
        return '', ''
    }
    if ($script:playlistRow -ne (1 + $importedPlaylist.treePath[0]) -or $env:SIM_EV -match '\bwrong-heading\b') {
        return 'Playlists', 'Playlists'
    }
    $name = $importedPlaylist.name
    if ($env:SIM_EV -match '\bwrong-nonce\b') { $name = $name -replace '0123456789ab$', 'fedcba987654' }
    if ($env:SIM_EV -match '\bclose-heading\b') { $name = $name -replace '9ab$', '9aa' }
    $label = if ($env:SIM_EV -match '\bgarbled-heading-count\b') { '16 Tr' } else { "$(@(Sim-Playlist).Count) Tracks" }
    "$name ($label)"
    if ($env:SIM_EV -notmatch '\bone-heading\b') { "$name ($label)" }
}

EOF
  # a library that fails to load (K: another kit folder)
  mkdir -p "$tmp/mk-fail"; cp "$here/kit/midi-auto.ps1" "$tmp/mk-fail/"; echo "throw 'kit-lib failed to load'" > "$tmp/mk-fail/kit-lib.ps1"
  sim() {   # NAME EVENTS ARGS...: its output in $tmp/sim-NAME/out (presses in presses), exit in rc
    local d="$tmp/sim-$1" ev="$2"; shift 2; mkdir -p "$d"; : > "$d/presses"
    SIM="$d" SIM_EV="$ev" TEMP="${TEMP:-$d}" APPDATA="${APPDATA:-$d}" LOCALAPPDATA="${LOCALAPPDATA:-$d}" \
      pwsh -NoProfile -NonInteractive -File "${K:-$k}/midi-auto.ps1" "$@" > "$d/out" 2>&1; echo $? > "$d/rc"
  }
  # simck NAME RC [+TEXT|-TEXT]...: the exit, and TEXT in (+) or not in (-) its output and presses
  simck() {
    local n="$1" d="$tmp/sim-$1" want="$2" c; shift 2
    [ "$(cat "$d/rc")" = "$want" ] && ok || { no "midi-auto $n: exit $(cat "$d/rc"), expected $want"; sed 's/^/    /' "$d/out"; }
    for c in "$@"; do
      if grep -qF -- "${c:1}" "$d/out" "$d/presses"; then [ "${c:0:1}" = + ] && ok || no "midi-auto $n: '${c:1}' found"
      else [ "${c:0:1}" = - ] && ok || { no "midi-auto $n: no '${c:1}'"; sed 's/^/    /' "$d/out"; }; fi
    done
  }
  r1=(-Do run -StartWait 1 -Quiet 1 -MaxTrack 4)
  sim clean '' "${r1[@]}"
  sim unreadable 'Load#1=unreadable' "${r1[@]}"
  sim unreaddesc 'desc,Load#1=unreadable,Load#3=unreadable' "${r1[@]}"
  sim unreadtwice 'Load#1=unreadable,Load#2=unreadable' "${r1[@]}"
  sim lostflip 'desc,SortSong#1=lose' "${r1[@]}"
  sim lostload 'Load#2=lose' "${r1[@]}"
  sim lostdown 'Down#3=lose' "${r1[@]}"
  sim resetrow 'Load#2=lose,Load#3=lose' "${r1[@]}"
  sim rotated 'Load#1=pad,Load#2=rotate' "${r1[@]}" -Expect hits
  sim truncated 'Load#1=pad,Load#2=truncate' "${r1[@]}" -Expect hits
  sim ownerstart owneronly -Do start
  sim updateboot updateboot -Do start
  sim silentstart silentstart -Do start
  sim updatequit updatequit -Do quit
  sim close-timeout close-timeout -Do quit
  sim ownerquit owneronly -Do quit
  sim ownerrun owneronly "${r1[@]}"
  sim ownerlogoff owneronly -Do logoff
  sim owneruninstall owneronly -Do uninstallcache
  sim noping noping -Do quit
  sim nofront nofront "${r1[@]}"
  sim frontlater frontlater "${r1[@]}"
  sim quit 'In=dialog' -Do quit
  sim sortdlg 'desc,Load#1=dialog' "${r1[@]}"
  sim retrydlg 'Down#3=lose,Load#2=dialog' "${r1[@]}"
  sim updlg 'Up#3=dialog' "${r1[@]}"
  sim downdlg 'Down#3=dialog' "${r1[@]}"
  sim recoverdlg 'Down#3=lose,Down#4=dialog' "${r1[@]}"
  sim sortstop 'Load#1=stop' "${r1[@]}"
  sim stoprow 'Load#2=stop' "${r1[@]}"
  sim stoplast 'said~row 3 charlie-three : read back=stop' "${r1[@]}"
  sim recstop 'Down#3=lose,Load#2=stop' "${r1[@]}"
  sim rectimeout 'Down#3=lose,Load#2=write' -Do run -StartWait 1 -Quiet 2 -MaxTrack 5
  sim prepdialog 'said~clicked=stop,said~clicked=dialog' -Do prepare -Minutes 1
  sim preptimeout 'said~clicked=stop' -Do prepare -Minutes 1
  sim prepappstays appstays -Do prepare -Minutes 1
  sim appexit '' -Do appexit
  sim appexitstays appstays -Do appexit
  sim diagonly 'Load#2=diagnostic' "${r1[@]}"
  sim silentprobe 'desc,Load#1=silent' "${r1[@]}" -Expect hits
  sim nearprobe 'desc,Load#1=near' "${r1[@]}" -Expect hits
  sim diagprobe 'Down#3=lose,Load#2=diagnostic' "${r1[@]}" -Expect hits
  sim repeathit 'Load#1=unreadable' "${r1[@]}" -Expect hits -Max 1
  sim repeatnohit 'Load#1=unreadable,Load#2=diagnostic' "${r1[@]}" -Expect hits -Max 1
  sim inject '' "${r1[@]}" -InjectAt 2
  (K="$tmp/mk-fail" sim libfail '' -Do quit)
  simck libfail 1 '+quit: STOP: error: kit-lib failed to load'   # RKR12
  simck clean 0 '+row 3 charlie-three : read back OK' '-STOP' '+run: expect any: 3 rows OK, 0 not'
  # rekordbox put in front (our app minimized) before the header and each deck read; a STOP if it can't be
  simck clean 0 "+run: 10:00:00 front: ok; our app's window(s) minimized: 1; rekordbox 0x10 in front" '+front#2'
  simck nofront 1 "+run: STOP: rekordbox not put in front (10:00:00 front: rekordbox's window 0x10 is not the foreground window)" '-Load#'
  simck frontlater 1 "+run: STOP: error: rekordbox not put in front (10:00:00 front: rekordbox's window 0x10 is not the foreground window)" '+Load#1' '-Load#2'
  # RKM19: retry OCR in the current direction. A matched other row permits one flip.
  simck unreadable 0 '+Load#2' '+run: expect any: 3 rows OK, 0 not' '-SortSong#'
  simck unreaddesc 0 "+run: row 1 is charlie-three; expected alpha-one: SortSONG (try 2)" '+Load#4' '+run: expect any: 3 rows OK, 0 not' '-SortSong#2'
  simck unreadtwice 1 '+Load#2' '+run: STOP: could not put row 1 first' '-SortSong#' '-Load#3'
  simck lostflip 1 '+SortSong#1' '+run: STOP: could not put row 1 first' '-SortSong#2'
  simck lostload 0 '+row 2 bravo-two : read back OK' '+run: expect any: 3 rows OK, 0 not'
  simck lostdown 0 '+deck 1 still has alpha-one: stepping again' '+run: expect any: 3 rows OK, 0 not'
  simck resetrow 0 '+finding the row from the top again' '+run: expect any: 3 rows OK, 0 not'
  simck rotated 0 '+0 misses, 2 hits, 0 refused' '+run: expect hits: 3 rows OK, 0 not'
  simck truncated 3 '+row 2 bravo-two : unmeasured' '+run: expect hits: 2 rows OK, 1 not'
  simck ownerstart 0 '+start: ready' '-already running'
  simck updateboot 0 '+start: ready' '-no MIDI'
  sim watcher-start watcher-start -Do start
  simck watcher-start 0 '+RB Stems Plus watcher: pressed Remind Me Later' '+start: ready'
  sim crash-start start-crash -Do start
  simck crash-start 1 '+start: STOP: rekordbox #32770 message box: 0x41: Unexpected application error' '-Down#' '-dismiss-rb-error'
  sim crash-load 'Load#1=crash' "${r1[@]}"
  simck crash-load 1 '+run: STOP: rekordbox #32770 message box: 0x41: Unexpected application error' '-Load#2' '-dismiss-rb-error'
  sim crash-private 'private-crash,Load#1=crash' "${r1[@]}"
  crash_private_log="$tmp/sim-crash-private/rbst/midi-auto.log"
  [ "$(cat "$tmp/sim-crash-private/rc")" = 1 ] && ! grep -qF 'Private Song' "$crash_private_log" &&
    grep -qF 'STOP: rekordbox #32770 message box: 0x11: unrecognised text (28 chars, sha256:1f5e9411c680)' "$crash_private_log" &&
    ok || { no "midi-auto crash-private: STOP leaked the title or omitted the safe diagnostic"; sed 's/^/    /' "$tmp/sim-crash-private/out"; }
  sim crash-quit quit-crash -Do quit
  # OI-80: rekordbox's Analysis Setting dialog is named in the STOP, never clicked
  sim analysis-load 'Load#1=analysis-dialog' "${r1[@]}"
  simck analysis-load 1 "+run: STOP: rekordbox's Analysis Setting dialog is open (0x14052E); the kit never clicks it (not approved)" \
    '+run autoanalysis-off' '+askAnalysisSettingWindow=0 in rekordbox3.settings' '-Load#2' '-STOP: error:'
  sim analysis-quit quit-analysis -Do quit
  simck analysis-quit 1 "+quit: STOP: rekordbox's Analysis Setting dialog is open (0x14052E)" '-new window' '-close asked'
  simck crash-quit 1 '+quit: STOP: rekordbox #32770 message box: 0x41: Unexpected application error' '-after 90 s' '-dismiss-rb-error'
  simck silentstart 0 '+start: no lights from rekordbox in 30 s: Browse Down and Up' '+Down#1' '+Up#1' '+start: ready' '-no MIDI'
  grep -qF 'start: 10:00:00 closeupdate: closed' "$tmp/sim-updateboot/rbst/midi-auto.log" && ok || no "midi-auto updateboot: close missing from the log"
  simck updatequit 0 '+quit: rekordbox closed' '-STOP'
  simck close-timeout 1 '+quit: STOP: rekordbox still running after 90 s; redacted UI dump format-uidump-0123456789abcdef0123456789abcdef.jsonl' '-quit: rekordbox closed'
  sim close-refused close-refused -Do quit
  simck close-refused 1 '+quit: STOP: rekordbox did not accept the close request (10:00:00 close asked: False)' '-quit: rekordbox closed'
  grep -qF 'quit: 10:00:00 closeupdate: closed' "$tmp/sim-updatequit/rbst/midi-auto.log" && ok || no "midi-auto updatequit: close missing from the log"
  grep -qF 'quit: 10:00:00 closeinfo: closed' "$tmp/sim-updatequit/rbst/midi-auto.log" && ok || no "midi-auto updatequit: INFO close missing from the log"
  simck ownerquit 0 '+quit: rekordbox not running' '-Play#'
  simck ownerrun 2 '+run: rekordbox not running' '-Load#'
  simck ownerlogoff 0 '+logoff: signing out'
  simck owneruninstall 2 '+uninstallcache: rekordbox is running'
  simck noping 1 '+quit: STOP: the session task did not report a session ID' '-Play#'
  # RKR2: no MIDI after the watch could have changed (Play, SortSong, a retry's Down and Load)
  simck quit 1 '+quit: STOP: not just' '-Play#'
  simck sortdlg 1 '+run: STOP: new window(s): rekordbox | Sign in' '-SortSong#'
  simck retrydlg 1 '+trying Load again' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#3'
  # RKM10: a dialog appears during navigation, after the earlier window check.
  simck updlg 1 '+Up#3' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#1'
  simck downdlg 1 '+Down#3' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#2'
  simck recoverdlg 1 '+Down#4' '+run: STOP: new window(s): rekordbox | Sign in' '-Load#4'
  # RKR5: midi-stop honoured inside the bridge waits and after the last row
  simck sortstop 1 '+run: STOP: midi-stop' '-run: row 1 is' '-Load#2'
  simck stoprow 1 '+run: STOP: midi-stop' '-row 2 bravo-two : read back' '-Load#3'
  simck stoplast 1 '+row 3 charlie-three : read back OK' '+run: STOP: midi-stop'
  # RKR8: the recovery load's wait acted on
  simck recstop 1 '+run: STOP: midi-stop' '-Load#3'
  simck rectimeout 1 "+run: STOP: row 2 bravo-two : the recovery load: the bridge still writing after 5 s" '-Load#3'
  simck prepdialog 1 '+prepare: STOP: midi-stop' '+prepare: STOP: new window(s)' '-prepstop:'
  simck preptimeout 1 '+prepare: prepstop: ok' '+not idle 60 s after Stop: prepstate: running (timeout 55 s)'
  # a fresh app for Prepare: its own Exit first; still running 30 s later is a STOP before anything else
  simck preptimeout 1 '+appexit' '+prepare: 10:00:00 appexit: ok; closed by'
  simck prepappstays 1 '+prepare: STOP: our app did not exit' '-prepare: 10:00:00 app: running' '-prepstop:'
  simck appexit 0 "+10:00:00 appexit: ok; closed by the menu's RB Stems Plus > Exit"
  simck appexitstays 1 '+appexit: STOP: still running 30 s after'
  simck diagonly 3 '+row 2 bravo-two : EXPECT FAIL: no classified bridge result' '+run: expect any: 2 rows OK, 1 not'
  simck silentprobe 3 "+row 3 charlie-three : EXPECT FAIL (the sort check's load): no bridge activity in 1 s" '+run: expect hits: 3 rows OK, 1 not'
  # RKM20: bridge activity alone is insufficient, including for a recovery probe.
  simck nearprobe 3 "+row 3 charlie-three : EXPECT FAIL (the sort check's load): 0 misses, 0 refused, 0 hits" '+run: expect hits: 3 rows OK, 1 not'
  simck diagprobe 3 "+row 1 alpha-one : EXPECT FAIL (the sort check's load): 0 misses, 0 refused, 0 hits" '+run: expect hits: 3 rows OK, 1 not'
  # Inconclusive OCR forces two loads of the same selection. Each has its own hit.
  simck repeathit 0 '+Load#1' '+Load#2' '-Load#3' '-Down#3' '-SortSong#' '+0 misses, 1 hits, 0 refused' '+run: expect hits: 1 rows OK, 0 not'
  [ "$(grep -cE '  hit  [^ ]+ \([0-9]+ samples' "$tmp/sim-repeathit/bridge.log")" = 2 ] && ok || no "midi-auto repeathit: expected two hits for two loads of alpha-one"
  simck repeatnohit 3 '+row 1 alpha-one : EXPECT FAIL: 0 misses, 0 refused, 0 hits' '+run: expect hits: 0 rows OK, 1 not'
  simck inject 1 '+run: STOP: new window(s): powershell | RBSP watch test | 0x50 (watch test);' '+run: closetest: ok' '-Load#2'
  touch "$k/corpus/v1/04-aac-256.m4a" "$k/corpus/v1/06-aac-48k.m4a" \
    "$k/corpus/v1/w10-mp4-video-first.mp4" "$k/corpus/v1/w18-aac-he-v2.m4a"
  sim directml-console '' -Do modules
  simck directml-console 3 '+modules: rekordbox session 7: onnxruntime.dll' '+modules: DirectML not loaded'
  sim aac-fresh '' -Do run -Aac -Warmup 90
  simck aac-fresh 0 '+waiting 90 s before the first load' '+w10-mp4-video-first : read back OK' \
    '+w18-aac-he-v2 : read back OK' '-bravo-two : read back OK'
  rm -f "$k/corpus/v1/04-aac-256.m4a" "$k/corpus/v1/06-aac-48k.m4a" \
    "$k/corpus/v1/w10-mp4-video-first.mp4" "$k/corpus/v1/w18-aac-he-v2.m4a"
  mkdir -p "$k/corpus/win"; touch "$k/corpus/win/w16-flac-longpath.flac" "$k/corpus/long-path.txt"
  sim w16 'w16' "${r1[@]}"
  sim w16swap 'w16,copyfirst' "${r1[@]}"
  sim w16only 'w16,copyfirst' "${r1[@]}" -From w16-flac-longpath -To w16-flac-longpath
  sim w16copy 'w16' "${r1[@]}" -From w16-flac-longpath-copy -To w16-flac-longpath-copy
  sim w16duplicate 'w16,copytwice' "${r1[@]}"
  sim w16from 'w16,copyfirst' "${r1[@]}" -From w16-flac-longpath-copy
  sim w16to 'w16,copyfirst' "${r1[@]}" -To w16-flac-longpath
  simck w16 0 '+row 4 w16-flac-longpath : read back OK' '+row 5 w16-flac-longpath-copy : read back OK' '+run: expect any: 5 rows OK, 0 not'
  simck w16swap 0 '+row 4 w16-flac-longpath-copy : read back OK' '+row 5 w16-flac-longpath : read back OK' '+run: expect any: 5 rows OK, 0 not'
  simck w16only 0 '+row 5 w16-flac-longpath : read back OK' '-w16-flac-longpath-copy : read back OK' '+run: expect any: 1 rows OK, 0 not'
  simck w16copy 0 '+row 5 w16-flac-longpath-copy : read back OK' '-w16-flac-longpath : read back OK' '+run: expect any: 1 rows OK, 0 not'
  simck w16duplicate 1 '+run: STOP: deck 1 is not the expected track'
  simck w16from 0 '+row 4 w16-flac-longpath-copy : read back OK' '-w16-flac-longpath : read back OK' '+run: expect any: 1 rows OK, 0 not'
  simck w16to 0 '+row 5 w16-flac-longpath : read back OK' '-w16-flac-longpath-copy : read back OK' '+run: expect any: 4 rows OK, 0 not'
  # Format subset rows in corpus/formats/: staged media files only (.tsv ignored), -Formats filtering,
  # rowkeys without Stems Cache/DirectML, and full 64-hex keycompare against rows.txt.
  rm -rf "$k/corpus/win" "$k/corpus/long-path.txt"
  mkdir -p "$k/corpus/formats" "$k/bench"
  touch "$k/corpus/formats/fmt-aac-lc-44100-2ch-lossy-m4a.m4a" \
        "$k/corpus/formats/fmt-wav-pcm-44100-1ch-s16-wav.wav" \
        "$k/corpus/formats/fmt-wav-pcm-44100-2ch-s16-wav.wav" \
        "$k/corpus/formats/rows.tsv"
  sim fmt-run '' -Do run -StartWait 1 -Quiet 1 -MaxTrack 4 -Formats
  sim fmt-hits '' -Do run -StartWait 1 -Quiet 1 -MaxTrack 4 -Expect hits -Formats
  simck fmt-run 2 '+use auto dumps --formats-only' '-Load#'
  simck fmt-hits 2 '+use auto dumps --formats-only' '-Load#'
  sim staged-hits legacy-playlist -Do run -StartWait 1 -Quiet 1 -MaxTrack 4 -Expect hits
  simck staged-hits 0 '+run: expect hits: 3 rows OK, 0 not' '-fmt-'
  # Stub research helper + model for rowkeys and keycompare (no Ort or DirectML.dll present).
  k_match='1111111111111111111111111111111111111111111111111111111111111111'
  k_last='2222222222222222222222222222222222222222222222222222222222222222'
  k_silent='3333333333333333333333333333333333333333333333333333333333333333'
  k_tail_diff='111111111111111111111111111111111111111111111111111111111111111f'
  stage_format_model() {
    (cd "$k/corpus/formats" && rm -f rows.tsv && printf 'RBSP format subset v1' > RBSP-FORMATS &&
      printf '%s\n' fmt-* "$@" | sed 's/\.[^.]*$//' > catalog.txt && shasum -a 256 catalog.txt fmt-* > SHA256SUMS)
  }
  stage_format_model
  cat > "$k/bench/rbsp-prepare.exe" << 'EOF'
#!/bin/bash
f="${@: -1}"
if [[ "$SIM_EV" == helper-error || ( "$SIM_EV" == helper-trace && "$f" == *-aac-* ) ]]; then
  trace='AAC MF type change: stream=1 native=0x1610/22050 Hz/2 ch PCM before=44100 Hz/2 ch/16 bit after=22050 Hz/2 ch/16 bit type_hr=0x00000000 flags=0x20 frames=1024 sample=1'
  printf '%s\n' "$trace" "wrapper: $trace" "$trace title=Private Track Name" \
    "$trace C:\Outside\private.flac" 'private helper diagnostic' >&2
fi
[ "$SIM_EV" != helper-error ] || exit 7
case "$SIM_EV" in
  helper-mode-proved) [[ "$1 $2" == '--resample-proved --keys' ]] || exit 8 ;;
  helper-mode-test-unproved-aac) [[ "$1 $2 $3" == '--test-unproved-aac --resample-proved --keys' ]] || exit 8 ;;
  helper-mode-test-unproved-mp3) [[ "$1 $2 $3" == '--test-unproved-mp3 --resample-proved --keys' ]] || exit 8 ;;
  helper-mode-test-unproved-aac+test-unproved-mp3)
    [[ "$1 $2 $3 $4" == '--test-unproved-aac --test-unproved-mp3 --resample-proved --keys' ]] || exit 8 ;;
esac
case "$f" in
  *fmt-speed-*)
    [[ "$1 $2" == '--resample-proved --keys' && "$#" = 7 ]] || exit 9
    id="${f##*fmt-speed-}"; id="${id%.wav}"
    if [[ "$SIM_EV" != *chunk-wait* || "$SIM_EV" == *quiet-only* || ( "$SIM_EV" == *chunk-refused* && "$id" == 01 ) ]]; then
      printf 'skipped\tunsupported test fixture\t%s\n' "$f"
      exit 0
    fi
    printf 'file\t1\t1455300\tpcm 44100 Hz\t%s\n' "$f"
    for chunk in 0 1 2; do
      token=$((10 * 10#$id + chunk + 1))
      if [[ "$SIM_EV" == *chunk-repeat* && "$id" == 01 && "$chunk" == 1 ]]; then token=$((token - 1)); fi
      printf 'key\t%012d%s\t0\t%s\t%s\t%s\n' "$token" \
        aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "$chunk" "$((chunk == 2))" "$f"
    done
    ;;
  *fmt-aac-lc-44100-2ch-lossy-m4a*)
    if [ "$1" = --test-unproved-aac ]; then
      [ "$2" != --test-unproved-mp3 ] || shift
      [ "$2" = --resample-proved ] && [ "$3" = --keys ] && [ "$#" = 8 ] || exit 9
      printf 'skipped\tAAC test refusal from this helper\t%s\n' "$f"
      exit 0
    fi
    printf 'file\t1\t1323000\taac\t%s\n' "$f"
    printf 'key\t1111111111111111111111111111111111111111111111111111111111111111\t0\t0\t0\t%s\n' "$f"
    printf 'key\t2222222222222222222222222222222222222222222222222222222222222222\t0\t1\t0\t%s\n' "$f"
    printf 'key\t3333333333333333333333333333333333333333333333333333333333333333\t0\t2\t1\t%s\n' "$f"
    ;;
  *fmt-mp3-cbr-22050-2ch-lossy-mp3*)
    [[ "$1 $2 $3" == '--test-unproved-mp3 --resample-proved --keys' && "$#" = 8 ]] || exit 9
    printf 'skipped\tMP3 test refusal from this helper\t%s\n' "$f"
    ;;
  *fmt-wav-pcm-44100-1ch-s16-wav*)
    printf 'file\t1\t485100\tpcm 44100 Hz\t%s\n' "$f"
    silent=0; [[ "$SIM_EV" == *all-silent* ]] && silent=1
    printf 'key\t4444444444444444444444444444444444444444444444444444444444444444\t0\t0\t%s\t%s\n' "$silent" "$f"
    ;;
  *fmt-wav-pcm-44100-2ch-s16-wav*)
    printf 'skipped\tunsupported test fixture\t%s\n' "$f"
    ;;
esac
EOF
  chmod +x "$k/bench/rbsp-prepare.exe"
  kcsim() {
    local n="$1" ev="$2"; shift 2
    local d="$tmp/sim-$n"
    mkdir -p "$d/Pioneer/rekordbox6/models/demucs3_model"
    : > "$d/Pioneer/rekordbox6/models/demucs3_model/hdemucs.onnx"
    (APPDATA="$d" TEMP="$d" SystemDrive= sim "$n" "$ev" "$@")
  }
  kcsim fmt-rowkeys '' -Do rowkeys -Formats
  simck fmt-rowkeys 0 '+rowkeys: fmt-aac-lc-44100-2ch-lossy-m4a exit 0' '+rowkeys: key 111111111111 version 0 chunk 0 silent 0' \
    '+rowkeys: fmt-wav-pcm-44100-2ch-s16-wav helper skipped input' '-alpha-one'
  kcsim fmt-rowkeys-aac owneronly -Do rowkeys -Formats -From fmt-aac-lc-44100-2ch-lossy-m4a -TestUnprovedAac
  simck fmt-rowkeys-aac 0 '+rowkeys: refusal: AAC test refusal from this helper' '-fmt-wav-' '-.m4a' '-rowkeys: key'
  kcsim fmt-rowkeys-open '' -Do rowkeys -Formats -From fmt-aac-lc-44100-2ch-lossy-m4a -TestUnprovedAac
  simck fmt-rowkeys-open 1 '+require rekordbox closed' '-AAC test refusal from this helper'
  # Repeated keys deduplicate. A silent helper chunk may be absent. A missing row cannot pass.
  mkdir -p "$tmp/sim-kc-match"
  printf '%s\n' \
    "fmt-aac-lc-44100-2ch-lossy-m4a 1700000000100 $k_match 485100" \
    "fmt-aac-lc-44100-2ch-lossy-m4a 1700000000200 $k_last 485100" \
    "fmt-aac-lc-44100-2ch-lossy-m4a 1700000000300 $k_match 485100" \
    "fmt-wav-pcm-44100-1ch-s16-wav 1700000000400 4444444444444444444444444444444444444444444444444444444444444444 485100" > "$tmp/sim-kc-match/rows.txt"
  kcsim kc-match '' -Do keycompare -Formats
  simck kc-match 3 '+fmt-aac-lc-44100-2ch-lossy-m4a : MATCH' '+fmt-wav-pcm-44100-1ch-s16-wav : MATCH' \
    '+fmt-wav-pcm-44100-2ch-s16-wav : MISSING_EVIDENCE' '+keycompare: evidence ' '+2 OK, 1 not OK'
  if grep -qE '[0-9a-f]{64}' "$tmp/sim-kc-match/out" "$tmp/sim-kc-match/rbst/t2/keycompare.tsv"; then
    no "keycompare leaked raw 64-hex keys into report or stdout"
  else
    ok
  fi
  # keycompare: full 64-hex comparison catches a mismatch past character 12, missing last chunk, and helper refusal
  mkdir -p "$tmp/sim-kc-diff"
  printf '%s\n' \
    "fmt-aac-lc-44100-2ch-lossy-m4a 1700000000100 $k_tail_diff 485100" \
    "fmt-wav-pcm-44100-2ch-s16-wav 1700000000200 5555555555555555555555555555555555555555555555555555555555555555 485100" > "$tmp/sim-kc-diff/rows.txt"
  kcsim kc-diff '' -Do keycompare -Formats
  simck kc-diff 3 '+fmt-aac-lc-44100-2ch-lossy-m4a : MISMATCH' '+last False' \
    '+fmt-wav-pcm-44100-1ch-s16-wav : MISSING_EVIDENCE' '+fmt-wav-pcm-44100-2ch-s16-wav : HELPER_REFUSED' '+0 OK, 3 not OK'
  helper_trace='helper stderr fmt-aac-lc-44100-2ch-lossy-m4a: AAC MF type change: stream=1 native=0x1610/22050 Hz/2 ch PCM before=44100 Hz/2 ch/16 bit after=22050 Hz/2 ch/16 bit type_hr=0x00000000 flags=0x20 frames=1024 sample=1'
  check_helper_trace() {
    local d="$tmp/sim-$1" artifact="$2"
    printf '%s\n' "$helper_trace" > "$tmp/expected-helper-stderr.log"
    cmp -s "$tmp/expected-helper-stderr.log" "$artifact" && ok ||
      no "$1: run folder did not retain exactly the safe helper diagnostic"
    grep -qxF "$helper_trace" "$d/rbst/midi-auto.log" && ok ||
      no "$1: kit log missing the helper diagnostic"
    if grep -qE 'wrapper:|Private Track Name|private[.]flac|private helper diagnostic' "$d/out" "$d/rbst/midi-auto.log"; then
      no "$1: unapproved helper stderr reached the kit log or console"
    else
      ok
    fi
  }
  kcsim fmt-helper-private helper-error -Do rowkeys -Formats
  simck fmt-helper-private 1 '+fmt-aac-lc-44100-2ch-lossy-m4a exit 7'
  rowkeys_trace=( "$tmp/sim-fmt-helper-private/rbst/t2"/rowkeys-*/helper-stderr.log )
  check_helper_trace fmt-helper-private "${rowkeys_trace[0]}"
  mkdir -p "$tmp/sim-kc-helper-private"
  cp "$tmp/sim-kc-match/rows.txt" "$tmp/sim-kc-helper-private/"
  kcsim kc-helper-private helper-error -Do keycompare -Formats -Rows fmt-aac-lc-44100-2ch-lossy-m4a
  simck kc-helper-private 3 '+HELPER_ERROR' '+exit 7'
  check_helper_trace kc-helper-private "$tmp/sim-kc-helper-private/rbst/t2/helper-stderr.log"

  cp -R "$tmp/sim-kc-match" "$tmp/sim-kc-scoped"
  mkdir "$tmp/sim-kc-scoped/real"
  scope=abcdef0123456789abcdef0123456789
  printf '%s' 0123456789abcdef0123456789abcdef > "$tmp/sim-kc-scoped/real/format-run.txt"
  kcsim kc-scoped '' -Do keycompare -Formats -Rows fmt-aac-lc-44100-2ch-lossy-m4a -EvidenceRun "$scope"
  simck kc-scoped 1 '+probe evidence belongs to another run' '- : MATCH'
  printf '%s' "$scope" > "$tmp/sim-kc-scoped/real/format-run.txt"
  kcsim kc-scoped '' -Do keycompare -Formats -Rows fmt-aac-lc-44100-2ch-lossy-m4a -EvidenceRun "$scope"
  simck kc-scoped 0 '+fmt-aac-lc-44100-2ch-lossy-m4a : MATCH'
  [ -f "$tmp/sim-kc-scoped/rbst/t2/compare-$scope/keycompare.tsv" ] && ok || no "helper comparison did not save a run-scoped report"

  # The active format directory owns its manifest. The desktop query is stubbed,
  # so put its filtered playlist snapshot into the recorded import state.
  format_fixture() {
    local name="$1" pos="$2" manifest playlist tracks="" id
    shift 2
    mkdir -p "$tmp/sim-$name/rbst"
    manifest="$(shasum -a 256 < "$fmt_dir/SHA256SUMS" | cut -d' ' -f1)"
    playlist="RBSP Formats ${manifest:0:16}-0123456789ab"
    for id in "$@"; do tracks="${tracks:+$tracks,}\"$id\""; done
    printf '{"manifest":"%s","name":"%s","playlist":{"id":"opaque-3","name":"%s","treePath":[%s],"corpusTracks":[%s],"otherFormatNames":["%s"],"listedPlaylistRows":%s}}' \
      "$(printf '%s' "$manifest" | tr 'a-f' 'A-F')" "$playlist" "$playlist" "$pos" "$tracks" \
      "RBSP Formats ${manifest:0:16}-0123456789ff" "${FORMAT_LISTED_ROWS:-8}" > "$tmp/sim-$name/rbst/format-import-state.json"
  }
  fmt_dir="$k/corpus/formats"
  fmt_a=fmt-aac-lc-44100-2ch-lossy-m4a; fmt_b=fmt-wav-pcm-44100-1ch-s16-wav; fmt_c=fmt-wav-pcm-44100-2ch-s16-wav
  fmt_run=0123456789abcdef0123456789abcdef
  format_fixture fmt-import 3 "$fmt_c" "$fmt_b"
  fmt_state="$tmp/sim-fmt-import/rbst/format-import-state.json"
  # --- format pass model
  format_fixture fmt-pass 3 "$fmt_c" "$fmt_b" "$fmt_a"
  kcsim fmt-pass format-model -Do format-hits -FormatPass -StartWait 1
  simck fmt-pass 0 '+FORMAT CONTINUATION 123:' '+verified the formats heading' '+Load#3'
  continuation="$(sed -n 's/^FORMAT CONTINUATION //p' "$tmp/sim-fmt-pass/out")"
  kcsim fmt-pass format-model -Do format-hits -FormatPass -FormatContinuation "$continuation" -StartWait 1
  simck fmt-pass 0 '+verified the formats heading' '+Load#3'
  continuation="$(sed -n 's/^FORMAT CONTINUATION //p' "$tmp/sim-fmt-pass/out")"
  kcsim fmt-pass 'format-model,restart-between-batches' -Do format-hits -FormatPass -FormatContinuation "$continuation" -StartWait 1
  simck fmt-pass 1 '+rekordbox changed during the format pass' '-Load#'
  kcsim fmt-pass 'format-model,Up#3=late-hit' -Do format-hits -FormatPass -FormatContinuation "$continuation" -StartWait 1
  simck fmt-pass 1 '+late bridge activity between format batches' '+verified the formats heading' '-Load#'
  # --- end format pass model
  # --- format focus model
  format_fixture fmt-focus 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-focus format-model -Do format-plan -FormatRun "$fmt_run"
  RBKIT_TIMING=1 kcsim fmt-focus 'format-model,selection-settling' -Do format-run -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c" -StartWait 1
  simck fmt-focus 0 '+verified the formats heading' "+$fmt_c KEYS" '+Load#3' '-Load#4' '-retrying from the top' '+TIMING windows midi-navigation elapsed_ms='
  # --- end format focus model
  # --- format readiness model
  RBKIT_TIMING=1 sim fmt-ready format-start -Do start -Formats
  simck fmt-ready 0 '+closeagent: 15000 ms' '+start: ready' '+TIMING windows browser-readiness elapsed_ms=' '-browser heading still pending'
  sim fmt-ready-update 'format-start,late-start-update' -Do start -Formats
  simck fmt-ready-update 0 '+closeupdate: closed;' '+closeagent: 15000 ms' '+start: ready' '-browser heading still pending'
  sim fmt-ready-cap 'format-start,slow-browser' -Do start -Formats
  simck fmt-ready-cap 0 '+closeagent: 15000 ms' '+browser heading still pending' '+start: ready'
  sim fmt-ready-dialog 'format-start,startup-dialog' -Do start -Formats
  simck fmt-ready-dialog 1 '+unexpected windows at the start' '-private fixture caption' '-start: ready'
  # --- end format readiness model
  kcsim fmt-import format-model -Do format-verify
  simck fmt-import 0 "+$fmt_a IMPORT_REFUSED" "+$fmt_b IMPORT_ADDED" "+$fmt_c IMPORT_ADDED" '-.wav' '-Load#'
  kcsim fmt-import format-model -Do format-plan -FormatRun "$fmt_run"
  simck fmt-import 1 "+absent corpus IDs: $fmt_a" '+--confirm-import-refusals' '-Load#'
  kcsim fmt-import format-model -Do format-plan -FormatRun "$fmt_run" -ConfirmImportRefusals
  simck fmt-import 0 "+FORMAT PLAN $fmt_a,$fmt_b,$fmt_c" '-Load#' '-AreaLeft#'
  kcsim fmt-import format-model -Do format-run -FormatRun "$fmt_run" -ConfirmImportRefusals -Rows "$fmt_a,$fmt_b,$fmt_c" -StartWait 1
  simck fmt-import 0 "+$fmt_a IMPORT_REFUSED" "+$fmt_b KEYS" "+$fmt_c KEYS" '+front#1' '+Down#2 x4' '+verified the formats heading' '+Load#2' '-Load#3' '-TIMING windows'
  evidence="$tmp/sim-fmt-import/rbst/t2/format-$fmt_run"
  cmp -s "$evidence/plan.txt" "$evidence/complete.txt" && ok || no "completed format run did not seal its plan"
  kcsim fmt-import 'format-model,desc' -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-import 3 "+$fmt_a IMPORT_REFUSED" "+$fmt_b CACHE_HIT exact=1 miss=1 refused=0 near=0 slowest-hit-ms=1.00" "+$fmt_c CACHE_HIT" '+Load#3' '-Load#4'
  kcsim fmt-import 'format-model,Load#1=unreadable,Load#2=unreadable' -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-import 3 "+$fmt_b LOAD_UNCONFIRMED" "+$fmt_c CACHE_HIT" "-$fmt_b CACHE_HIT"
  kcsim fmt-import 'format-model,Down#2=lose' -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-import 1 '+RBSP Formats heading not verified' '-Load#' '-AreaLeft#2'
  kcsim fmt-import 'format-model,wrong-heading' -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-import 1 '+RBSP Formats heading not verified at MIDI tree step 4' '+Down#2 x4' '-Load#' '-AreaLeft#2'
  for ev in wrong-nonce one-heading close-heading; do
    RBKIT_TIMING=1 kcsim fmt-import "format-model,$ev" -Do format-hits -ConfirmImportRefusals -StartWait 1
    simck fmt-import 1 '+RBSP Formats heading not verified at MIDI tree step 4' '+TIMING windows ocr-heading elapsed_ms=' '-Load#'
  done
  FORMAT_LISTED_ROWS=13 format_fixture fmt-heading-count 13 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-heading-count 'format-model,garbled-heading-count' -Do format-hits -StartWait 1
  simck fmt-heading-count 0 '+verified the formats heading at MIDI tree step 14' '+Load#3'
  kcsim fmt-heading-count 'format-model,garbled-heading-count,wrong-nonce' -Do format-hits -StartWait 1
  simck fmt-heading-count 1 '+RBSP Formats heading not verified at MIDI tree step 14' '-Load#'
  sed -i.bak 's/0123456789ff/0123456789ab (Copy)/' "$tmp/sim-fmt-heading-count/rbst/format-import-state.json"
  kcsim fmt-heading-count 'format-model,garbled-heading-count' -Do format-hits -StartWait 1
  simck fmt-heading-count 1 '+RBSP Formats heading not verified at MIDI tree step 14' '-Load#'
  format_fixture fmt-nested 3,1 "$fmt_c" "$fmt_b"
  kcsim fmt-nested format-model -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-nested 1 '+verified root position' '-AreaLeft#' '-Load#'
  sed -i.bak 's/opaque-3/opaque-4/' "$fmt_state"
  kcsim fmt-import format-model -Do format-run -FormatRun "$fmt_run" -ConfirmImportRefusals -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-import 1 '+changed since verification' '-Load#'
  format_fixture fmt-refusals 2 "$fmt_b" "$fmt_c"
  kcsim fmt-refusals format-model -Do format-plan -FormatRun "$fmt_run" -ConfirmImportRefusals
  simck fmt-refusals 0 "+FORMAT PLAN $fmt_a,$fmt_b,$fmt_c" '-Load#'
  kcsim fmt-refusals 'format-model,private-deck,Load#1=lose,Load#2=lose' -Do format-run -FormatRun "$fmt_run" \
    -ConfirmImportRefusals -Rows "$fmt_a,$fmt_b,$fmt_c" -StartWait 1
  simck fmt-refusals 0 "+$fmt_a IMPORT_REFUSED" "+$fmt_b LOAD_UNCONFIRMED" "+$fmt_c KEYS" '+Load#3' '-private fixture title' '-LOAD_REFUSED'
  kcsim fmt-refusals format-model -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-refusals 3 "+$fmt_a : IMPORT_REFUSED" "+$fmt_b : LOAD_UNCONFIRMED" "+$fmt_c : HELPER_REFUSED" '-unsupported test fixture'
  evidence="$tmp/sim-fmt-refusals/rbst/t2/format-$fmt_run"
  awk -F '\t' -v row="$fmt_a" '$1 == row && $12 == "IMPORT_REFUSED" && $13 == "KEYS" { found = 1 } END { exit !found }' "$evidence/keycompare.tsv" &&
    ok || no "format comparison omitted the helper result for a rekordbox refusal"
  printf '%s\n' "$fmt_a IMPORT_REFUSED" "$fmt_b LOAD_REFUSED" > "$evidence/outcomes.txt"
  kcsim fmt-refusals format-model -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-refusals 3 "+$fmt_c : MISSING_EVIDENCE" "+$fmt_b : LOAD_UNCONFIRMED" '+0 OK, 3 not OK'

  printf '%s\n' "$fmt_a IMPORT_REFUSED" "$fmt_b LOAD_UNCONFIRMED" "$fmt_c KEYS" > "$evidence/outcomes.txt"
  : > "$evidence/rows.txt"
  kcsim fmt-refusals format-model -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-refusals 3 "+$fmt_c : MISSING_EVIDENCE" '-HELPER_REFUSED'

  format_fixture fmt-crash 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-crash 'format-model,Load#1=crash' -Do format-hits -StartWait 1
  simck fmt-crash 1 '+format-hits: STOP: rekordbox #32770 message box: 0x41: Unexpected application error' '-CACHE_HIT' '-Load#2' '-dismiss-rb-error'

  format_fixture fmt-analysis 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-analysis 'format-model,Load#1=analysis-dialog' -Do format-hits -StartWait 1
  simck fmt-analysis 1 "+format-hits: STOP: rekordbox's Analysis Setting dialog is open (0x14052E)" '-format operation failed' '-CACHE_HIT' '-Load#2'

  format_fixture fmt-crash-wait 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-crash-wait 'format-model,crash-while-waiting' -Do format-hits -StartWait 1 -Quiet 3
  simck fmt-crash-wait 1 '+format-hits: STOP: rekordbox #32770 message box: 0x41: Unexpected application error' '-CACHE_HIT' '-Load#2'

  format_fixture fmt-exit-settle 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-exit-settle format-model -Do format-plan -FormatRun "$fmt_run"
  kcsim fmt-exit-settle 'format-model,Load#3=exit-settle' -Do format-run -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c" -StartWait 1
  simck fmt-exit-settle 1 "+$fmt_b KEYS" '+Load#3' '-LOADED_NO_KEYS'
  evidence="$tmp/sim-fmt-exit-settle/rbst/t2/format-$fmt_run"
  [ ! -e "$evidence/complete.txt" ] && ok || no "interrupted format run wrote a completion seal"
  kcsim fmt-exit-settle format-model -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-exit-settle 1 '+formats: the format run did not complete' '- : MATCH'
  cp "$evidence/plan.txt" "$evidence/complete.txt"
  printf '\342\200\213' >> "$evidence/complete.txt" # U+200B in UTF-8
  kcsim fmt-exit-settle format-model -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c"
  simck fmt-exit-settle 1 '+formats: the format run did not complete' '- : MATCH'

  format_fixture fmt-exit-final-watch 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-exit-final-watch format-model -Do format-plan -FormatRun "$fmt_run"
  kcsim fmt-exit-final-watch "format-model,said~$fmt_c KEYS=exit-on-window-list" -Do format-run \
    -FormatRun "$fmt_run" -Rows "$fmt_a,$fmt_b,$fmt_c" -StartWait 1
  simck fmt-exit-final-watch 1 "+$fmt_c KEYS" '+formats: rekordbox exited after load'
  evidence="$tmp/sim-fmt-exit-final-watch/rbst/t2/format-$fmt_run"
  [ ! -e "$evidence/complete.txt" ] && ok || no "format run wrote a completion seal after rekordbox exited during the final watch"

  # KFW5
  for ev in late-unreadable late; do
    format_fixture "fmt-settle-$ev" 3 "$fmt_b" "$fmt_c"
    RBKIT_TIMING=1 kcsim "fmt-settle-$ev" "format-model,Load#1=$ev" -Do format-hits -ConfirmImportRefusals -StartWait 1 -Quiet 3
    simck "fmt-settle-$ev" 1 '+late bridge activity after a silent Load' '+Load#1' '-Load#2'
    awk -v scope="row=$fmt_b attempt=1" '
      /TIMING windows load-activity-wait / && index($0, scope) { activity = NR }
      /TIMING windows load-settle-wait / && index($0, scope) { settle = NR }
      /TIMING windows load / && index($0, scope) { load = NR }
      END { exit !(activity && activity < settle && settle < load) }
    ' "$tmp/sim-fmt-settle-$ev/out" && ok || no "silent Load $ev did not time activity, settle and total in order"
  done
  format_fixture fmt-retry-late 3 "$fmt_b" "$fmt_c"
  kcsim fmt-retry-late 'format-model,retry-late-hit,Up#4=late-hit' -Do format-hits -ConfirmImportRefusals -StartWait 1 -Quiet 3
  simck fmt-retry-late 1 '+late bridge activity after a silent Load' '+Load#1' '+Up#4' '-Load#2'
  # End KFW5

  # KFW11
  mv "$fmt_dir" "$k/corpus/saved-formats"; mkdir "$fmt_dir"
  touch "$fmt_dir/$fmt_b.wav"
  stage_format_model
  format_fixture fmt-all-silent 3 "$fmt_b"
  kcsim fmt-all-silent format-model -Do format-plan -FormatRun "$fmt_run"
  simck fmt-all-silent 0 "+FORMAT PLAN $fmt_b"
  kcsim fmt-all-silent 'format-model,Load#1=silent' -Do format-run -FormatRun "$fmt_run" -Rows "$fmt_b" -StartWait 1
  simck fmt-all-silent 0 "+$fmt_b LOADED_NO_KEYS"
  kcsim fmt-all-silent all-silent -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_b"
  simck fmt-all-silent 3 "+$fmt_b : MISSING_EVIDENCE (rb 0, helper 0+1s" '- : MATCH'
  rm -rf "$fmt_dir"; mv "$k/corpus/saved-formats" "$fmt_dir"
  # End KFW11

  # KFW7
  format_fixture fmt-private-start 3 "$fmt_b" "$fmt_c"
  kcsim fmt-private-start 'format-model,private-window-at-start' -Do format-hits -ConfirmImportRefusals
  simck fmt-private-start 1 '+2 window(s); handles: 0x10, 0x40' '-private fixture' '-0x99' '-Load#'
  format_fixture fmt-private-load 3 "$fmt_b" "$fmt_c"
  kcsim fmt-private-load 'format-model,private-window,Up#3=dialog' -Do format-hits -ConfirmImportRefusals
  simck fmt-private-load 1 '+1 window(s); handles: 0x40' '-private fixture' '-Load#'
  for n in fmt-private-start fmt-private-load; do
    grep -qF 'handles:' "$tmp/sim-$n/rbst/midi-auto.log" && ! grep -qF 'private fixture' "$tmp/sim-$n/rbst/midi-auto.log" &&
      ok || no "midi-auto $n: format watch did not redact the logged window title"
  done
  # End KFW7

  # KFW4
  for ev in 'Up#3=stop' 'Down#3=stop' 'Down#3=expire' 'Down#3=exit-rb'; do
    n="fmt-abort-${ev%=*}-${ev#*=}"
    format_fixture "$n" 3 "$fmt_c" "$fmt_b"
    kcsim "$n" "format-model,$ev" -Do format-hits -ConfirmImportRefusals -StartWait 1
    simck "$n" 1 '+formats: measurement stopped before load' "+${ev%=*}" '-Load#'
    if [[ "$ev" == Up* ]]; then simck "$n" 1 '-Down#3'; fi
  done
  # End KFW4

  # KFW2
  FORMAT_LISTED_ROWS=170 format_fixture fmt-deep-tree 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-deep-tree 'format-model,deep-tree' -Do format-hits -StartWait 1
  simck fmt-deep-tree 0 '+Up#1 x10' '+Up#2 x172' '+verified the formats heading' '+Load#3'
  # End KFW2

  # KFW13
  format_fixture fmt-hit-one-miss 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-hit-one-miss format-model -Do format-hits -StartWait 1
  simck fmt-hit-one-miss 0 "+$fmt_a CACHE_HIT exact=1 miss=1 refused=0"
  format_fixture fmt-hit-many-misses 3 "$fmt_a" "$fmt_b" "$fmt_c"
  kcsim fmt-hit-many-misses 'format-model,many-misses' -Do format-hits -StartWait 1
  simck fmt-hit-many-misses 3 "+$fmt_a CACHE_HIT exact=1 miss=3 refused=0"
  # End KFW13

  # KFW14
  fmt_prepare=(-Do prepare -Formats -Minutes 1 -Playlist 'RBSP Formats 0123456789abcdef-0123456789ab')
  sim fmt-prep-stop 'format-stop-model,said~clicked=stop' "${fmt_prepare[@]}"
  simck fmt-prep-stop 1 '+prepare: STOP: midi-stop' '+prepare: prepstop: ok' '+prepare: stopped, idle'
  sim fmt-prep-timeout format-stop-model "${fmt_prepare[@]}"
  simck fmt-prep-timeout 1 '+prepare: STOP: still running after 1 min' '+prepare: prepstop: ok' '+prepare: stopped, idle' '- shot '
  sim fmt-prep-private-at-start 'said~app: running=secret-dialog' "${fmt_prepare[@]}"
  simck fmt-prep-private-at-start 1 '+unexpected windows at the start' '-Secret Title' '-prepare: clicked'
  sim fmt-prep-private-poll 'said~clicked=secret-dialog' "${fmt_prepare[@]}"
  simck fmt-prep-private-poll 1 '+window(s); handles: 0x40; new windows' '-Secret Title'
  sim fmt-prep-private-stop 'said~clicked=stop,said~midi-stop=secret-dialog' "${fmt_prepare[@]}"
  simck fmt-prep-private-stop 1 '+prepare: STOP: midi-stop' '+window(s); handles: 0x40; new windows' '-Secret Title' '-prepstop'
  # End KFW14

  # KFW17
  format_fixture fmt-helper-modes 3 "$fmt_b"
  fmt_all="$fmt_a,$fmt_b,$fmt_c"
  kcsim fmt-helper-modes format-model -Do format-plan -FormatRun "$fmt_run" -ConfirmImportRefusals
  simck fmt-helper-modes 0 "+FORMAT PLAN $fmt_all"
  kcsim fmt-helper-modes format-model -Do format-run -FormatRun "$fmt_run" -Rows "$fmt_all" -ConfirmImportRefusals -StartWait 1
  simck fmt-helper-modes 0 "+$fmt_b KEYS"
  mode_report="$tmp/sim-fmt-helper-modes/rbst/t2/format-$fmt_run/keycompare.tsv"
  for mode in proved test-unproved-aac test-unproved-mp3 test-unproved-aac+test-unproved-mp3; do
    opts=()
    case "$mode" in
      test-unproved-aac) opts=(-TestUnprovedAac) ;;
      test-unproved-mp3) opts=(-TestUnprovedMp3) ;;
      test-unproved-aac+test-unproved-mp3) opts=(-TestUnprovedAac -TestUnprovedMp3) ;;
    esac
    kcsim fmt-helper-modes "helper-mode-$mode" -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_all" ${opts[@]+"${opts[@]}"}
    simck fmt-helper-modes 3 "+$fmt_b : MATCH" '-HELPER_ERROR'
    awk -F '\t' -v mode="$mode" 'NR == 1 { if ($14 != "helper_mode") exit 1; next }
      $14 != mode { exit 1 } END { if (NR != 4) exit 1 }' "$mode_report" &&
      ok || no "format comparison omitted helper_mode $mode"
  done
  # End KFW17

  fmt_mp3=fmt-mp3-cbr-22050-2ch-lossy-mp3
  touch "$fmt_dir/$fmt_mp3.mp3"
  stage_format_model
  kcsim fmt-rowkeys-mp3 owneronly -Do rowkeys -Formats -From "$fmt_mp3" -TestUnprovedMp3
  simck fmt-rowkeys-mp3 0 '+rowkeys: refusal: MP3 test refusal from this helper' '-fmt-wav-' '-.mp3' '-rowkeys: key'
  kcsim fmt-rowkeys-mp3-open '' -Do rowkeys -Formats -From "$fmt_mp3" -TestUnprovedMp3
  simck fmt-rowkeys-mp3-open 1 '+MP3 diagnostics require rekordbox closed' '-MP3 test refusal from this helper'
  rm "$fmt_dir/$fmt_mp3.mp3"
  stage_format_model

  # CROSS-ROW
  format_fixture fmt-cross-row 3 "$fmt_a" "$fmt_b" "$fmt_c"
  fmt_all="$fmt_a,$fmt_b,$fmt_c"
  kcsim fmt-cross-row format-model -Do format-plan -FormatRun "$fmt_run"
  simck fmt-cross-row 0 "+FORMAT PLAN $fmt_all"
  evidence="$tmp/sim-fmt-cross-row/rbst/t2/format-$fmt_run"
  cp "$evidence/plan.txt" "$evidence/complete.txt"
  printf '%s\n' "$fmt_a KEYS" "$fmt_b KEYS" "$fmt_c LOAD_UNCONFIRMED" > "$evidence/outcomes.txt"
  hash_b=4444444444444444444444444444444444444444444444444444444444444444
  printf '%s\n' "$fmt_a 1700000000100 $hash_b 485100" "$fmt_a 1700000000200 $hash_b 485100" \
    "$fmt_b 1700000000300 $k_last 485100" "$fmt_b 1700000000400 $k_match 485100" > "$evidence/rows.txt"
  kcsim fmt-cross-row helper-trace -Do keycompare -FormatRun "$fmt_run" -Rows "$fmt_all"
  check_helper_trace fmt-cross-row "$evidence/helper-stderr.log"
  simck fmt-cross-row 3 "+$fmt_a : WRONG_TRACK" "+$fmt_b : WRONG_TRACK" "+$fmt_c : LOAD_UNCONFIRMED" "-$hash_b"
  {
    printf 'row\tsource\thash\tsilent\n'
    printf '%s\trekordbox\t%s\t\n' "$fmt_a" "$hash_b" "$fmt_a" "$hash_b" "$fmt_b" "$k_last" "$fmt_b" "$k_match"
    printf '%s\thelper\t%s\t%s\n' "$fmt_a" "$k_match" 0 "$fmt_a" "$k_last" 0 "$fmt_a" "$k_silent" 1 "$fmt_b" "$hash_b" 0
  } > "$tmp/want-chunk-hashes.tsv"
  cmp -s "$tmp/want-chunk-hashes.tsv" "$evidence/chunk-hashes.tsv" &&
    ok || no "format comparison omitted raw scoped content hash lists"
  # End CROSS-ROW

  fmt_aif=fmt-aif-pcm-44100-2ch-s16-aif
  touch "$fmt_dir/$fmt_aif.aif"
  stage_format_model "${fmt_a%m4a}mp4.mp4" "${fmt_aif}f.aiff"
  format_fixture fmt-deck-siblings 3 "$fmt_aif" "$fmt_a" "$fmt_b"
  kcsim fmt-deck-siblings 'format-model,off-list-siblings' -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-deck-siblings 3 "+$fmt_aif LOAD_UNCONFIRMED" "+$fmt_a LOAD_UNCONFIRMED" "+$fmt_b LOAD_UNCONFIRMED" '-CACHE_HIT'
  stage_format_model "fmt-aif-pcm-441oo-2ch-s16-aif.aif"
  format_fixture fmt-deck-collision 3 "$fmt_aif" "$fmt_a" "$fmt_b"
  kcsim fmt-deck-collision format-model -Do format-hits -ConfirmImportRefusals -StartWait 1
  simck fmt-deck-collision 3 "+$fmt_aif LOAD_UNCONFIRMED" "+$fmt_a CACHE_HIT" "-$fmt_aif CACHE_HIT"

  rm -rf "$k/corpus/formats"; mkdir "$k/corpus/formats"
  fmt_rows=()
  for ((i=1; i<=172; i++)); do
    id="$(printf 'fmt-row-%03d' "$i")"
    touch "$fmt_dir/$id.wav"
    fmt_rows+=("$id")
  done
  stage_format_model
  FORMAT_LISTED_ROWS=16 format_fixture fmt-large 15 "${fmt_rows[@]}"
  kcsim fmt-large 'format-model,bottom' -Do format-plan -FormatRun "$fmt_run"
  simck fmt-large 0 '+batch 1 of 43' '+FORMAT PLAN fmt-row-001,fmt-row-002,fmt-row-003,fmt-row-004'
  kcsim fmt-large 'format-model,bottom' -Do format-run -FormatRun "$fmt_run" -Rows fmt-row-001,fmt-row-002,fmt-row-003,fmt-row-004 -StartWait 1
  simck fmt-large 0 '+Up#1 x10' '+Up#2 x18' '+Up#3 x172' '+fmt-row-001 KEYS' '+fmt-row-004 KEYS' '-fmt-row-005'
  format_fixture fmt-resume 2 "${fmt_rows[@]}"
  kcsim fmt-resume format-model -Do format-plan -FormatRun "$fmt_run" -FormatBatch 43
  simck fmt-resume 0 '+batch 43 of 43' '+FORMAT PLAN fmt-row-169,fmt-row-170,fmt-row-171,fmt-row-172'
  cp "$here/kit/format-stage.ps1" "$k/"
  (TEMP="$tmp" SystemDrive= pwsh -NoProfile -NonInteractive -File "$k/format-stage.ps1" -Mode clear) > "$tmp/format-clear-out" 2>&1 &&
    [ ! -e "$fmt_dir/SHA256SUMS" ] && ok || no "format-plan fixture clear: $(cat "$tmp/format-clear-out")"
  kcsim fmt-cleared '' -Do format-plan -FormatRun "$fmt_run"
  simck fmt-cleared 1 '+formats: no active format selection; stage a verified format subset first' '-FORMAT PLAN'
  # --- format speed model
  # Replay the 16-row selection with four batches and both load modes. Use the verified
  # playlist in both cases. Startup covers MIDI-to-browser only; external work has no fake cost.
  rm -rf "${fmt_dir:?}"
  mkdir -p "$fmt_dir"
  speed_rows=()
  for ((i=1; i<=16; i++)); do
    id="$(printf 'fmt-speed-%02d' "$i")"
    touch "$fmt_dir/$id.wav"
    speed_rows+=("$id")
  done
  stage_format_model
  format_fixture fmt-speed-before 3 "${speed_rows[@]}"
  speed_ids=(); speed_batches=()
  for ((i=1; i<=4; i++)); do
    speed_ids+=("$(printf '%032d' "$i")")
    speed_batches+=("$(IFS=,; echo "${speed_rows[*]:$(((i - 1) * 4)):4}")")
    kcsim fmt-speed-before format-model -Do format-plan -FormatBatch "$i" -FormatRun "${speed_ids[i-1]}"
    simck fmt-speed-before 0 "+FORMAT PLAN ${speed_batches[i-1]}"
  done
  cp -R "$tmp/sim-fmt-speed-before" "$tmp/sim-fmt-speed-after"
  format_fixture fmt-speed-cache 3 "${speed_rows[@]}"
  speed_totals=(); speed_attempts=()
  for speed_pass in before after cache; do
    speed_name="fmt-speed-$speed_pass"
    speed_ev='format-model,measure-clock'
    speed_ms=0; speed_loads=0; speed_next=""
    for ((i=1; i<=4; i++)); do
      speed_args=()
      [ "$speed_pass" = before ] || speed_args+=(-FormatPass)
      [ -z "$speed_next" ] || speed_args+=(-FormatContinuation "$speed_next")
      case "$speed_pass" in
        before | after)
          ev="$speed_ev,selection-settling"
          [ "$speed_pass" = after ] || ev="$ev,legacy-focus"
          kcsim "$speed_name" "$ev" -Do format-run -FormatBatch "$i" \
            -FormatRun "${speed_ids[i-1]}" -Rows "${speed_batches[i-1]}" ${speed_args[@]+"${speed_args[@]}"} ;;
        cache)
          kcsim "$speed_name" "$speed_ev" -Do format-hits -FormatBatch "$i" "${speed_args[@]}" ;;
      esac
      simck "$speed_name" 0 '+verified the formats heading' '-LOAD_UNCONFIRMED'
      speed_dir="$tmp/sim-$speed_name"
      speed_next="$(sed -n 's/^FORMAT CONTINUATION //p' "$speed_dir/out")"
      if [ "$speed_pass" != before ] && [ -z "$speed_next" ]; then no "$speed_name: missing session continuation"; fi
      speed_elapsed="$(cat "$speed_dir/clock-ms" 2> /dev/null)"
      [[ "$speed_elapsed" =~ ^[0-9]+$ ]] || { no "$speed_name: missing fake clock"; speed_elapsed=0; }
      speed_ms=$((speed_ms + speed_elapsed))
      speed_loads=$((speed_loads + $(grep -c '^Load#' "$speed_dir/presses")))
    done
    speed_totals+=("$speed_ms"); speed_attempts+=("$speed_loads")
  done
  sim fmt-speed-start-before format-start -Do start
  simck fmt-speed-start-before 0 '+closeagent: 15000 ms'
  speed_boot_before="$(sed -n 's/^closeagent: \([0-9][0-9]*\) ms$/\1/p' "$tmp/sim-fmt-speed-start-before/out")"
  speed_boot_after="$(sed -n 's/^closeagent: \([0-9][0-9]*\) ms$/\1/p' "$tmp/sim-fmt-ready/out")"
  # The mocked controller above checks four batches per mode with one start, versus four starts.
  speed_before=$((speed_totals[0] + speed_totals[2] + 8 * speed_boot_before))
  speed_after=$((speed_totals[1] + speed_totals[2] + 2 * speed_boot_after))
  echo "format-speed: 16 rows per mode, ${speed_before}ms -> ${speed_after}ms, probe Loads ${speed_attempts[0]} -> ${speed_attempts[1]}, cache Loads ${speed_attempts[2]}"
  [ "${speed_attempts[*]}" = '28 16 16' ] && [ "$speed_after" -lt "$speed_before" ] &&
    ok || no "format speed: expected three eliminated probe retries per batch and a lower elapsed time"
  # --- end format speed model
  # --- format chunk wait model
  measure_format_wait() {
    local name="$1" mode="$2" events="$3" batch="${4:-1}" args=()
    if [ "$#" -ge 4 ]; then shift 4; else shift 3; fi
    format_fixture "$name" 3 "${speed_rows[@]}"
    if [ "$mode" = probe ]; then
      kcsim "$name" format-model -Do format-plan -FormatBatch "$batch" -FormatRun "${speed_ids[batch-1]}"
      simck "$name" 0 "+FORMAT PLAN ${speed_batches[batch-1]}"
      args+=(-Do format-run -FormatRun "${speed_ids[batch-1]}" -Rows "${speed_batches[batch-1]}")
    else
      args+=(-Do format-hits)
    fi
    RBKIT_TIMING=1 kcsim "$name" "format-model,measure-clock,chunk-wait,$events" -FormatBatch "$batch" -StartWait 4 "${args[@]}" "$@"
  }
  wait_ms() { cat "$tmp/sim-$1/clock-ms"; }
  # BSR1
  for delay in 1000 2500 5000; do
    name="fmt-probe-$delay"
    measure_format_wait "$name" probe "probe-delay-$delay"
    simck "$name" 0 '+load-completion-margin ' '-load-quiet-wait ' '-STOP'
    evidence="$tmp/sim-$name/rbst/t2/format-${speed_ids[0]}"
    [ -f "$evidence/complete.txt" ] && [ "$(wc -l < "$evidence/rows.txt")" -eq 12 ] &&
      [ "$(grep -c ' Run demucs ' "$tmp/sim-$name/probe.log")" -eq 12 ] &&
      ok || no "$name did not collect all trailing never_cached chunks and their Run lines before sealing"
  done
  # End BSR1
  # BR11: the writer's once-per-process disk report can arrive after the tail.
  measure_format_wait fmt-cache-disk-free cache cache-disk-free
  simck fmt-cache-disk-free 0 '+load-completion-margin ' '-load-quiet-wait ' '-STOP'
  [ "$(grep -c 'CACHE_HIT exact=3 miss=1' "$tmp/sim-fmt-cache-disk-free/out")" -eq 4 ] &&
    ok || no 'the late disk report prevented a cache row from completing'
  for event in cache-pass-through cache-rebuild-failed; do
    measure_format_wait "fmt-$event" cache "$event"
    simck "fmt-$event" 1 '+late bridge activity during the completion margin' '+load-completion-margin ' '-CACHE_HIT' '-Load#2'
  done
  # BSR2
  for delay in 500 2300 5000 14000; do
    name="fmt-cache-$delay"
    # The 14 s replay extends the quiet budget so it can observe that tail.
    measure_format_wait "$name" cache "cache-delay-$delay" 1 -Quiet 16
    simck "$name" 0 '+load-completion-margin ' '-load-quiet-wait ' '-STOP'
    [ "$(grep -c 'CACHE_HIT exact=3 miss=1' "$tmp/sim-$name/out")" -eq 4 ] &&
      ok || no "$name did not attribute each delayed miss to its own row"
    elapsed=$((2000 + (500 - delay % 500) % 500))
    [ "$(cat "$tmp/sim-$name/cache-margin-ms" 2> /dev/null)" = "$(printf '%s\n' "$elapsed" "$elapsed" "$elapsed" "$elapsed")" ] &&
      ok || no "$name did not start the margin at the first poll after the tail"
  done
  measure_format_wait fmt-cache-no-tail cache cache-no-tail
  simck fmt-cache-no-tail 0 '+load-quiet-wait ' '-load-completion-margin ' '-STOP'
  [ "$(grep -c 'CACHE_HIT exact=3 miss=0' "$tmp/sim-fmt-cache-no-tail/out")" -eq 4 ] &&
    ok || no 'the cache row without a tail did not complete quietly'
  for event in cache-double-tail cache-known-after-tail; do
    measure_format_wait "fmt-$event" cache "$event"
    simck "fmt-$event" 1 '+late bridge activity after the cache tail' '-CACHE_HIT' '-Load#2'
  done
  # End BSR2
  # BSR8
  measure_format_wait fmt-chunk-repeat cache chunk-repeat
  simck fmt-chunk-repeat 0 "+${speed_rows[0]} CACHE_HIT exact=3 miss=1" '-STOP'
  grep -qE "^TIMING windows load-quiet-wait .* row=${speed_rows[0]} attempt=1$" "$tmp/sim-fmt-chunk-repeat/out" &&
    ! grep -qE "^TIMING windows load-completion-margin .* row=${speed_rows[0]} attempt=1$" "$tmp/sim-fmt-chunk-repeat/out" &&
    ok || no 'a row with repeated chunk hashes entered the completion margin'
  # End BSR8
  for mode in probe cache; do
    name="fmt-chunk-$mode"
    measure_format_wait "$name-quiet" "$mode" quiet-only
    measure_format_wait "$name-fast" "$mode" ''
    simck "$name-quiet" 0 '+load-quiet-wait '
    simck "$name-fast" 0 '+load-completion-margin ' '-load-quiet-wait '
    [ "$(($(wait_ms "$name-quiet") - $(wait_ms "$name-fast")))" = "$([ "$mode" = probe ] && echo 112000 || echo 32000)" ] &&
      ok || no "$mode did not replace the quiet period with exactly 2 s after the last required chunk"

    measure_format_wait "$name-cross" "$mode" chunk-cross
    simck "$name-cross" 1 '+another planned row during Load' '+Load#1' '-Load#2' '-000000000021' \
      "-${speed_rows[0]} KEYS" "-${speed_rows[0]} CACHE_HIT"
    measure_format_wait "$name-late" "$mode" chunk-late
    simck "$name-late" 1 '+late bridge activity' '+load-completion-margin ' '-Load#2' \
      "-${speed_rows[0]} KEYS" "-${speed_rows[0]} CACHE_HIT"
    if [ "$mode" = probe ]; then
      for ev in cross late; do
        evidence="$tmp/sim-$name-$ev/rbst/t2/format-${speed_ids[0]}"
        [ ! -s "$evidence/rows.txt" ] && [ ! -e "$evidence/complete.txt" ] &&
          ok || no "probe $ev activity wrote evidence or sealed the run"
      done
    fi
  done

  # OI-81: KEY and Run pair per thread. Another model's runs on a thread with no KEY pending are
  # counted and ignored; a stems-model run with nothing pending on its thread still stops.
  measure_format_wait fmt-embed-other-thread probe embed-other-thread
  simck fmt-embed-other-thread 0 "+${speed_rows[0]} KEYS" "+${speed_rows[3]} KEYS" '-STOP' '-Run without KEY' \
    "+formats: ${speed_rows[0]} ignored 3 Run line(s) with no KEY on their thread, not of the stems model: cnn_embedding.onnx x1, model_genre.onnx x2"
  [ "$(grep -c "^${speed_rows[0]} " "$tmp/sim-fmt-embed-other-thread/rbst/t2/format-${speed_ids[0]}/rows.txt")" = 3 ] &&
    ok || no 'ignored embedding runs changed the first row'"'"'s keys'
  measure_format_wait fmt-stray-same-thread probe stray-same-thread
  simck fmt-stray-same-thread 1 '+late bridge activity during probe Load (Run without KEY)' '+Load#1' '-Load#2' "-${speed_rows[0]} KEYS"
  for ev in chunk-refused chunk-unknown; do
    name="fmt-chunk-$ev"
    measure_format_wait "$name" probe "$ev"
    simck "$name" 0 '+load-quiet-wait ' "+${speed_rows[0]} KEYS"
    [ "$(($(wait_ms "$name") - $(wait_ms fmt-chunk-probe-fast)))" = 28000 ] &&
      ok || no "$ev did not keep the 30 s quiet wait for the first row only"
  done
  measure_format_wait fmt-chunk-partial cache chunk-partial
  simck fmt-chunk-partial 0 '+load-quiet-wait ' '-load-completion-margin ' "+${speed_rows[0]} CACHE_HIT exact=2 miss=1"
  [ "$(wait_ms fmt-chunk-partial)" = "$(wait_ms fmt-chunk-cache-quiet)" ] &&
    ok || no 'an unexpected cache miss replaced the quiet wait without every expected exact hit'

  chunk_totals=()
  for events in quiet-only fast; do
    total=0
    for mode in probe cache; do
      for ((i=1; i<=4; i++)); do
        name="fmt-chunk-16-$mode-$events-$i"
        measure_format_wait "$name" "$mode" "$events" "$i"
        simck "$name" 0 '+Load#4' '-Load#5' '-LOAD_UNCONFIRMED'
        total=$((total + $(wait_ms "$name")))
      done
    done
    chunk_totals+=("$total")
  done
  echo "format-chunk-wait: 16 rows per mode, ${chunk_totals[0]}ms -> ${chunk_totals[1]}ms"
  [ "$((chunk_totals[0] - chunk_totals[1]))" = 576000 ] && ok || no 'format chunk wait: unexpected 16-row reduction'
  # --- end format chunk wait model
  # --- format navigation model
  # Within a batch, a confirmed row is followed by one Down in the track list. The batch's first
  # row starts from the full reset with the heading check, also later in the same session.
  nav_timing() {   # NAME ROW ATTEMPT MODE: the row's navigation timing names that nav mode
    grep -qE "^TIMING windows midi-navigation elapsed_ms=[0-9]+ row=$2 attempt=$3 nav-mode=$4\$" "$tmp/sim-$1/out" &&
      ok || no "format navigation $1: $2 attempt $3 was not timed as $4"
  }
  nav_headings() {   # NAME COUNT: heading checks in the run
    [ "$(grep -c 'verified the formats heading' "$tmp/sim-$1/out")" = "$2" ] &&
      ok || no "format navigation $1: expected $2 heading check(s)"
  }
  format_fixture fmt-nav-relative 3 "${speed_rows[@]}"
  RBKIT_TIMING=1 kcsim fmt-nav-relative 'format-model,bottom' -Do format-hits -FormatPass -StartWait 1
  simck fmt-nav-relative 0 "+${speed_rows[0]} CACHE_HIT" "+${speed_rows[3]} CACHE_HIT" '+AreaLeft#1' '+Up#3 x16' \
    '+Down#3 x1' '+Down#4 x1' '+Down#5 x1' '-Down#6' '-Up#4' '+AreaLeft#4 x5' '-AreaLeft#5' '+AreaRight#8 x1' '-AreaRight#9' '+Load#4' '-Load#5'
  nav_timing fmt-nav-relative "${speed_rows[0]}" 1 reset
  for ((i=1; i<4; i++)); do nav_timing fmt-nav-relative "${speed_rows[i]}" 1 relative; done
  nav_headings fmt-nav-relative 1
  continuation="$(sed -n 's/^FORMAT CONTINUATION //p' "$tmp/sim-fmt-nav-relative/out")"
  RBKIT_TIMING=1 kcsim fmt-nav-relative 'format-model,bottom' -Do format-hits -FormatBatch 2 -FormatPass \
    -FormatContinuation "$continuation" -StartWait 1
  simck fmt-nav-relative 0 "+${speed_rows[4]} CACHE_HIT" "+${speed_rows[7]} CACHE_HIT" '+AreaLeft#1' '+Up#3 x16' \
    '+Down#3 x4' '+Down#4 x1' '+Down#6 x1' '-Down#7' '-Up#4' '+AreaLeft#4 x5' '-AreaLeft#5' '+Load#4' '-Load#5'
  nav_timing fmt-nav-relative "${speed_rows[4]}" 1 reset
  for ((i=5; i<8; i++)); do nav_timing fmt-nav-relative "${speed_rows[i]}" 1 relative; done
  nav_headings fmt-nav-relative 1
  # A lost relative Down reloads the row above. That Load does not count: one full reset with
  # the heading check, then the retry. Two failed readbacks leave the row without evidence.
  for nav in retry unconfirmed; do
    n="fmt-nav-$nav"
    format_fixture "$n" 3 "${speed_rows[@]}"
    kcsim "$n" format-model -Do format-plan -FormatRun "${speed_ids[0]}"
    simck "$n" 0 "+FORMAT PLAN ${speed_batches[0]}"
    ev='format-model,Down#4=lose'
    [ "$nav" = retry ] || ev="$ev,Load#4=unreadable"
    RBKIT_TIMING=1 kcsim "$n" "$ev" -Do format-run -FormatRun "${speed_ids[0]}" -Rows "${speed_batches[0]}" -StartWait 1
    simck "$n" 0 "+${speed_rows[2]} deck identity did not match (deck 1 read as ${speed_rows[1]}); retrying from the top" '+AreaLeft#2' '+Up#6 x16' \
      '+Down#7 x2' "+${speed_rows[1]} KEYS" "+${speed_rows[3]} KEYS" '+Load#5' '-Load#6'
    nav_timing "$n" "${speed_rows[2]}" 1 relative
    nav_timing "$n" "${speed_rows[2]}" 2 reset
    evidence="$tmp/sim-$n/rbst/t2/format-${speed_ids[0]}"
    [ "$(grep -c "^${speed_rows[2]} " "$evidence/rows.txt")" = "$([ "$nav" = retry ] && echo 1 || echo 0)" ] &&
      ok || no "format navigation $n: unexpected evidence for ${speed_rows[2]}"
  done
  simck fmt-nav-retry 0 "+${speed_rows[2]} KEYS" '+Down#8 x1' '-Down#9' '+AreaLeft#5' '-AreaLeft#6'
  nav_timing fmt-nav-retry "${speed_rows[3]}" 1 relative
  nav_headings fmt-nav-retry 2
  simck fmt-nav-unconfirmed 0 "+${speed_rows[2]} LOAD_UNCONFIRMED" '+AreaLeft#5' '-AreaLeft#6' '+Up#9 x16' '+Down#10 x3' '-Down#11' \
    "-${speed_rows[2]} KEYS"
  nav_timing fmt-nav-unconfirmed "${speed_rows[3]}" 1 reset
  nav_headings fmt-nav-unconfirmed 3
  grep -qx "${speed_rows[2]} LOAD_UNCONFIRMED" "$tmp/sim-fmt-nav-unconfirmed/rbst/t2/format-${speed_ids[0]}/outcomes.txt" &&
    ok || no "format navigation: the unconfirmed row's outcome was not recorded"
  # OI-82: on the VM a Load left rekordbox's browser off the track list, so the next Browse Down
  # did not move it and every relative Load reloaded the row above. The relative step re-enters
  # the list (Area Left x5, Area Right x2) before its Down: no retry, no extra heading check.
  format_fixture fmt-nav-reenter 3 "${speed_rows[@]}"
  kcsim fmt-nav-reenter format-model -Do format-plan -FormatRun "${speed_ids[0]}"
  RBKIT_TIMING=1 kcsim fmt-nav-reenter 'format-model,load-leaves-list' -Do format-run -FormatRun "${speed_ids[0]}" \
    -Rows "${speed_batches[0]}" -StartWait 1
  simck fmt-nav-reenter 0 "+${speed_rows[1]} KEYS" "+${speed_rows[2]} KEYS" "+${speed_rows[3]} KEYS" \
    '-deck identity did not match' '+AreaLeft#4 x5' '-AreaLeft#5' '+AreaRight#8 x1' '-Up#4' '+Load#4' '-Load#5'
  for ((i=1; i<4; i++)); do nav_timing fmt-nav-reenter "${speed_rows[i]}" 1 relative; done
  nav_headings fmt-nav-reenter 1
  # --- end format navigation model
  rm -rf "$k/corpus/formats" "$k/bench"
else
  echo "SKIP: no pwsh: the kit's PowerShell not parsed, the title match and the watch not run"
fi

# --- a copy of the toolkit in its own git repository (its local config is the test's own)
r="$tmp/repo"; mkdir -p "$r/tests/rekordbox" "$r/tests/win"
cp -R "$here" "$r/tests/rekordbox/win"; cp -R "$here/../analysis" "$r/tests/rekordbox/analysis"
cp "$repo/tests/win/vm-run.sh" "$r/tests/win/"
git -C "$r" init -q
S="$r/tests/rekordbox/win/pc-session.sh"
F="$tmp/fake"; mkdir -p "$F/bin"
# ssh: the call and its decoded PowerShell logged (LOGIN<TAB>SCRIPT); answers by what the script
# does, from $FAKE/<name> files when a case sets them (NAME-out: its output, NAME-rc: its exit)
cat > "$F/bin/ssh" << 'EOF'
#!/bin/bash
login="${@: -2:1}"; remote="${@: -1}"
# the sign-in's tunnel (ssh -N -L): logged, held open briefly
case " $* " in *' -N '*) printf '%s\ttunnel\t%s\n' "$login" "$*" >> "$FAKE/calls"; exec perl -e 'sleep 2' ;; esac
ps="$(printf '%s' "${remote##* }" | base64 -D | iconv -f UTF-16LE -t UTF-8)"
# nothing may come on stdin (/dev/null or a terminal is fine; a pipe or a file with bytes is not)
if [ -p /dev/stdin ] || [ -f /dev/stdin ]; then
  [ -z "$(cat)" ] || printf '%s\t%s\n' "$login" "${ps:0:60}" >> "$FAKE/stdin-used"
fi
# a kit script for the owner's login (kitps): the encoded runner command names the runner and the
# script with their SHA-256; the admin login's folder is $FAKE/adminfs (filled by scp). As the real
# command: a missing or changed file is refused (exit 9), else the runner gets the prelude and script
case "$ps" in
  '# rbkit-runner'*)
    for nh in $(printf '%s\n' "$ps" | grep -o "Get-KitFile '[^']*' '[0-9a-f]*'" | sed "s/Get-KitFile '\([^']*\)' '\([0-9a-f]*\)'/\1:\2/"); do
      n="${nh%%:*}"; h="${nh##*:}"
      [ -f "$FAKE/adminfs/$n" ] || { printf '%s\t%s\tRBKIT missing %s\n' "$login" "$*" "$n" >> "$FAKE/calls"; echo "RBKIT: missing $n"; exit 9; }
      [ "$(shasum -a 256 < "$FAKE/adminfs/$n" | cut -d' ' -f1)" = "$h" ] ||
        { printf '%s\t%s\tRBKIT mismatch %s\n' "$login" "$*" "$n" >> "$FAKE/calls"; echo "RBKIT: hash mismatch $n"; exit 9; }
    done
    script="$(printf '%s\n' "$ps" | sed -n "s/.*\$s = Get-KitFile '\([^']*\)'.*/\1/p")"
    prelude="$(printf '%s\n' "$ps" | sed -n "s/.*FromBase64String('\([^']*\)').*/\1/p" | base64 -D)"
    if [ "${FAKE_CAPTURE_RUNNER:-0}" = 1 ]; then
      printf '%s\n' "$ps" > "$FAKE/boot.ps1"
      printf '%s\n' "$remote" > "$FAKE/boot-command"
      printf '%s\n' "$prelude" > "$FAKE/prelude.ps1"
      { printf '%s\n' "$prelude"; cat "$FAKE/adminfs/$script"; } > "$FAKE/runner.ps1"
    fi
    ps="$({ printf '%s\n' "$prelude"; cat "$FAKE/adminfs/$script"; } | tr '\n\t' '  ')" ;;
  *"'rbsp-kit-admin'"*'New-Item -ItemType Directory'*) printf '%s\tmkdir\n' "$login" >> "$FAKE/calls"; exit 0 ;;
esac
printf '%s\t%s\t%s\n' "$login" "$*" "$ps" >> "$FAKE/calls"
[ -z "${FAKE_RC:-}" ] || exit "$FAKE_RC"
# answer NAME DEFAULT: $FAKE/NAME-seq's first line "RC OUTPUT" (taken off), else $FAKE/NAME-out
# (else DEFAULT) printed, exit $FAKE/NAME-rc (else 0)
answer() {
  if [ -s "$FAKE/$1-seq" ]; then
    local l; l="$(head -1 "$FAKE/$1-seq")"; tail -n +2 "$FAKE/$1-seq" > "$FAKE/$1-seq.n"; mv "$FAKE/$1-seq.n" "$FAKE/$1-seq"
    printf '%b\n' "${l#* }"; exit "${l%% *}"
  fi
  if [ -f "$FAKE/$1-out" ]; then cat "$FAKE/$1-out"; else printf '%b' "$2"; fi; exit "$(cat "$FAKE/$1-rc" 2> /dev/null || echo 0)"
}
# Model stderr separately from stdout. The PowerShell model below executes the real trace calls.
if [[ "$ps" == *'[Console]::Error.WriteLine'*"\$Mode = 'up'"* ]]; then
  echo '2026-11-19T10:00:00.0000000Z console: before Add-Type' >&2
fi
case "$ps" in
  *'# cleanup-runners.ps1:'*) answer cleanup-runners '' ;;
  *'ResolveOnly = $true'*'# probe.ps1:'*)
    version="${ps#*"\$RbVersion = '"}"; version="${version%%\'*}"
    answer probe-target "PROBE TARGET ${version:-7.2.19}\n" ;;
  *"\$EvidenceRun = '"[0-9a-f]*"\$Mode = 'install'"*'NoDumps = $true'*'# probe.ps1:'*)
    if [ -f "$FAKE/probe-already-installed-out" ]; then printf '%s\n' 'REFUSED: scoped runs require a fresh probe evidence directory'; exit 2; fi
    answer probe-install 'PROBE probe (rekordbox 7.2.19; 0 mix dumps)\n' ;;
  *"\$Mode = 'install'"*'# probe.ps1:'*) answer probe-install 'PROBE probe (rekordbox 7.2.19; 0 mix dumps)\n' ;;
  *"\$Mode = 'remove'"*'# probe.ps1:'*) answer probe-remove 'PROBE pioneer (rekordbox 7.2.19; 0 mix dumps)\n' ;;
  *"\$Mode = 'state'"*'# probe.ps1:'*) answer probe-state 'PROBE pioneer (rekordbox 7.2.19; 0 mix dumps)\n' ;;
  *"\$Mode = 'up'"*) answer console-up 'SESSION 2 test Active console\nCONSOLE ok 2\n' ;;
  *"\$Mode = 'state'"*) answer console-state 'CONSOLE ok 2\n' ;;
  *"\$Mode = 'down'"*) answer console-down 'SESSION 2 test Disconnected\nCONSOLE down\n' ;;
  *"\$Check = \$true"*) answer worker-check 'worker: C:\\x\\RB Stems Plus.exe\nWORKER ready (not started)\n' ;;
  # the worker changes rekordbox's DLL when it succeeds (no worker-rc/worker-out set by the case)
  *"\$Verb = '"*)
    if [ ! -f "$FAKE/worker-rc" ] && [ ! -f "$FAKE/worker-out" ]; then
      case "$ps" in *"\$Verb = 'install'"*) echo 'other 33d96af6' ;; *) echo pioneer ;; esac > "$FAKE/ort-state"
    fi
    case "$ps" in
      *"\$Verb = 'install'"*) target='worker: rekordbox 7.2.19: installed (exit 0)' ;;
      *) target='worker: rekordbox 7.2.19: put-back (exit 0)' ;;
    esac
    answer worker "worker: C:\\\\x\\\\RB Stems Plus.exe\n$target\nWORKER exit 0\n" ;;
  *"-Step 'm12-"*)
    action="${ps#*"-Step 'm12-"}"; action="${action%%\'*}"
    printf '%s %s\n' "$action" "$(cat "$FAKE/share/serve-current.txt")" >> "$FAKE/served"
    answer "m12-$action" "x  m12-$action: OK\n" ;;
  *"-Step 't2-model-"*) answer t2-model 'x  t2 model OK\n' ;;
  *'-Do t2-facts'*) answer t2-facts 'x  T2 item 8: PartAnalysisQuality=0 AutoAnalysis=0\n' ;;
  *'-Do t2-probe'*) answer t2-probe 'x  T2 probe: SetIntraOpNumThreads=8\n' ;;
  *'-Do modules'*) answer modules 'x  modules: DirectML loaded\n' ;;
  *'-Do preconditions'*) answer preconditions 'x  preconditions: all met (0 put right by the plan itself)\n' ;;
  *'-Do autoanalysis-read'*) answer autoanalysis-read 'x  autoanalysis: rekordbox3.settings AutoAnalysis=0\n' ;;
  *'-Do autoanalysis'*) answer autoanalysis 'x  autoanalysis: ok; Auto Analysis was On; now Off\n' ;;
  *'Get-AppSwapCacheState'*) answer swap-cache 'clear\n' ;;
  *'-Do appswap'*) answer appswap 'x  x appinstall: ok; release 1.4.0\n' ;;
  *'-Do loc'*) answer loc 'x  loc: x loc set: ok\n' ;;
  *'-Do rowkeys'*) answer rowkeys 'x  rowkeys: key 23ad8b346b2f version 0 chunk 0 silent 0\n' ;;
  *'-Do format-step'*) answer format-step 'format-step: 1 element(s)\n{"Name":"rekordbox","Depth":0,"Capture":"main"}\n' ;;
  *'-Do format-uidump'*) answer format-uidump 'format-uidump: 1 element(s)\n{"Name":"rekordbox","Depth":0}\n' ;;
  *'-Do winlist'*) answer winlist '{"Processes":[123],"Windows":[{"Handle":"0x123A","Caption":"<redacted>"}]}\nWINLIST SHOT C:\\Users\\rbtest\\AppData\\Local\\Temp\\rbst\\shots\\winlist-0123456789abcdef0123456789abcdef-0x123A.png\n' ;;
  *'-Do library-check'*) answer library-check 'LIBRARY CHECK C:\\Users\\rbtest\\AppData\\Local\\Temp\\rbst\\library-check-0123456789abcdef0123456789abcdef.json\n' ;;
  *'-Do format-import'*) answer format-import '' ;;
  *'-Do format-reuse'*)
    if [ -f "$FAKE/format-reuse-rc" ]; then answer format-reuse ''; fi
    printf 'FORMAT REUSE NONE\n'; exit 4 ;;
  *'-Do format-verify'*) answer format-verify 'formats: fmt-e IMPORT_REFUSED\nformats: fmt-f IMPORT_ADDED\n' ;;
  *'-Do format-plan '*)
    answer format-plan 'FORMAT PLAN fmt-e,fmt-f
' ;;
 *'-Do format-hits '*)
    [[ "$ps" != *' -FormatPass '* ]] || printf 'FORMAT CONTINUATION 123:456/10:789,absent\n'
    answer format-hits 'formats: fmt-e CACHE_HIT exact=1 miss=1 refused=0 near=0' ;;
 *'-Do format-run '*)
    [[ "$ps" != *' -FormatPass '* ]] || printf 'FORMAT CONTINUATION 123:456/10:789,absent\n'
    answer format-run 'formats: fmt-e IMPORT_REFUSED
formats: fmt-f LOAD_UNCONFIRMED
' ;;
  *'-Do keycompare '*'-FormatRun '*)
    id="$(printf '%s' "$ps" | sed -n "s/.*-FormatRun '\([a-f0-9]*\)'.*/\1/p")"
    path='C:\Users\rbtest\AppData\Local\Temp\rbst\t2'
    if [ -f "$FAKE/keycompare-mixed" ]; then printf '<S N="Source">rbsp-kit\\ki' >&2; fi
    printf '%s\n' "FORMAT REPORT $path\format-$id\keycompare.tsv"
    exit "$(cat "$FAKE/keycompare-rc" 2> /dev/null || echo 0)" ;;
  *'-Do keycompare'*) answer keycompare 'x  keycompare: evidence 1234567890ab; 2 OK, 0 not OK (report: C:\x\keycompare.tsv)\n' ;;
  *"'format-stage.ps1') -Mode 'prepare'"*) answer format-prepare 'FORMATS prepare\n' ;;
  *"'format-stage.ps1') -Mode 'publish'"*) answer format-publish 'FORMATS publish\n' ;;
  *"'format-stage.ps1') -Mode 'discard'"*) answer format-discard '' ;;
  *"'format-stage.ps1') -Mode 'clear'"*) answer format-clear 'FORMATS clear\n' ;;
  *'function CheckStage'*)
    if [ -f "$FAKE/format-path-rc" ]; then answer format-path 'formats: reparse point in destination\n'; fi
    answer temp 'C:\Users\rbtest\AppData\Local\Temp\r\n' ;;
  *'-Do keptinfo'*) answer keptinfo 'x  keptinfo: x holds 2 item(s)\n' ;;
  *'-Do stopkeep'*) answer stopkeep "x  x stopkeep: ok; 'RBSP Tests' no longer kept\n" ;;
  *'-Do playlists'*) answer playlists "x  playlists: clean environment: exit 0 in 1.2 s; 4 stdout line(s); 'RBSP Tests' listed 1; stderr: (none)\n" ;;
  *'-Do appmin'*) answer appmin 'x  x appmin: minimized True\n' ;;
  *'-Do appcheck'*) answer appcheck "x  x appcheck: title 'RB Stems Plus'; tabs 1: Manage; black 0.02 of the window; shot x\n" ;;
  *'-Do appexit'*) answer appexit "x  x appexit: ok; closed by the menu's RB Stems Plus > Exit in 1 s\n" ;;
  *'-Do appopen'*) answer appopen 'x  appopen: x app: running, its window up (pid 9)\n' ;;
  *'-Do dmlbench'*) answer dmlbench 'x  dmlbench: dml: exit 0; 2 file(s) prepared; 20 chunks; ep dml\n' ;;
  *'-Do dmlstall'*) answer dmlstall 'x  dmlstall: deck 1: 9 of 9 readouts read; 0 stalled step(s)\n' ;;
  *'installed-release\payload.json'*) answer payload 'none\n' ;;
  *"'reached '"*) answer reached 'reached rbtest\n' ;;
  *'-Do run '*)
    if [ -f "$FAKE/stall" ]; then exec perl -e 'sleep 3'; fi
    if [ -f "$FAKE/chatty" ]; then exec perl -e '$|=1; for (1..6) { print "beat "; select undef,undef,undef,0.3 } print "done\n"'; fi
    answer run '' ;;
  *'-Do quit'*)
    if [ -f "$FAKE/format-quit-sim" ] && [ "$(grep -c -- '-Do quit' "$FAKE/calls")" = 2 ]; then
      IFS= read -r sim_kit < "$FAKE/format-quit-sim"
      d="$FAKE/quit-sim"
      args=(-Do quit)
      [[ "$ps" != *'-Do quit -Formats '* ]] || args+=(-Formats)
      SIM="$d" SIM_EV= TEMP="$d" APPDATA="$d" LOCALAPPDATA="$d" \
        pwsh -NoProfile -NonInteractive -File "$sim_kit/midi-auto.ps1" "${args[@]}"
      exit $?
    fi
    answer quit 'x  quit: rekordbox closed after 3 s\n' ;;
  *'-Do logoff'*) answer logoff 'x  logoff: signing out\n' ;;
  *'-Do ensure'*) answer ensure 'x  ensure: 0 not OK\n' ;;
  *'-Do version '*"-RbVersion '7.2.19'"*) answer version 'REKORDBOX 7.2.19\n' ;;
  *'-Do version'*)
    if [ -f "$FAKE/version-warning" ]; then printf '%s\n' '** WARNING: connection is not using a post-quantum key exchange algorithm.' >&2; fi
    answer version 'REKORDBOX 7.2.20\n' ;;
  *'-Do watchtest'*) answer watchtest 'x  watchtest: STOP: new window(s): powershell | RBSP watch test | 0x50 (watch test); shot x\nx  watchtest: PASS\n' ;;
  *'-Do bundled '*) [[ "$ps" =~ collect-[0-9]{8}-[0-9]{6}\.zip ]] || exit 2 ;;
  *'-Do bundle'*) printf 'BUNDLE C:\\Users\\rbtest\\AppData\\Local\\Temp\\rbst\\collect-20261118-100000.zip\r\n' ;;
  *'verify-kit.ps1'*) answer verify 'SHA256SUMS: 9 files present, 0 bad\n' ;;
  *'Get-OrtState'*) answer ort "$(cat "$FAKE/ort-state" 2> /dev/null || echo pioneer)\n" ;;
  *"rbstate.ps1') -Mode Snapshot"*) answer snap 'SNAPSHOT 1\nexit 0\nINVENTORY a\nexit 0\n' ;;
  *'-Do uninstallcache'*) answer uninstall '' ;;
  *'Remove-LocalGroupMember'*) answer admin 'admin 0\n' ;;
  *"\$Phase = 'check'"*'# t5-app.ps1:'*) answer t5check "T5: verified 'Samsung Portable SSD T5' on USB (disk 2)\n" ;;
  *"\$Phase = 'rows'"*'# t5-app.ps1:'*) answer t5 "J28 D:\\rbsp-test\\: facts volume: fs=exfat drive=fixed\nCLEAN: the listing matches (our folder gone: True)\n" ;;
  *'# t5-app.ps1:'*) answer t5app "T5: volume D:\\ 'T5' exFAT, 400 GB free\nT5FOLDER D:\\rbsp-test\n" ;;
  *"chosen.ps1') -Mode set"*) answer chosenset 'chosen: the kit set cache=1 (model=0 kept) in C:\\x\\installed; it was absent\n' ;;
  *"-Mode restore"*) answer chosenrestore 'chosen: nothing the kit set\n' ;;
  *'# Read the requested drive on the verified PC.'*) answer disk-probe 'DISK E: type Removable bus unknown ready False fs none\n' ;;
  *"'sid '"*) answer ident 'sid S-1-5-21-11-22-33-1001\npc TESTPC\nadmin 1\n' ;;
  *quser*) answer quser 'signed out\n' ;;
  *'; $env:TEMP'*) answer temp 'C:\\Users\\rbtest\\AppData\\Local\\Temp\r\n' ;;
esac
exit 0
EOF
# scp: logged; a copy from the PC gets a small zip
cat > "$F/bin/scp" << 'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$FAKE/scp"
last="${@: -1}"
# to the admin login's kit folder: the files land in $FAKE/adminfs
case "$last" in *:rbsp-kit-admin/)
  mkdir -p "$FAKE/adminfs"; for a in "${@:1:$#-1}"; do [ -f "$a" ] && cp "$a" "$FAKE/adminfs/"; done; exit 0 ;;
esac
case "$*" in
  *formats-incoming-*) exit "$(cat "$FAKE/format-upload-rc" 2> /dev/null || echo 0)" ;;
  *winlist-0123456789abcdef0123456789abcdef-0x123A.png*) touch "$last/winlist-0123456789abcdef0123456789abcdef-0x123A.png"; exit $? ;;
  *library-check-0123456789abcdef0123456789abcdef.json*) printf '[]\n' > "$last/library-check-0123456789abcdef0123456789abcdef.json"; exit 0 ;;
  *keycompare.tsv*)
    printf 'row\tverdict\n' > "$last/keycompare.tsv"
    if [[ "$*" == *chunk-hashes.tsv* ]]; then printf 'row\tsource\thash\tsilent\n' > "$last/chunk-hashes.tsv"; fi
    if [[ "$*" == *helper-stderr.log* ]]; then printf '%s\n' 'filtered helper stderr' > "$last/helper-stderr.log"; fi
    exit 0 ;;
  *C:/rbtest/rows.txt*)
    [ ! -f "$FAKE/probe-fetch-rc" ] || exit "$(cat "$FAKE/probe-fetch-rc")"
    if [[ "$*" == *C:/rbtest/dump* ]]; then cp -R "$FAKE/probe-fetch/." "$last/"
    elif [[ "$*" == *C:/rbtest/probe.log* ]]; then cp "$FAKE/probe-fetch/probe.log" "$FAKE/probe-fetch/rows.txt" "$last/"
    else cp "$FAKE/probe-fetch/rows.txt" "$last/"; fi
    exit $? ;;
esac
[[ "$last" == *:* ]] || cp "$FAKE/collect-20261118-100000.zip" "$last/"
EOF
printf '#!/bin/bash\nprintf "%%s\\n" "$1" >> "$FAKE/notified"\n' > "$F/notify"
printf '#!/bin/bash\nexit 1\n' > "$F/notify-fails"
chmod +x "$F/bin/ssh" "$F/bin/scp" "$F/notify" "$F/notify-fails"
(cd "$tmp" && mkdir z && echo hello > z/midi-auto.log && cd z && zip -q ../fake/collect-20261118-100000.zip midi-auto.log)
export FAKE="$F" RBKIT_NOTIFY="$F/notify" RBKIT_OUT="$tmp/out" RBKIT_POLL=0 PATH="$F/bin:$PATH"
reset() { rm -f "$F/calls" "$F/scp" "$F/notified" "$F"/*-out "$F"/*-rc "$F"/*-seq "$F/ort-state"; }
# expect RC NAME ARGS...: pc-session.sh exits with RC
expect() {
  local want="$1" name="$2"; shift 2
  (
    if [ "${1:-}" = env ]; then
      shift
      while [[ "$1" == *=* ]]; do export "$1"; shift; done
    fi
    "$S" "$@"
  ) > "$tmp/o" 2>&1
  local rc=$?
  if [ "$rc" = "$want" ]; then ok; else no "$name (exit $rc, expected $want)"; sed 's/^/    /' "$tmp/o"; fi
}
calls() { cut -f3- "$F/calls" 2> /dev/null; }
has() { if grep -qF -- "$2" "$3"; then ok; else no "$1: no '$2' in $(basename "$3")"; fi; }
hasnt() { if grep -qF -- "$2" "$3" 2> /dev/null; then no "$1: '$2' in $(basename "$3")"; else ok; fi; }

reset
expect 2 "no arguments" ; expect 2 "unknown command" bogus
expect 2 "run without a kind" run; expect 2 "run with an unknown kind" run T3
expect 2 "--from with a quote" run T2 --from "a'b"; expect 2 "--from with a semicolon" run T2 --from 'a;b'
expect 2 "--max not a number" run T2 --max x; expect 2 "--max 0" run T2 --max 0; expect 2 "--minutes 08" run T2 --minutes 08
expect 2 "--playlist on T2" run T2 --playlist X; expect 2 "--from on prepare" run prepare --from a
expect 2 "--playlist with a quote" run prepare --playlist "a'b"; expect 2 "--from without a value" run T2 --from
expect 2 "--dist on preflight" preflight --dist x; expect 2 "--server not HOST:PORT" preflight --server 'a b'
expect 2 "--label with a space" cleanup --label 'a b'; expect 2 "stage --corpus without v1/win" stage --corpus "$tmp"
expect 2 "no login configured" run T2
git -C "$r" config rbstemsplus.pcSsh '-oProxyCommand=x@pc.example'; expect 2 "pcSsh starting with -" run T2
rog_protection="$rog_example"
usb_protection='VTJTTnxVU0IgRGlzaw=='
git -C "$r" config rbstemsplus.pcProtectedDisks "$rog_protection"
git -C "$r" config rbstemsplus.pcT5Serial 'T5SN'
git -C "$r" config rbstemsplus.pcSsh 'owner@pc.example'; git -C "$r" config rbstemsplus.pcTestServer 'a b:1'
expect 2 "pcTestServer not HOST:PORT" preflight; git -C "$r" config --unset rbstemsplus.pcTestServer
[ ! -s "$F/calls" ] && ok || no "ssh was called for a refused command line"

# --- Windows target selection, through the real vm-run.sh and a fake Parallels CLI
mkdir "$tmp/vm-share"
cat > "$F/bin/fake-parallels" <<'PY'
#!/usr/bin/env python3
import base64
import hashlib
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile

args = sys.argv[1:]
vms = {"rb-win-template": "202a80c8-4bc4-4e95-890e-a31d6bfd5b70",
       "rb-win-hand-rekordbox": "8905c42f-722b-4145-a88b-c0539af7cec6"}
fake = Path(os.environ["FAKE"])
if len(args) == 3 and args[:2] == ["list", "-i"]:
    # vm-run.sh asks the template by its uuid and another VM by its name
    name = next((n for n, u in vms.items() if args[2] in (n, u)), None)
    assert name, args
    hires = "on" if os.environ.get("FAKE_VM_HIRES") == "on" and not (fake / "vm-set").exists() else "off"
    print(f"Name: {name}\nID: {{{vms[name]}}}\nState: running\n"
          f"  video adapter-type=parallels size=0Mb high-resolution={hires} high-resolution-in-guest=off")
    sys.exit(0)
if args[:1] == ["set"]:
    assert len(args) == 4 and args[1] in vms and args[2:] == ["--high-resolution", "off"], args
    with (fake / "vm-set").open("a") as log:
        print(" ".join(args), file=log)
    sys.exit(0)
assert len(args) == 9 and args[0] == "exec" and args[1] in vms.values() and args[2:8] == [
    "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File"
], args
with (fake / "vm-uuids").open("a") as log:
    print(args[1], file=log)
share = Path(os.environ["VMRUN_SHARE"])
guest = fake / "guest"
account_temp = r"C:\Users\rbtest\AppData\Local\Temp"
admin_dir = r"C:\Windows\System32\config\systemprofile\rbsp-kit-admin"

def host(p):
    assert p.startswith("\\\\Mac\\win\\"), p
    return share.joinpath(*p.split("\\")[4:])

def win(p):
    p = p.replace("/", "\\").rstrip("\\")
    assert p.startswith("C:\\"), p
    return guest.joinpath(*p[3:].split("\\"))

def quoted(s):
    return [p.replace("''", "'") for p in re.findall(r"'((?:[^']|'')*)'", s)]

wrapper = host(args[8]).read_text()
mode = "sys"
user = None
if args[8].endswith(".launch.ps1"):
    mode, job, out, task = quoted(wrapper.splitlines()[0])
    assert mode == "user-signed" and out.startswith("C:\\rbkit-vm\\") and task.startswith("rbstems-run-one-")
    assert '$rbkit = $true;' in wrapper and '$user -ne "$env:COMPUTERNAME\\$rbkitUser"' in wrapper
    user = quoted(wrapper.splitlines()[1])[0]
    assert 'Win32_ComputerSystem' in wrapper and 'New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive' in wrapper
    wrapper = host(job).read_text()
step, marker = quoted(wrapper.splitlines()[0])
ps = host(step).read_text(encoding="utf-8-sig")
with (fake / "vm-calls").open("a") as log:
    print(f"{mode}\t" + ps.replace("\n", " "), file=log)
options = os.environ.get("FAKE_VM_CASE")
rc = 0
if "Copy-Item -LiteralPath" in ps:
    for line in ps.splitlines():
        if line.startswith("Copy-Item "):
            src, dest = quoted(line)
            src, dest = host(src), win(dest)
            if src.is_dir():
                shutil.copytree(src, dest / src.name, dirs_exist_ok=True)
            else:
                shutil.copyfile(src, dest / src.name)
elif "& tar.exe -cf " in ps:
    for line in ps.splitlines():
        if line.startswith("& tar.exe -cf "):
            archive, folder, leaf = quoted(line)
            assert " -C " in line and leaf.startswith("./")
            with tarfile.open(host(archive), "w") as tar:
                tar.add(win(folder) / leaf[2:], arcname=leaf)
elif "Get-LocalUser -Name '" in ps:
    assert mode == "sys"
    print("sid S-1-5-21-11-22-33-1001\npc TESTVM\nadmin 0")
elif "New-Item -ItemType Directory" in ps and "'rbsp-kit-admin'" in ps:
    assert mode == "sys"
    win(admin_dir).mkdir(parents=True, exist_ok=True)
    print(admin_dir)
elif "FromBase64String(" in ps:
    assert mode == "sys"
    stub = base64.b64decode(re.search(r"FromBase64String\('([^']+)'\)", ps)[1]).decode("utf-16-le")
    for name, digest in re.findall(r"Get-KitFile '([^']+)' '([a-f0-9]+)'", stub):
        file = win(admin_dir) / name
        if not file.is_file():
            print(f"RBKIT: missing {name}")
            rc = 9
            break
        assert hashlib.sha256(file.read_bytes()).hexdigest() == digest
    if rc == 0:
        prelude = base64.b64decode(re.search(r"FromBase64String\('([^']+)'\)", stub)[1]).decode()
        assert "$env:COMPUTERNAME -ne 'TESTVM'" in prelude
        assert "$Sid = 'S-1-5-21-11-22-33-1001'" in prelude
        if "Get-KitFile 'probe.ps1'" in stub:
            assert "$Mode = 'remove'" in prelude
            with (fake / "vm-probe").open("a") as log:
                print("resolve" if "$ResolveOnly = $true" in prelude else "remove", file=log)
            print("PROBE TARGET 7.2.19" if "$ResolveOnly = $true" in prelude else "PROBE pioneer (rekordbox 7.2.19; 0 mix dumps)")
        else:
            print("CONSOLE ok 2" if "$Mode = 'up'" in prelude else "CONSOLE down")
elif "shutdown.exe /r" in ps:
    assert mode == "sys"
    (fake / "vm-restarted").touch()
elif "Win32_ComputerSystem).UserName" in ps:
    assert mode == "sys" and "'\\rbtest'" in ps
    print("signed in")
else:
    assert mode == "user-signed"
    if "'reached ' + $env:USERNAME" in ps:
        print(f"reached {user}")
    elif "'vm-display.ps1') -Width 1920 -Height 1080" in ps:
        dpi = os.environ.get("FAKE_VM_DPI", "96")
        print(f"DISPLAY 1920x1080 dpi {dpi}")
        rc = 0 if dpi == "96" else 3
    elif "; $env:TEMP;" in ps:
        if options == "retry" and not (fake / "vm-retried").exists():
            (fake / "vm-retried").touch()
            print("PrlJob_GetResult/GetRetCode: Invalid argument")
            sys.exit(1)
        win(account_temp + r"\rbsp-kit").mkdir(parents=True, exist_ok=True)
        print(account_temp)
    elif "'format-stage.ps1'" in ps:
        transfer = re.search(r"-Transfer '([a-f0-9]{32})'", ps)[1]
        corpus = win(account_temp + r"\rbsp-kit\corpus")
        pending = corpus / f"formats-incoming-{transfer}"
        if "-Mode 'prepare'" in ps:
            pending.mkdir(parents=True)
        elif "-Mode 'publish'" in ps:
            pending.rename(corpus / "formats")
        elif "-Mode 'discard'" in ps:
            shutil.rmtree(pending, ignore_errors=True)
    elif "-Do library-check" in ps:
        report = account_temp + r"\rbst\library-check-0123456789abcdef0123456789abcdef.json"
        win(report).parent.mkdir(parents=True, exist_ok=True)
        win(report).write_text("[]\n")
        print(f"LIBRARY CHECK {report}")
    elif "-Do format-hits" in ps and options == "idle":
        assert "$secs = 2700;" in host(args[8]).read_text()
        assert "$idle = 600" in host(args[8]).read_text()
        print("RB-KIT: no output for 600 s")
        sys.exit(142)
    elif "-Do format-import" in ps and options == "stop":
        print("format-import: STOP: unexpected rekordbox window")
        rc = 1
print(f"{marker} {rc}")
sys.exit(rc)
PY
chmod +x "$F/bin/fake-parallels"
target_kit() {
  local target="$1"; shift
  RBKIT_TARGET="$target" RBKIT_VM="${TARGET_VM-rb-win-template}" RBKIT_VM_USER="${TARGET_USER-rbtest}" \
    VMRUN_VM='Windows 11' VMRUN_UUID=foreign-uuid VMRUN_RBKIT_USER=foreign VMRUN_SHARE="$tmp/vm-share" \
    VMRUN_PRLCTL="$F/bin/fake-parallels" "$S" "$@" > "$tmp/o" 2>&1
}
target_reset() { reset; rm -f "$F/vm-calls" "$F/vm-uuids" "$F/vm-set" "$F/vm-restarted"; }

target_reset
target_kit typo preflight; wrong=$?
target_kit vm disk probe --drive C; disk=$?
if [ "$wrong:$disk" = 2:2 ] && [ ! -e "$F/calls" ] && [ ! -e "$F/scp" ] && [ ! -e "$F/vm-calls" ]; then ok
else no 'unknown targets and physical disk commands must stop before transport'; fi

target_reset
if (unset RBKIT_TARGET; "$S" run library-check) > "$tmp/pc-default" 2>&1 &&
   cp "$F/calls" "$tmp/default-calls" && cp "$F/scp" "$tmp/default-scp" &&
   target_reset && target_kit pc run library-check &&
   cmp -s "$tmp/pc-default" "$tmp/o" && cmp -s "$tmp/default-calls" "$F/calls" && cmp -s "$tmp/default-scp" "$F/scp"; then ok
else no 'default PC target must match explicit pc output and transport bytes'; fi

target_reset
if VMRUN_HTTP_BIND=vm-gateway.example VMRUN_HTTP_PORT=9172 target_kit vm preflight &&
   grep -qF -- "-Server 'vm-gateway.example:9172'" "$F/vm-calls" &&
   grep -qF 'OK    Parallels to the test account (rbtest)' "$tmp/o" &&
   target_reset && RBKIT_VM_SERVER= target_kit vm preflight && grep -qF 'RBKIT_VM_SERVER is empty' "$tmp/o" &&
   ! grep -qF 'net-probe.ps1' "$F/vm-calls" &&
   [ ! -e "$F/calls" ] && [ ! -e "$F/scp" ]; then ok
else no "VM preflight must use the fixed VM, interactive account and VM server: $(cat "$tmp/o")"; fi

target_reset
TARGET_USER= target_kit vm preflight; nouser=$?
TARGET_USER='a b' target_kit vm preflight; baduser=$?
TARGET_VM="x'y" target_kit vm preflight; badvm=$?
if [ "$nouser:$baduser:$badvm" = 2:2:2 ] && [ ! -e "$F/vm-calls" ] && [ ! -e "$F/vm-uuids" ]; then ok
else no "a VM target without a valid user or VM name must stop before transport: $nouser:$baduser:$badvm"; fi

target_reset
git -C "$r" config rbstemsplus.vmUser vmowner
if RBKIT_VM_SERVER= TARGET_VM= TARGET_USER= target_kit vm preflight &&
   grep -qF 'OK    Parallels to the test account (vmowner)' "$tmp/o" &&
   [ "$(sort -u "$F/vm-uuids")" = 8905c42f-722b-4145-a88b-c0539af7cec6 ] &&
   target_reset && RBKIT_VM_SERVER= TARGET_VM= TARGET_USER=other target_kit vm preflight &&
   grep -qF 'OK    Parallels to the test account (other)' "$tmp/o"; then ok
else no "the VM target must default to rb-win-hand-rekordbox and rbstemsplus.vmUser, RBKIT_VM_USER first: $(cat "$tmp/o")"; fi
git -C "$r" config --unset rbstemsplus.vmUser

target_reset
rm -f "$F/vm-probe"
if target_kit vm probe remove --rekordbox 7.2.19 && grep -qF "OK    rekordbox's onnxruntime.dll is Pioneer's" "$tmp/o" &&
   [ "$(tr '\n' ' ' < "$F/vm-probe")" = 'resolve remove ' ] && grep -qF -- '-Do quit' "$F/vm-calls" &&
   [ ! -e "$F/calls" ] && [ ! -e "$F/scp" ]; then ok
else no "VM probe remove must keep the VM target after resolving the probe: $(cat "$tmp/o")"; fi

target_reset
(unset RBKIT_TARGET; "$S" session display) > "$tmp/o" 2>&1; pcdisplay=$?
if [ "$pcdisplay" = 2 ] && grep -qF 'session display requires RBKIT_TARGET=vm' "$tmp/o" &&
   [ ! -e "$F/calls" ] && [ ! -e "$F/vm-calls" ] && [ ! -e "$F/vm-set" ]; then ok
else no "session display on the PC must stop before transport: $(cat "$tmp/o")"; fi

target_reset
if FAKE_VM_HIRES=on target_kit vm session display &&
   grep -qF 'OK    the VM display: 1920x1080 dpi 96 (Parallels high resolution switched off)' "$tmp/o" &&
   grep -qxF 'set rb-win-template --high-resolution off' "$F/vm-set" && [ ! -e "$F/vm-restarted" ] &&
   target_reset && target_kit vm session display && [ ! -e "$F/vm-set" ] &&
   grep -qF '(Parallels high resolution off)' "$tmp/o"; then ok
else no "session display must switch Parallels high resolution off and set 1920x1080 in the session: $(cat "$tmp/o")"; fi

target_reset
FAKE_VM_DPI=192 target_kit vm session display; rc=$?
if [ "$rc" = 1 ] && [ -e "$F/vm-restarted" ] &&
   grep -qF 'BAD   the VM display scale is not 100 % after a restart: 1920x1080 dpi 192' "$tmp/o" &&
   [ "$(grep -c 'vm-display.ps1' "$F/vm-calls")" = 2 ]; then ok
else no "session display must restart Windows once for the scale and then stop: $(cat "$tmp/o")"; fi

target_reset
guest_kit="$F/guest/Users/rbtest/AppData/Local/Temp/rbsp-kit"
mkdir "$tmp/vm-formats"
printf 'fixture\n' > "$tmp/vm-formats/fmt-a.wav"
(cd "$tmp/vm-formats" && shasum -a 256 fmt-a.wav > SHA256SUMS)
printf 'row\tfile\tscope\nfmt-a\tfmt-a.wav\tpublished\n' > "$tmp/vm-formats/rows.tsv"
if FAKE_VM_CASE=retry target_kit vm stage --formats "$tmp/vm-formats" --format-tier published &&
   [ -f "$F/vm-retried" ] &&
   cmp -s "$guest_kit/verify-kit.ps1" "$here/kit/verify-kit.ps1" &&
   cmp -s "$(find "$guest_kit" -name fmt-a.wav -print -quit)" "$tmp/vm-formats/fmt-a.wav" &&
   [ -z "$(find "$tmp/vm-share" -name 'rbkit-transfer.*' -print)" ] && [ ! -e "$F/scp" ]; then ok
else no "VM stages kit and format bytes through the share: $(cat "$tmp/o")"; fi

target_reset
if target_kit vm session up && grep -qF 'session on the console, unlocked' "$tmp/o" &&
   target_kit vm session down && grep -qFx 'CONSOLE down' "$tmp/o" &&
   grep -qF $'sys\t' "$F/vm-calls" && ! grep -qF 'user-signed' "$F/vm-calls" &&
   [ ! -e "$F/calls" ] && [ ! -e "$F/scp" ]; then ok
else no "VM session commands must use SYSTEM with rbtest's SID and preserve admin hash retry: $(cat "$tmp/o")"; fi

target_reset
if target_kit vm run library-check --rekordbox 7.2.19 &&
   grep -qF -- "-Do library-check -RbVersion '7.2.19'" "$F/vm-calls" &&
   grep -qxF '[]' "$RBKIT_OUT/library-check-0123456789abcdef0123456789abcdef.json" &&
   grep -qF '& tar.exe -cf ' "$F/vm-calls" && [ ! -e "$F/scp" ]; then ok
else no "VM must collect the library report through the share: $(cat "$tmp/o")"; fi

target_reset
FAKE_VM_CASE=stop target_kit vm run format-import --fresh-format-import --rekordbox 7.2.19; rc=$?
if [ "$rc" = 1 ] && grep -qF 'format-import: STOP:' "$tmp/o" && [ -s "$F/notified" ] &&
   ! grep -Eq -- '-Do (quit|format-verify)' "$F/vm-calls"; then ok
else no "VM format import STOP must leave the desktop at the stopped step: $(cat "$tmp/o")"; fi

target_reset
FAKE_VM_CASE=idle target_kit vm run hits --formats; rc=$?
if [ "$rc" = 1 ] && grep -qF 'RB-KIT: no output for 600 s' "$tmp/o" &&
   grep -qF 'no output for 600 s' "$F/notified"; then ok
else no "VM format batches must reach the long-running step and propagate the idle STOP: $(cat "$tmp/o")"; fi
target_reset
# --- end Windows target selection
# t5 rows: the owner's test drive found by model, serial and bus with the admin login; the wintest copied,
# its hash and the protected disks handed over; the listing check decides the row
mkdir -p "$tmp/wt"; echo exe > "$tmp/wt/winc-location-wintest.exe"
expect 2 "t5 rows, --idle too long" t5 rows --idle 30 --wintest "$tmp/wt/winc-location-wintest.exe"
expect 2 "t5 rows, not a wintest" t5 rows --wintest "$tmp/c"
reset; expect 0 "t5 rows" t5 rows --wintest "$tmp/wt/winc-location-wintest.exe" --idle 1; calls > "$tmp/c"
has "t5 rows prelude" "\$Phase = 'rows'; \$T5Serial = 'T5SN'; \$Protected = @('$rog_protection'); \$Wintest = '$(shasum -a 256 "$tmp/wt/winc-location-wintest.exe" | cut -c1-64)'; \$IdleMinutes = 1" "$tmp/c"
grep -q "winc-location-wintest.exe owner@pc.example:rbsp-kit-admin/winc-location-wintest.exe" "$F/scp" && ok || no "t5 rows: the wintest not copied to the admin login"
has "t5 rows" "OK    t5: J28 and the sleeping-disk row read" "$tmp/o"
expect 2 "t5, an unknown phase" t5 wipe
reset; expect 0 "t5 prep" t5 prep; calls > "$tmp/c"; has "t5 prep prelude" "\$Phase = 'prep'; \$T5Serial = 'T5SN'; \$Protected = @('$rog_protection'); \$TestUser = 'rbtest'" "$tmp/c"
has "t5 prep" "OK    t5 prep" "$tmp/o"
# A missing, malformed or mixed list refuses the entire T5 action before contacting the PC.
git -C "$r" config --unset-all rbstemsplus.pcProtectedDisks
reset; expect 1 "t5 prep, no protected disk set" t5 prep
[ ! -s "$F/calls" ] && ok || no "unset protection reached the PC"
reset; expect 1 "t5 rows, no protected disk set" t5 rows --wintest "$tmp/wt/winc-location-wintest.exe"
reset; expect 0 "t5 show, no protected disk set" t5 show
git -C "$r" config --add rbstemsplus.pcProtectedDisks "$rog_protection"
git -C "$r" config --add rbstemsplus.pcProtectedDisks "$usb_protection"
mkdir "$tmp/protection-bin"
cat > "$tmp/protection-bin/git" << 'EOF'
#!/bin/bash
if [[ "${@: -1}" = rbstemsplus.pcProtectedDisks ]]; then printf 'read\n' >> "$FAKE/protection-reads"; fi
exec "$REAL_GIT" "$@"
EOF
chmod +x "$tmp/protection-bin/git"
reset
expect 0 "t5 reads both configured values" env REAL_GIT="$(command -v git)" PATH="$tmp/protection-bin:$PATH" t5 finish
[ "$(cat "$F/protection-reads")" = read ] && ok || no "t5 did not read protection exactly once"
calls > "$tmp/c"
has "both protected values passed to the PC" "\$Protected = @('$rog_protection','$usb_protection')" "$tmp/c"
for invalid in 'invalid!' '' 'UFJPVEVDVEVE' 'VHxZ=' '/3xY'; do
  git -C "$r" config --add rbstemsplus.pcProtectedDisks "$invalid"
  reset; expect 1 "t5 refuses a mixed protection list with '$invalid'" t5 prep
  [ ! -s "$F/calls" ] && ok || no "invalid protection reached the PC"
  git -C "$r" config --unset-all rbstemsplus.pcProtectedDisks
  git -C "$r" config --add rbstemsplus.pcProtectedDisks "$rog_protection"
done
# The retired commands fail at parsing; cleanup leaves legacy recovery markers untouched.
for phase in offline online; do reset; expect 2 "retired t5 $phase" t5 "$phase"; done
reset; expect 2 "retired stage disk option" stage --disk-offline
[ ! -s "$F/calls" ] && ok || no "the retired stage disk option contacted the PC"
printf 'TESTPC VDVTTg== False 1\n' > "$tmp/out/disk-state.txt"
printf 'old recovery marker\n' > "$tmp/out/t5-offline.txt"
git -C "$r" config rbstemsplus.pcOfflineDisk '2:USB Disk'
reset; expect 0 "cleanup ignores old disk recovery state" cleanup; calls > "$tmp/c"
hasnt "cleanup ignores old disk recovery state" "Get-Disk" "$tmp/c"
hasnt "cleanup ignores old T5 recovery state" "\$Phase = 'online'" "$tmp/c"
[ -f "$tmp/out/disk-state.txt" ] && [ -f "$tmp/out/t5-offline.txt" ] && ok || no "cleanup consumed a legacy recovery marker"
git -C "$r" config --unset rbstemsplus.pcOfflineDisk
rm "$tmp/out/disk-state.txt" "$tmp/out/t5-offline.txt"
has "cleanup puts the choice back" "-Mode restore" "$tmp/c"
reset; printf 'chosen: C:\\x put back as it was\n' > "$F/chosenrestore-out"; expect 0 "cleanup, the choice put back" cleanup; has "cleanup, the choice put back" "OK    chosen: C:" "$tmp/o"
reset; printf 'T5: volume D:\\\nCHANGED OUTSIDE OUR FOLDER: 1 difference(s): > D:\\x|1|2\n' > "$F/t5app-out"; expect 1 "t5 list, outside changed" t5 list
has "t5 list, outside changed" "BAD   t5 list: something outside our folder" "$tmp/o"
reset; printf "T5: expected one 'Samsung Portable SSD T5' on USB, found 0\n" > "$F/t5-out"; echo 3 > "$F/t5-rc"; expect 3 "t5 rows, no T5" t5 rows --wintest "$tmp/wt/winc-location-wintest.exe"
has "t5 rows, no T5" "rows wait for it" "$tmp/o"
reset; printf 'T5: disk 2\nCLEAN: CHANGED: 1 difference(s): <= D:\\x|1|2\n' > "$F/t5-out"; expect 1 "t5 rows, the listing changed" t5 rows --wintest "$tmp/wt/winc-location-wintest.exe"
has "t5 rows, the listing changed" "BAD   t5: the T5's listing changed" "$tmp/o"
# The owner disk probe is read-only and bound to the test account's PC.
reset; "$S" disk probe --drive 'E:' > "$tmp/o" 2>&1; rc=$?
[ "$rc" = 2 ] && grep -q 'without a colon' "$tmp/o" && [ ! -s "$F/calls" ] && ok || no 'disk probe accepts only a drive letter before making calls'
reset; "$S" disk probe --drive e > "$tmp/o" 2>&1; rc=$?
[ "$rc" = 0 ] && grep -q '^OK *DISK E: type Removable bus unknown ready False fs none$' "$tmp/o" &&
  grep -q "^owner@pc.example.*COMPUTERNAME -ne 'TESTPC'.*Drive = 'E'.*Get-Disk" "$F/calls" &&
  ! calls | grep -q 'Set-Disk' && ok || no 'disk probe did not report the chosen drive with a read-only check on the verified PC'
reset; printf 'DISK E: type Fixed bus USB ready True fs NTFS\nID %s NAME USB Disk\n' "$usb_protection" > "$F/disk-probe-out"; "$S" disk probe --drive e > "$tmp/o" 2>&1
grep -q '^OK *DISK E: type Fixed bus USB ready True fs NTFS$' "$tmp/o" && grep -qF "key $usb_protection (to protect it" "$tmp/o" && ok || no "disk probe: the disk's protection value not reported"
git -C "$r" config --add rbstemsplus.pcProtectedDisks "$usb_protection"; reset; printf 'DISK E: type Fixed bus USB ready True fs NTFS\nID %s NAME USB Disk\n' "$usb_protection" > "$F/disk-probe-out"; "$S" disk probe --drive e > "$tmp/o" 2>&1
grep -q 'disk E: a protected disk' "$tmp/o" && ok || no "disk probe: a protected disk not named as one"; git -C "$r" config --unset-all rbstemsplus.pcProtectedDisks; git -C "$r" config rbstemsplus.pcProtectedDisks "$rog_protection"
reset; printf '%s\n' 'drive E:\ unavailable' > "$F/disk-probe-out"
"$S" disk probe --drive E > "$tmp/o" 2>&1; rc=$?
[ "$rc" = 1 ] && grep -q '^BAD .*disk probe' "$tmp/o" && ok || no 'disk probe accepted a call with no confirmed result'

reset; expect 0 "run T2" run T2 --from 01-mp3-cbr320 --max 3 --inject-at 3
[ "$(cut -f1 "$F/calls" | sort -u)" = "rbtest@pc.example" ] && ok || no "run T2: the login isn't rbtest@ the owner's host: $(cut -f1 "$F/calls" | sort -u)"
grep -q 'BatchMode=yes .*-- rbtest@pc.example' "$F/calls" && ok || no "ssh without BatchMode or --"
calls > "$tmp/c"
has "run T2" "-Do start" "$tmp/c"; has "run T2" "-Do run -Expect any -From '01-mp3-cbr320' -Max 3 -InjectAt 3 -Minutes 90; exit \$LASTEXITCODE" "$tmp/c"; has "run T2" "-Do quit" "$tmp/c"
[ ! -s "$F/notified" ] && ok || no "run T2: a notification without a STOP"
reset; expect 0 "run hits --keep-open" run hits --keep-open; calls > "$tmp/c"
has "run hits" "-Expect hits" "$tmp/c"; hasnt "run hits --keep-open" '-Do quit' "$tmp/c"
# a STOP, or an unexpected exit: rekordbox is left alone (no quit), the owner notified at once
reset; printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "run T2 with a STOP" run T2; has "run T2 STOP" "Sign in" "$F/notified"; calls > "$tmp/c"; hasnt "run T2 STOP" '-Do quit' "$tmp/c"
reset; echo 1 > "$F/run-rc"; expect 1 "run T2, exit 1 without a STOP line" run T2
has "run T2 exit 1" "exit 1 without a STOP line" "$F/notified"; calls > "$tmp/c"; hasnt "run T2 exit 1" '-Do quit' "$tmp/c"
reset; printf '10:00:00  run: STOP: error: boom\n' > "$F/run-out"; expect 1 "run T2, exit 0 with a STOP line" run T2
calls > "$tmp/c"; hasnt "run T2 exit 0 STOP" '-Do quit' "$tmp/c"
reset; printf '10:00:00  run: STOP: new window(s): powershell | RBSP watch test | 0x50 (watch test); shot x\n' > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "run T2 --inject-at's STOP" run T2 --inject-at 2; has "inject-at" "run T2: [watch test, nothing to do]" "$F/notified"
reset; echo 3 > "$F/run-rc"; expect 3 "run hits with an expectation not met" run hits; calls > "$tmp/c"; has "run hits exit 3" '-Do quit' "$tmp/c"
reset; printf 'x  quit: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/quit-out"; echo 1 > "$F/quit-rc"
expect 1 "run T2, the quit STOPs" run T2; has "quit STOP" "run T2 quit: new window(s)" "$F/notified"
reset; expect 1 "run T2, PC unreachable" env FAKE_RC=255 run T2; has "unreachable" "can't be reached" "$F/notified"
[ -z "${FAKE_RC:-}" ] && ok || no "fixture connection exit leaked to the next case"
reset; expect 0 "run watchtest" run watchtest; has "watchtest" "run watchtest: [watch test, nothing to do]" "$F/notified"
reset; expect 1 "run watchtest, the notifier fails" env RBKIT_NOTIFY="$F/notify-fails" run watchtest
reset; printf 'x  watchtest: FAIL: the window list failed after the test window was closed\n' > "$F/watchtest-out"; echo 1 > "$F/watchtest-rc"
expect 1 "run watchtest, no STOP line" run watchtest
reset; expect 0 "run prepare" run prepare --playlist 'RBSP Tests' --minutes 5; calls > "$tmp/c"
has "run prepare" "-Do prepare -Playlist 'RBSP Tests' -Minutes 5" "$tmp/c"
hasnt "run prepare" "-PauseTest" "$tmp/c"
reset; expect 2 "format preparation requires a unique import name" run prepare --formats --playlist 'RBSP Formats'
reset; expect 0 "prepare the exact imported format playlist" run prepare --formats --playlist 'RBSP Formats 0123456789abcdef-0123456789ab' --minutes 5
calls > "$tmp/c"
has "format preparation passes its restricted mode" "-Do prepare -Playlist 'RBSP Formats 0123456789abcdef-0123456789ab' -Minutes 5 -Formats" "$tmp/c"
git -C "$r" config rbstemsplus.pcTestSsh 'tester@other.example'
reset; expect 0 "pcTestSsh" run T2; [ "$(cut -f1 "$F/calls" | sort -u)" = "tester@other.example" ] && ok || no "pcTestSsh not used"
git -C "$r" config --unset rbstemsplus.pcTestSsh

# stage: the kit hashed, checked here, copied to the PC's %TEMP%\rbsp-kit, checked there
mkdir -p "$tmp/corpus/v1" "$tmp/corpus/win"; echo a > "$tmp/corpus/v1/01-x.mp3"; echo b > "$tmp/corpus/win/w16-x.flac"
(cd "$tmp/corpus/v1" && shasum -a 256 01-x.mp3 > SHA256SUMS)
reset; expect 0 "stage" stage --corpus "$tmp/corpus"
st="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
(cd "$st" && shasum -a 256 -c --quiet SHA256SUMS) && ok || no "stage: SHA256SUMS doesn't check"
for f in "$here"/kit/*.ps1 corpus/v1/01-x.mp3 corpus/v1/SHA256SUMS; do grep -qF "  ${f#$here/kit/}" "$st/SHA256SUMS" && ok || no "stage: ${f#$here/kit/} not in SHA256SUMS"; done
has "stage" "-- $tmp/out/stage-" "$F/scp"; has "stage" "rbtest@pc.example:C:/Users/rbtest/AppData/Local/Temp/rbsp-kit/" "$F/scp"
calls > "$tmp/c"; has "stage" "verify-kit.ps1" "$tmp/c"; has "stage" "-Do ensure" "$tmp/c"
reset; mkdir "$tmp/corpus/v1/.hidden"; touch "$tmp/corpus/v1/.hidden/track.mp3"
expect 2 "stage refuses a dot path" stage --corpus "$tmp/corpus"; [ ! -e "$F/scp" ] && ok || no "stage copied a dot path"
rm -r "$tmp/corpus/v1/.hidden"
reset; ln -s "01-x.mp3" "$tmp/corpus/v1/link.mp3"
expect 2 "stage refuses a symlink" stage --corpus "$tmp/corpus"; [ ! -e "$F/scp" ] && ok || no "stage copied a symlink"
rm "$tmp/corpus/v1/link.mp3"
reset; ln -s corpus "$tmp/linked-corpus"
expect 2 "stage refuses a linked corpus directory with a trailing slash" stage --corpus "$tmp/linked-corpus/"
rm "$tmp/linked-corpus"
reset; { printf 'C:\\Users\\rbtest\\AppData\\Local\\Temp\n'; (cd "$st" && shasum -a 256 corpus/v1/01-x.mp3); printf '%064d  dist/payload.json\n' 1; } > "$F/temp-out"
expect 0 "stage keeps the corpus and dist manifest entries" stage
kept="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
has "stage kept corpus" '  corpus/v1/01-x.mp3' "$kept/SHA256SUMS"; has "stage kept dist" '  dist/payload.json' "$kept/SHA256SUMS"
[ ! -e "$kept/corpus" ] && [ ! -e "$kept/dist" ] && ok || no "stage recopied kept files"
# stage --formats: subset by --format-tier or --format-rows, media files only, SHA-256 verified, old formats cleared
mkdir -p "$tmp/formats"
printf 'a\n' > "$tmp/formats/fmt-aac-lc-44100-2ch-lossy-m4a.m4a"
printf 'b\n' > "$tmp/formats/fmt-wav-pcm-44100-1ch-s16-wav.wav"
printf 'c\n' > "$tmp/formats/fmt-wav-pcm-88200-1ch-s16-wav.wav"
printf 'd\n' > "$tmp/formats/fmt-flac-12000-1ch-s16-flac.flac"
printf 'tools\n' > "$tmp/formats/TOOLS"
(cd "$tmp/formats" && shasum -a 256 fmt-* > SHA256SUMS)
printf 'row\tfile\tcodec\trate_hz\tchannels\tdepth\tscope\tsource\tstart_s\tscale\n' > "$tmp/formats/rows.tsv"
printf 'fmt-aac-lc-44100-2ch-lossy-m4a\tfmt-aac-lc-44100-2ch-lossy-m4a.m4a\taac\t44100\t2\tlossy\tpublished\ts\t1\t0.5\n' >> "$tmp/formats/rows.tsv"
printf 'fmt-wav-pcm-44100-1ch-s16-wav\tfmt-wav-pcm-44100-1ch-s16-wav.wav\tpcm_s16le\t44100\t1\ts16\tpublished\ts\t2\t0.6\n' >> "$tmp/formats/rows.tsv"
printf 'fmt-wav-pcm-88200-1ch-s16-wav\tfmt-wav-pcm-88200-1ch-s16-wav.wav\tpcm_s16le\t88200\t1\ts16\tpublished\ts\t3\t0.7\n' >> "$tmp/formats/rows.tsv"
printf 'fmt-flac-12000-1ch-s16-flac\tfmt-flac-12000-1ch-s16-flac.flac\tflac\t12000\t1\ts16\tdiscovery\ts\t4\t0.8\n' >> "$tmp/formats/rows.tsv"
reset
expect 2 "stage --formats without selection" stage --formats "$tmp/formats"
expect 2 "stage --format-tier without --formats" stage --format-tier published
expect 2 "stage --format-rows without --formats" stage --format-rows fmt-aac-lc-44100-2ch-lossy-m4a
expect 2 "stage --formats with both tier and rows" stage --formats "$tmp/formats" --format-tier published --format-rows fmt-aac-lc-44100-2ch-lossy-m4a
expect 2 "stage --formats bogus tier" stage --formats "$tmp/formats" --format-tier bogus
expect 2 "stage --formats unknown row" stage --formats "$tmp/formats" --format-rows fmt-does-not-exist
expect 2 "stage --formats inverted range" stage --formats "$tmp/formats" --format-rows fmt-wav-pcm-88200-1ch-s16-wav:fmt-wav-pcm-44100-1ch-s16-wav
expect 2 "stage --formats and --clear-formats" stage --formats "$tmp/formats" --format-tier published --clear-formats
[ ! -s "$F/calls" ] && ok || no "refused stage --formats contacted the PC"

reset
{ printf 'C:\\Users\\rbtest\\AppData\\Local\\Temp\n'; (cd "$st" && shasum -a 256 corpus/v1/01-x.mp3); } > "$F/temp-out"
expect 0 "stage --formats --format-tier published" stage --formats "$tmp/formats" --format-tier published
fst="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
[ -f "$fst/formats-upload/fmt-aac-lc-44100-2ch-lossy-m4a.m4a" ] && [ -f "$fst/formats-upload/fmt-wav-pcm-88200-1ch-s16-wav.wav" ] &&
  [ ! -e "$fst/formats-upload/fmt-flac-12000-1ch-s16-flac.flac" ] && [ ! -e "$fst/formats-upload/rows.tsv" ] &&
  [ ! -e "$fst/formats-upload/TOOLS" ] && [ -f "$fst/formats-upload/SHA256SUMS" ] &&
  ok || no "stage --format-tier published: wrong staged files"
has "stage --formats kept v1" '  corpus/v1/01-x.mp3' "$fst/SHA256SUMS"
staged_catalog="$(sed -nE 's/^[a-f0-9]{64}  (catalog-[a-f0-9]{64}\.txt)$/\1/p' "$fst/formats-upload/SHA256SUMS")"
has "format catalog includes unstaged rows" 'fmt-flac-12000-1ch-s16-flac' "$fst/formats-upload/$staged_catalog"
[ -n "$staged_catalog" ] && (cd "$fst/formats-upload" && shasum -c --quiet SHA256SUMS &&
  [ "$staged_catalog" = "catalog-$(shasum -a 256 < "$staged_catalog" | cut -d' ' -f1).txt" ]) &&
  ok || no "format catalog is sealed in the subset manifest under its checksum"
hasnt "stage --formats keeps manifest with media" 'fmt-' "$fst/SHA256SUMS"
has "stage uploads to a sibling dir" '/corpus/formats-incoming-' "$F/scp"
calls > "$tmp/c"; has "stage publishes verified manifest" "-Mode 'publish'" "$tmp/c"
hasnt "stage stops before measurement" "-Do format-run" "$tmp/c"
has "stage --formats prints the reuse note" "NOTE  run format-import looks for a matching RBSP Formats playlist before importing" "$tmp/o"

reset
expect 0 "stage --formats --format-rows range" stage --formats "$tmp/formats" --format-rows fmt-wav-pcm-44100-1ch-s16-wav:fmt-flac-12000-1ch-s16-flac
fst2="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
[ ! -e "$fst2/formats-upload/fmt-aac-lc-44100-2ch-lossy-m4a.m4a" ] && [ -f "$fst2/formats-upload/fmt-wav-pcm-44100-1ch-s16-wav.wav" ] &&
  [ -f "$fst2/formats-upload/fmt-flac-12000-1ch-s16-flac.flac" ] && ok || no "stage --format-rows range: wrong staged files"

reset
printf 'tampered\n' > "$tmp/formats/fmt-wav-pcm-44100-1ch-s16-wav.wav"
expect 1 "stage --formats rejects SHA-256 mismatch" stage --formats "$tmp/formats" --format-rows fmt-wav-pcm-44100-1ch-s16-wav
[ ! -s "$F/calls" ] && ok || no "stage --formats SHA-256 mismatch contacted the PC"

reset
{ printf 'C:\\Users\\rbtest\\AppData\\Local\\Temp\n'; (cd "$st" && shasum -a 256 corpus/v1/01-x.mp3); } > "$F/temp-out"
expect 0 "stage --clear-formats" stage --clear-formats
cst="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
has "stage --clear-formats kept v1" '  corpus/v1/01-x.mp3' "$cst/SHA256SUMS"
hasnt "stage --clear-formats dropped formats" 'corpus/formats/' "$cst/SHA256SUMS"
calls > "$tmp/c"; has "clear uses ownership-checked stage script" "-Mode 'clear'" "$tmp/c"
reset; echo 2 > "$F/format-path-rc"
expect 1 "clear refuses a destination junction" stage --clear-formats
[ ! -e "$F/scp" ] && ok || no "clear attempted a write through a junction"
reset; echo 2 > "$F/format-path-rc"
expect 1 "upload refuses a destination junction" stage --formats "$tmp/formats" --format-tier discovery
[ ! -e "$F/scp" ] && ok || no "upload attempted a write through a junction"
reset; echo 255 > "$F/format-upload-rc"
expect 1 "interrupted format upload" stage --formats "$tmp/formats" --format-tier discovery
calls > "$tmp/c"; has "interrupt discards pending only" "-Mode 'discard'" "$tmp/c"
hasnt "interrupt keeps active subset" "-Mode 'publish'" "$tmp/c"
has "interrupted upload preserves previous subset" "previous verified subset is kept" "$tmp/o"

if command -v pwsh > /dev/null; then
  stage_sim="$tmp/format-stage-model"; mkdir -p "$stage_sim/corpus/formats" "$stage_sim/temp"
  cp "$here"/kit/format-{lib,stage}.ps1 "$stage_sim/"
  fmt_stage_sim() {
    TEMP="$stage_sim/temp" SystemDrive="" pwsh -NoProfile -NonInteractive -File "$stage_sim/format-stage.ps1" \
      -Mode "$1" -Transfer 0123456789abcdef0123456789abcdef "${@:2}" > "$tmp/stage-model-out" 2>&1
  }
  cat > "$stage_sim/read-selection.ps1" << 'EOF'
param([string]$dir)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'format-lib.ps1')
$catalog = $null
$staged = Get-FormatFiles $dir -Verify -CatalogFile ([ref]$catalog)
"selection: $(($staged.Keys | Sort-Object) -join ',')"
"catalog: $((Get-FormatCatalog $catalog $staged) -join ',')"
EOF
  read_stage_selection() {
    TEMP="$stage_sim/temp" SystemDrive="" pwsh -NoProfile -NonInteractive -File "$stage_sim/read-selection.ps1" "$old"
  }
  old="$stage_sim/corpus/formats"
  pending="$stage_sim/corpus/formats-incoming-0123456789abcdef0123456789abcdef"
  printf 'old\n' > "$old/fmt-old.wav"
  (cd "$old" && shasum -a 256 fmt-old.wav > SHA256SUMS)
  if fmt_stage_sim clear; then
    no "clear erased an unowned format directory"
    mkdir -p "$old"; printf 'old\n' > "$old/fmt-old.wav"
  else has "unowned clear" "lacks the kit ownership marker" "$tmp/stage-model-out"; fi
  [ -f "$old/fmt-old.wav" ] && ok || no "unowned media was removed"
  printf 'RBSP format subset v1' > "$old/RBSP-FORMATS"
  printf 'fmt-old\n' > "$old/catalog.txt"
  (cd "$old" && shasum -a 256 catalog.txt fmt-old.wav > SHA256SUMS)
  if fmt_stage_sim prepare; then ok; else no "format model prepare"; cat "$tmp/stage-model-out"; fi
  printf 'new\n' > "$pending/fmt-new.wav"
  [ -f "$old/fmt-old.wav" ] && [ -f "$old/SHA256SUMS" ] && ok || no "pending upload replaced active subset"
  new_catalog='fmt-new\nfmt-new-sibling\n'
  catalog_name="catalog-$(printf '%b' "$new_catalog" | shasum -a 256 | cut -d' ' -f1).txt"
  printf '%b' "$new_catalog" > "$pending/$catalog_name"
  (cd "$pending" && shasum -a 256 "$catalog_name" fmt-new.wav > SHA256SUMS)
  sha="$(shasum -a 256 "$pending/SHA256SUMS" | cut -d' ' -f1)"
  printf 'partial\n' > "$pending/fmt-new.wav"
  if fmt_stage_sim publish -ManifestSha "$sha"; then no "format stage accepted partial upload"
  else has "partial upload" 'subset does not match its manifest' "$tmp/stage-model-out"; fi
  [ -f "$old/fmt-old.wav" ] && (cd "$old" && shasum -c --quiet SHA256SUMS) && ok || no "bad upload damaged the active subset"
  printf 'new\n' > "$pending/fmt-new.wav"
  if fmt_stage_sim publish -ManifestSha "$sha"; then ok; else no "format model publish"; cat "$tmp/stage-model-out"; fi
  [ ! -e "$pending" ] && [ "$(cat "$old/fmt-old.wav")" = old ] && [ "$(cat "$old/fmt-new.wav")" = new ] &&
    (cd "$old" && shasum -c --quiet SHA256SUMS) && ok || no "additive publish did not keep both fixtures"
  [ "$(read_stage_selection)" = $'selection: fmt-new\ncatalog: fmt-new,fmt-new-sibling' ] &&
    [ "$(cat "$old/catalog.txt")" = fmt-old ] && ok || no "additive publish did not seal the new catalog while retaining the old catalog and media"
  hasnt "retained fixture is outside the active selection" 'fmt-old.wav' "$old/SHA256SUMS"

  ln "$old/fmt-old.wav" "$stage_sim/retained.wav"
  if fmt_stage_sim prepare; then ok; else no "format model collision prepare"; cat "$tmp/stage-model-out"; fi
  printf 'different\n' > "$pending/fmt-old.wav"
  printf 'third\n' > "$pending/fmt-third.wav"
  (cd "$pending" && shasum -a 256 fmt-*.wav > SHA256SUMS)
  sha="$(shasum -a 256 "$pending/SHA256SUMS" | cut -d' ' -f1)"
  if fmt_stage_sim publish -ManifestSha "$sha"; then no "format stage replaced a retained fixture"
  else has "retained collision" 'SHA-256 mismatch for existing fixture fmt-old.wav' "$tmp/stage-model-out"; fi
  [ "$old/fmt-old.wav" -ef "$stage_sim/retained.wav" ] && [ "$(cat "$old/fmt-old.wav")" = old ] &&
    [ ! -e "$old/fmt-third.wav" ] && (cd "$old" && shasum -c --quiet SHA256SUMS) && ok || no "collision changed the active stage"
  printf 'old\n' > "$pending/fmt-old.wav"
  (cd "$pending" && shasum -a 256 fmt-*.wav > SHA256SUMS)
  sha="$(shasum -a 256 "$pending/SHA256SUMS" | cut -d' ' -f1)"
  if fmt_stage_sim publish -ManifestSha "$sha"; then ok; else no "format model reuse"; cat "$tmp/stage-model-out"; fi
  [ "$old/fmt-old.wav" -ef "$stage_sim/retained.wav" ] && [ -f "$old/fmt-new.wav" ] && [ -f "$old/fmt-third.wav" ] &&
    (cd "$old" && shasum -c --quiet SHA256SUMS) && ok || no "same-hash fixture was not kept in place"
  if fmt_stage_sim clear; then ok; else no "format model clear selection"; cat "$tmp/stage-model-out"; fi
  [ ! -e "$old/SHA256SUMS" ] && [ -f "$old/fmt-old.wav" ] && [ -f "$old/fmt-new.wav" ] &&
    [ -f "$old/fmt-third.wav" ] && ok || no "clear removed previously staged media"

  mv "$stage_sim/corpus" "$stage_sim/keep"; ln -s "$stage_sim/keep" "$stage_sim/corpus"
  if fmt_stage_sim clear; then no "clear followed a reparse point"
  else has "junction simulation" 'reparse point in destination' "$tmp/stage-model-out"; fi
  [ -f "$stage_sim/keep/formats/fmt-new.wav" ] && ok || no "clear changed a symlink target"
  if TEMP="$stage_sim/temp" SystemDrive="X:" pwsh -NoProfile -NonInteractive -File "$stage_sim/format-stage.ps1" \
    -Mode clear -Transfer 0123456789abcdef0123456789abcdef > "$tmp/stage-model-out" 2>&1; then no "clear accepted another volume"
  else has "volume simulation" 'destination must be on the system volume' "$tmp/stage-model-out"; fi
fi

if command -v pwsh > /dev/null; then
  vk="$tmp/verify-kit"; mkdir -p "$vk/corpus/win"; cp "$here/kit/verify-kit.ps1" "$vk/"
  verify() {
    local want="$1" name="$2" rc; shift 2
    pwsh -NoProfile -NonInteractive -File "$vk/verify-kit.ps1" "$@" > "$tmp/verify-out" 2>&1; rc=$?
    [ "$rc" = "$want" ] && ok || { no "verify kit: $name (exit $rc, expected $want)"; sed 's/^/    /' "$tmp/verify-out"; }
  }
  : > "$vk/SHA256SUMS"; verify 1 'empty manifest'
  (cd "$vk" && shasum -a 256 verify-kit.ps1 > SHA256SUMS)
  verify 0 'complete manifest'
  echo 'extra' > "$vk/corpus/unlisted.txt"; verify 1 'unlisted retained file'; rm "$vk/corpus/unlisted.txt"
  echo w16 > "$vk/corpus/win/w16-flac-longpath.flac"
  (cd "$vk" && shasum -a 256 corpus/win/w16-flac-longpath.flac >> SHA256SUMS)
  verify 0 'generate and list the long-path files' -RefreshLongPath
  has "long-path copy listed" '  corpus/long/' "$vk/SHA256SUMS"
  has "long-path pointer listed" '  corpus/long-path.txt' "$vk/SHA256SUMS"
  verify 0 'verify the generated long-path files again'
  if [ -f "$vk/corpus/long-path.txt" ]; then
    long="$(cat "$vk/corpus/long-path.txt")"; echo changed > "$long"; verify 1 'changed kept long-path file'
  else no "verify kit: no long-path pointer"; fi
fi
# Stage checks the pinned T5 itself, through the verified owner's login, after ensure.
reset; echo 1 > "$F/verify-rc"; expect 1 "stage, the kit doesn't verify" stage; calls > "$tmp/c"
hasnt "stage unverified" "-Do ensure" "$tmp/c"
hasnt "stage unverified" "\$Phase = 'check'" "$tmp/c"
reset; echo 1 > "$F/ensure-rc"; expect 1 "stage, ensure fails" stage; calls > "$tmp/c"
hasnt "stage, ensure fails" "\$Phase = 'check'" "$tmp/c"
reset; echo 'ensure: STOP: new window' > "$F/ensure-out"; expect 1 "stage, ensure STOPs" stage; calls > "$tmp/c"
hasnt "stage, ensure STOPs" "\$Phase = 'check'" "$tmp/c"
reset; expect 0 "stage verifies the T5" stage
grep -qF "\$obsolete = Join-Path \$k 't5-rows.ps1'; if (Test-Path -LiteralPath \$obsolete -PathType Leaf) { Remove-Item -LiteralPath \$obsolete -Force -ErrorAction Stop }; & (Join-Path \$k 'verify-kit.ps1')" "$F/calls" &&
  ok || no "stage did not remove the obsolete script before verifying the manifest"
grep -q "^owner@pc.example.*COMPUTERNAME -ne 'TESTPC'.*Phase = 'check'; .*T5Serial = 'T5SN'" "$F/calls" &&
  ok || no "stage did not check the pinned T5 on the verified PC"
reset; printf 'T5: expected one matching drive, found 0\n' > "$F/t5check-out"; echo 3 > "$F/t5check-rc"
expect 1 "stage refuses a missing T5 when a serial is pinned" stage
git -C "$r" config --unset-all rbstemsplus.pcProtectedDisks
reset; expect 0 "stage with protection unset" stage; calls > "$tmp/c"
hasnt "stage with protection unset" "\$Phase = 'check'" "$tmp/c"
git -C "$r" config --add rbstemsplus.pcProtectedDisks "$rog_protection"
git -C "$r" config --unset rbstemsplus.pcT5Serial

reset; expect 0 "collect" collect --out "$tmp/col"
[ -f "$tmp/col/midi-auto.log" ] && ok || no "collect: the bundle not unzipped"
has "collect" "-- rbtest@pc.example:C:/Users/rbtest/AppData/Local/Temp/rbst/collect-20261118-100000.zip" "$F/scp"
calls > "$tmp/c"; has "collect" "-Do bundled -Keys 'collect-20261118-100000.zip'" "$tmp/c"
hasnt "collect" "copies of the screenshots not deleted" "$tmp/o"
# results inside the checkout only where git ignores them
reset; expect 2 "collect --out in the checkout" collect --out "$r/tests/out"; expect 2 "RBKIT_OUT in the checkout" env RBKIT_OUT="$r/res" preflight
printf 'ign/\n' > "$r/.gitignore"; reset; expect 0 "RBKIT_OUT in the checkout, git-ignored" env RBKIT_OUT="$r/ign" preflight
printf 'results/x\nonly-sentinel/x\n' >> "$r/.gitignore"
reset; expect 2 "only the results sentinel is ignored" env RBKIT_OUT="$r/results" preflight
[ ! -e "$F/calls" ] && [ -z "$(ls -A "$r/results")" ] && ok || no "only the results sentinel is ignored: the driver went ahead"
reset; expect 2 "only the collect sentinel is ignored" collect --out "$r/only-sentinel"
[ ! -e "$F/calls" ] && ok || no "only the collect sentinel is ignored: the driver went ahead"
reset; expect 0 "collect in a git-ignored folder" collect --out "$r/ign/col"
# stage --dist: only payload.json, and the test CA as DER (test-ca.cer)
mkdir -p "$tmp/dist/extra"; echo '{}' > "$tmp/dist/payload.json"; echo x > "$tmp/dist/extra/big.bin"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/dist/k.pem" -out "$tmp/dist/test-ca.pem" -subj /CN=rbkit-test -days 1 2> /dev/null
reset; expect 0 "stage --dist" stage --dist "$tmp/dist"; st="$(sed -n 's/^stage: [0-9][0-9]* files in //p' "$tmp/o")"
[ "$(cd "$st/dist" && ls | tr '\n' ' ')" = "payload.json test-ca.cer " ] && ok || no "stage --dist: $(cd "$st/dist" && ls | tr '\n' ' ')"
openssl x509 -inform der -in "$st/dist/test-ca.cer" -noout 2> /dev/null && ok || no "stage --dist: test-ca.cer is not a DER certificate"
reset; expect 0 "preflight" preflight; has "preflight" "SKIP  the TEST server" "$tmp/o"
reset; expect 1 "preflight without a notifier" env RBKIT_NOTIFY="$tmp/none" preflight
reset; echo 1 > "$F/verify-rc"; expect 1 "preflight, the kit doesn't verify" preflight; calls > "$tmp/c"; hasnt "preflight unverified" "-Do ensure" "$tmp/c"
# cleanup: the account's SID and PC from its own login; the owner's login removes only that SID, on that PC
reset; expect 0 "cleanup" cleanup --label t1
grep -q "^owner@pc.example	.*COMPUTERNAME -ne 'TESTPC'.*-eq 'S-1-5-21-11-22-33-1001'.*Remove-LocalGroupMember" "$F/calls" && ok || no "cleanup: admin rights not removed by the test account's SID on its PC"
calls > "$tmp/c"; has "cleanup" "-Mode Inventory -Name 't1'" "$tmp/c"; has "cleanup" "-Do logoff" "$tmp/c"; has "cleanup" "quser" "$tmp/c"
reset; echo 'admin 1' > "$F/admin-out"; expect 1 "cleanup, still an administrator" cleanup
reset; echo "not the test account's PC: OTHER" > "$F/admin-out"; echo 2 > "$F/admin-rc"; expect 1 "cleanup, the owner's login on another PC" cleanup
reset; echo 'admin 0' > "$F/admin-out"; echo 1 > "$F/admin-rc"; expect 1 "cleanup, the admin step failed" cleanup
reset; echo 'sid S-1-5-32-544' > "$F/ident-out"; expect 1 "cleanup, a SID not of an account" cleanup; calls > "$tmp/c"; hasnt "bad SID" "Remove-LocalGroupMember" "$tmp/c"
# a STOP at quit, or Stems Cache still installed: cleanup stops, the account keeps its rights and session
reset; printf 'x  quit: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/quit-out"; echo 1 > "$F/quit-rc"
expect 1 "cleanup, the quit STOPs" cleanup; calls > "$tmp/c"; hasnt "cleanup quit STOP" "uninstallcache" "$tmp/c"; hasnt "cleanup quit STOP" "-Do logoff" "$tmp/c"
has "cleanup quit STOP" "cleanup: new window(s)" "$F/notified"
reset; echo 'other 1234abcd' > "$F/ort-out"; expect 1 "cleanup, Stems Cache not uninstalled" cleanup; calls > "$tmp/c"
# with the admin login, the worker's uninstall runs there (no UAC prompt), never the app's button
has "cleanup ort" "\$Verb = 'uninstall'; \$Sid = 'S-1-5-21-11-22-33-1001'" "$tmp/c"; hasnt "cleanup ort" "-Do uninstallcache" "$tmp/c"
grep -q "^owner@pc.example	.*Verb = 'uninstall'" "$F/calls" && ok || no "cleanup: the worker's uninstall not on the owner's login"
hasnt "cleanup ort" "Remove-LocalGroupMember" "$tmp/c"; hasnt "cleanup ort" "-Do logoff" "$tmp/c"
has "cleanup ort" "keeps its admin rights and session" "$F/notified"
# an uninstall that fails: nothing more, and no second read of the DLL (RKR1)
reset; echo 'other 1234abcd' > "$F/ort-out"; printf 'worker: rekordbox is open\nWORKER exit 5\n' > "$F/worker-out"
expect 1 "cleanup, the worker's uninstall fails" cleanup; calls > "$tmp/c"; has "uninstall fails" "cleanup: uninstallcache exit 1" "$F/notified"
[ "$(grep -c Get-OrtState "$tmp/c")" = 1 ] && ok || no "cleanup, the uninstall fails: the DLL read again"; hasnt "uninstall fails" "rbstate.ps1" "$tmp/c"; hasnt "uninstall fails" "-Do logoff" "$tmp/c"
# without the admin login: the app's own button, as before
git -C "$r" config rbstemsplus.pcTestSsh 'rbtest@pc.example'; git -C "$r" config --unset rbstemsplus.pcSsh
reset; echo 'other 1234abcd' > "$F/ort-out"; printf 'x  uninstallcache: STOP: rekordbox'"'"'s onnxruntime.dll is other 1234abcd; shot x\n' > "$F/uninstall-out"; echo 1 > "$F/uninstall-rc"
expect 1 "cleanup without pcSsh, the uninstall STOPs" cleanup; calls > "$tmp/c"; has "uninstall STOP" "cleanup: rekordbox's onnxruntime.dll is other" "$F/notified"; hasnt "uninstall STOP" "\$Verb = " "$tmp/c"
[ "$(grep -c Get-OrtState "$tmp/c")" = 1 ] && ok || no "cleanup, the uninstall STOPs: the DLL read again"; hasnt "uninstall STOP" "rbstate.ps1" "$tmp/c"; hasnt "uninstall STOP" "-Do logoff" "$tmp/c"
git -C "$r" config rbstemsplus.pcSsh 'owner@pc.example'; git -C "$r" config --unset rbstemsplus.pcTestSsh
# a step's ssh status counts, not only its text (RKR9)
reset; echo 255 > "$F/ort-rc"; expect 1 "cleanup, the DLL not read" cleanup; calls > "$tmp/c"; hasnt "ort not read" "rbstate.ps1" "$tmp/c"
reset; echo 255 > "$F/snap-rc"; expect 1 "cleanup, the snapshot's ssh fails" cleanup; has "snapshot exit" "snapshot or inventory failed (exit 255)" "$tmp/o"
# a STOP line is notified before a lost connection or an overrun (RKR4)
reset; printf 'x  quit: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/quit-out"; echo 255 > "$F/quit-rc"
expect 1 "cleanup, the quit STOPs, then ssh drops" cleanup; has "STOP before lost" "cleanup: new window(s)" "$F/notified"; hasnt "STOP before lost" "can't be reached" "$F/notified"
reset; printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/run-out"; echo 142 > "$F/run-rc"
expect 1 "run T2, a STOP, then the cap" run T2; has "run STOP before 142" "run T2: new window(s)" "$F/notified"; hasnt "run STOP before 142" "no end in the time" "$F/notified"
# Shorten the idle interval in the test copy for these cases only. The run's overall cap is unchanged.
sed -i '' 's/local idle=600/local idle=1/' "$S"
reset; touch "$F/stall"; expect 1 "run T2, no output" run T2; rm "$F/stall"
has "idle notification" "no output for 1 s" "$F/notified"
[ -f "$F/notified" ] && [ "$(wc -l < "$F/notified" | tr -d ' ')" = 1 ] && ok || no "the idle notification was not sent exactly once"
calls > "$tmp/c"; hasnt "idle did not quit rekordbox" "-Do quit" "$tmp/c"
reset; touch "$F/chatty"; expect 0 "run T2, output without newlines resets the watchdog" run T2; rm "$F/chatty"
has "output preserved" "beat beat beat beat beat beat done" "$tmp/o"
# Put the real interval back: a later case that runs real pwsh behind auto (the format quit model)
# is silent for longer than 1 s while pwsh starts under load, and the watchdog would kill it.
sed -i '' 's/local idle=1 /local idle=600 /' "$S"
grep -q 'local idle=600 ' "$S" && ! grep -q 'local idle=1 ' "$S" && ok || no "the idle interval was not restored in the test copy"
# without the owner's login: the test account checks itself
git -C "$r" config rbstemsplus.pcTestSsh 'rbtest@pc.example'; git -C "$r" config --unset rbstemsplus.pcSsh
reset; expect 1 "cleanup without pcSsh, still an administrator" cleanup
reset; printf 'sid S-1-5-21-11-22-33-1001\npc TESTPC\nadmin 0\n' > "$F/ident-out"; expect 0 "cleanup without pcSsh" cleanup
git -C "$r" config rbstemsplus.pcSsh 'owner@pc.example'; git -C "$r" config --unset rbstemsplus.pcTestSsh
# the sign-out: asked, then its session gone
reset; echo 'signed in' > "$F/quser-out"; expect 1 "cleanup, still signed in" cleanup
[ "$(grep -c quser "$F/calls")" = 12 ] && ok || no "cleanup: not 12 sign-out checks"
# the poll fails closed: "signed out" over a failed ssh is not signed out (RKR6)
reset; echo 255 > "$F/quser-rc"; expect 1 "cleanup, 'signed out' with ssh exit 255" cleanup; has "quser exit 255" "exit 255: signed out" "$tmp/o"
# the poll's own PowerShell (as cleanup sends it), quser.exe stubbed by a function
if command -v pwsh > /dev/null; then
  grep -m1 'quser' "$F/calls" | cut -f3- > "$tmp/quser.ps1"
  qcase() {   # NAME EXIT LINE STUB
    { printf '%s\n' "$4"; cat "$tmp/quser.ps1"; } > "$tmp/q.ps1"
    local out rc; out="$(TEMP="$tmp" pwsh -NoProfile -NonInteractive -File "$tmp/q.ps1" 2>&1)"; rc=$?
    [ "$rc" = "$2" ] && [[ "$out" == $3 ]] && ok || no "the quser poll, $1: exit $rc: $out"
  }
  hdr='" USERNAME              SESSIONNAME        ID  STATE   IDLE TIME  LOGON TIME"'
  qcase 'quser missing' 2 'quser missing' ''
  qcase 'an indented row' 0 'signed in' "function quser.exe { $hdr; '  rbtest                console             1  Active      none   11/18/2026 10:00'; \$global:LASTEXITCODE = 0 }"
  qcase 'the current session' 0 'signed in' "function quser.exe { $hdr; '>rbtest                console             1  Active      none   11/18/2026 10:00'; \$global:LASTEXITCODE = 0 }"
  qcase 'another user only' 0 'signed out' "function quser.exe { $hdr; ' rbtest2               console             1  Active      none   11/18/2026 10:00'; \$global:LASTEXITCODE = 0 }"
  qcase 'no user (exit 1)' 0 'signed out' "function quser.exe { 'No User exists for *'; \$global:LASTEXITCODE = 1 }"
  qcase 'quser fails' 2 'quser failed: exit 5:*' "function quser.exe { 'Error 0x000006BA enumerating sessionnames'; \$global:LASTEXITCODE = 5 }"
  qcase 'no output' 2 'quser failed: exit 0:*' "function quser.exe { \$global:LASTEXITCODE = 0 }"
fi
reset; printf 'x  logoff: no answer from the session task in 20 s\n' > "$F/logoff-out"; echo 1 > "$F/logoff-rc"; expect 1 "cleanup, the sign-out not confirmed" cleanup

# --- unattended: the desktop session, the worker through the admin login, the plans
expect 2 "session without an action" session; expect 2 "session bogus" session bogus; expect 2 "cache bogus" cache bogus
expect 2 "auto bogus" auto bogus; expect 2 "auto record without --commit" auto record; expect 2 "--commit on session2" auto session2 --commit "$(printf '%040d' 0)"
expect 2 "auto record, --commit not a commit here" auto record --commit "$(printf '%040d' 0)" --dist "$tmp"
expect 2 "--lock on run" run T2 --lock
# --- kitps models (the encoded command and prelude are from the real driver)
mkdir "$tmp/deadline-bin"
cat > "$tmp/deadline-bin/perl" << 'EOF'
#!/bin/bash
if [ "$1" = -e ] && [[ "$2" == 'alarm shift; '* ]]; then printf '%s\n' "$3" > "$FAKE/mac-deadline"; fi
exec "$REAL_PERL" "$@"
EOF
chmod +x "$tmp/deadline-bin/perl"
reset
unset RBKIT_TRACE
# Give this invocation its own log directory so earlier session up cases can't supply its log.
expect 0 "session up with default tracing" env RBKIT_OUT="$tmp/session-up-trace" \
  REAL_PERL="$(command -v perl)" PATH="$tmp/deadline-bin:$PATH" FAKE_CAPTURE_RUNNER=1 session up
session_log="$(find "$tmp/session-up-trace" -name '*-session-up.log' -print -quit)"
has "session up logs trace stderr by default" '2026-11-19T10:00:00.0000000Z console: before Add-Type' "$session_log"
hasnt "session up keeps trace off the terminal" 'console: before Add-Type' "$tmp/o"
[ "$(cat "$F/mac-deadline")" = 120 ] && grep -qF 'Create($r)) 120 ' "$F/boot.ps1" &&
  [ "$(wc -c < "$F/boot-command")" -lt 8191 ] && ok || no 'kitps: the runner command does not carry the Mac deadline within the Windows command length limit'
cp "$F/prelude.ps1" "$tmp/console-prelude.ps1"
cp "$F/boot-command" "$tmp/console-command"
cp "$F/boot.ps1" "$tmp/console-stub.ps1"
reset; expect 0 "session up with trace disabled" env RBKIT_TRACE=0 FAKE_CAPTURE_RUNNER=1 session up
hasnt "session up honors RBKIT_TRACE=0" '[Console]::Error.WriteLine' "$F/runner.ps1"
reset; expect 0 "session cleanup-runners" env RBKIT_TRACE=1 FAKE_CAPTURE_RUNNER=1 session cleanup-runners
grep -q "^owner@pc.example	.*COMPUTERNAME -ne 'TESTPC'.*# cleanup-runners.ps1:" "$F/calls" && ok || no 'cleanup-runners did not use the admin login and PC check'
reset; echo 1 > "$F/cleanup-runners-rc"; expect 1 "auto prepare, runner cleanup fails" auto prepare
calls > "$tmp/c"; hasnt "auto stops before session up if runner cleanup fails" "\$Mode = 'up'" "$tmp/c"

if command -v pwsh > /dev/null; then
  # Substitute the Windows job APIs in the runner and the scheduled action.
  # The real reflection binding, managed callback, and script dispatch run on the Mac.
  cat > "$tmp/job-model.ps1" << 'EOF'
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.IO;
public static class JobModel {
    static bool assigned;
    public static IntPtr CreateJobObjectW(IntPtr attr, IntPtr name) { return new IntPtr(1); }
    public static bool AssignProcessToJobObject(IntPtr job, IntPtr process) { return assigned = job.ToInt32() == 1; }
    public static bool TerminateJobObject(IntPtr job, uint code) {
        if (!assigned || job.ToInt32() != 1) return false;
        var child = Environment.GetEnvironmentVariable("MODEL_CHILD");
        if (File.Exists(child)) File.WriteAllText(child, "terminated with job");
        Environment.Exit((int)code);
        return true;
    }
}
'@
function Use-ModelJob([string]$script) {
    $script.Replace('[AppDomain]::CurrentDomain.DefineDynamicAssembly', '[Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly').Replace('$api = $type.CreateType()', '$api = [JobModel]')
}
EOF
  cat > "$tmp/deadline.ps1" << 'EOF'
. "$PSScriptRoot/job-model.ps1"
$runner = Use-ModelJob (Get-Content -Raw -LiteralPath $args[0])
& ([scriptblock]::Create($runner)) 1 '$model = 1' (Get-Content -Raw -LiteralPath $args[1])
EOF
  printf '[IO.File]::WriteAllText($env:MODEL_CHILD, "running"); Start-Sleep -Seconds 3; "runner survived"; exit 9\n' > "$tmp/blocked-script"
  run="$r/tests/rekordbox/win/kitps-run.ps1"
  MODEL_CHILD="$tmp/child" pwsh -NoProfile -NonInteractive -File "$tmp/deadline.ps1" "$run" "$tmp/blocked-script" < /dev/null > "$tmp/deadline-out" 2> "$tmp/deadline-err"
  [ "$?" = 142 ] && [ "$(cat "$tmp/child" 2>/dev/null)" = 'terminated with job' ] && [ ! -s "$tmp/deadline-out" ] && [ ! -s "$tmp/deadline-err" ] &&
    ok || no 'remote deadline did not stop the blocked script and its child'
  printf 'Start-Sleep -Seconds 3; exit 9\n' > "$tmp/waiting-script"
  MODEL_CHILD="$tmp/no-child" pwsh -NoProfile -NonInteractive -File "$tmp/deadline.ps1" "$run" "$tmp/waiting-script" < /dev/null > "$tmp/deadline-out" 2> "$tmp/deadline-err"
  [ "$?" = 142 ] && [ ! -s "$tmp/deadline-out" ] && [ ! -s "$tmp/deadline-err" ] && ok || no 'remote deadline did not stop a script that only waits'
  printf '"normal output $model"; $global:LASTEXITCODE = 3\n' > "$tmp/normal-script"
  MODEL_CHILD="$tmp/no-child" pwsh -NoProfile -NonInteractive -File "$tmp/deadline.ps1" "$run" "$tmp/normal-script" < /dev/null > "$tmp/deadline-out" 2> "$tmp/deadline-err"
  [ "$?" = 3 ] && [ "$(cat "$tmp/deadline-out")" = 'normal output 1' ] && [ ! -s "$tmp/deadline-err" ] && ok || no 'the runner changed ordinary output or exit status, or lost the prelude'
  # The real runner command (captured from session up), run in pwsh against a model admin folder: a
  # missing or changed file is refused before anything runs; the right files pass the check.
  kitdir() { rm -rf "$tmp/home"; mkdir -p "$tmp/home/rbsp-kit-admin"; }
  stub() { USERPROFILE="$tmp/home" RAN="$tmp/ran" pwsh -NoProfile -NonInteractive -Command "& ([scriptblock]::Create((Get-Content -Raw -LiteralPath '$tmp/console-stub.ps1')))" < /dev/null > "$tmp/stub-out" 2>&1; }
  kitdir; stub; [ "$?" = 9 ] && grep -qx 'RBKIT: missing kitps-run.ps1' "$tmp/stub-out" && ok || no "the runner command ran without its runner ($(head -1 "$tmp/stub-out"))"
  kitdir; cp "$run" "$tmp/home/rbsp-kit-admin/"; stub
  [ "$?" = 9 ] && grep -qx 'RBKIT: missing console.ps1' "$tmp/stub-out" && ok || no "the runner command ran without its script ($(head -1 "$tmp/stub-out"))"
  kitdir; cp "$run" "$tmp/home/rbsp-kit-admin/"; { printf '[IO.File]::WriteAllText($env:RAN, "ran")\n'; cat "$r/tests/rekordbox/win/kit/console.ps1"; } > "$tmp/home/rbsp-kit-admin/console.ps1"; rm -f "$tmp/ran"; stub
  [ "$?" = 9 ] && grep -qx 'RBKIT: hash mismatch console.ps1' "$tmp/stub-out" && [ ! -e "$tmp/ran" ] && ok || no "the runner command ran a changed script ($(head -1 "$tmp/stub-out"))"
  kitdir; printf '\n' >> "$tmp/home/rbsp-kit-admin/kitps-run.ps1"; cp "$r/tests/rekordbox/win/kit/console.ps1" "$tmp/home/rbsp-kit-admin/"; stub
  [ "$?" = 9 ] && grep -qx 'RBKIT: hash mismatch kitps-run.ps1' "$tmp/stub-out" && ok || no "the runner command ran a changed runner ($(head -1 "$tmp/stub-out"))"
  kitdir; cp "$run" "$r/tests/rekordbox/win/kit/console.ps1" "$tmp/home/rbsp-kit-admin/"; stub
  [ "$?" != 9 ] && ! grep -q '^RBKIT: ' "$tmp/stub-out" && ok || no "the runner command refused the right files ($(head -1 "$tmp/stub-out"))"

  # Use the real runner with the captured prelude and model the Windows console APIs.
  cat > "$tmp/console-apis.ps1" << 'EOF'
$env:COMPUTERNAME = 'TESTPC'
if ($env:MODEL_SCENARIO -eq 'hung-start') {
    [Console]::SetError([IO.StreamWriter]::new([Console]::OpenStandardError()))
}
Add-Type @'
using System;
using System.IO;
using System.Threading;
public static class RbspWts {
    public static int Console = 1;
    public static bool Started;
    static int afterDisconnect;
    static bool askedToDisconnect;
    static string Scenario = Environment.GetEnvironmentVariable("MODEL_SCENARIO");

    public static int WTSGetActiveConsoleSessionId() { return Console; }
    public static string[] List() {
        if (Started && Scenario == "hung-snapshot") Thread.Sleep(1600);
        if (askedToDisconnect) {
            afterDisconnect++;
            File.AppendAllText(Environment.GetEnvironmentVariable("MODEL_CALLS"), "poll\n");
            if (Scenario == "down-poll-budget") Thread.Sleep(1100);
        }
        int state = Scenario == "down-slow" && afterDisconnect >= 3 ? 4 : 0;
        return new string[] { "2\trbtest\t" + state };
    }
    public static bool WTSDisconnectSession(IntPtr server, int id, bool wait) {
        File.AppendAllText(Environment.GetEnvironmentVariable("MODEL_CALLS"), "disconnect " + id + " " + wait + "\n");
        askedToDisconnect = true;
        return true;
    }
}
'@
function Wait-ModelCall([string]$name) {
    if ($env:MODEL_SCENARIO -eq "hung-$name") { Start-Sleep -Milliseconds 1600 }
}
function Add-Type {
    Wait-ModelCall 'compile'
    [Console]::Error.WriteLine('model: Add-Type')
}
function New-ScheduledTaskAction($Execute, $Argument) {
    if ($env:MODEL_SCENARIO -eq 'up' -and $Argument -match ' -EncodedCommand (\S+)$') {
        [IO.File]::WriteAllText($env:MODEL_ACTION, [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String($Matches[1])))
    }
    'action'
}
function New-ScheduledTaskPrincipal { 'principal' }
function Register-ScheduledTask { Wait-ModelCall 'register' }
function Start-ScheduledTask {
    Wait-ModelCall 'start'
    [Console]::Error.WriteLine('model: tscon')
    [RbspWts]::Started = $true
    if ($env:MODEL_SCENARIO -ne 'hung-result') { [RbspWts]::Console = 2 }
}
function Get-ScheduledTaskInfo {
    Wait-ModelCall 'result'
    @{ LastTaskResult = 0 }
}
function Unregister-ScheduledTask {
    param([switch]$Confirm)
    Wait-ModelCall 'unregister'
}
EOF
  cat > "$tmp/console-model.ps1" << 'EOF'
. "$PSScriptRoot/job-model.ps1"
$runner = Use-ModelJob (Get-Content -Raw -LiteralPath $args[0])
$script = Get-Content -Raw -LiteralPath $args[1]
$prelude = (Get-Content -Raw -LiteralPath "$PSScriptRoot/console-apis.ps1") +
    "`n" + (Get-Content -Raw -LiteralPath "$PSScriptRoot/console-prelude.ps1")
if ($env:MODEL_SCENARIO -like 'down-*') { $prelude = $prelude.Replace('$Mode = ''up''', '$Mode = ''down''') }
$script = $script -creplace '(?m)^    \[Security\.Principal\.WindowsIdentity\].*', '    $false'
$script = $script -creplace '(?m)^    \(\[Security\.Principal\.SecurityIdentifier\].*', '    ''rbtest'''
$script = $script.Replace('([Environment]::SystemDirectory)', '($PSHOME)')
if ($env:MODEL_SCENARIO -like 'hung-*') { $script = $script -replace '\b(8000|15000)\b', '600' }
if ($env:MODEL_SCENARIO -in 'down-stuck', 'down-poll-budget', 'hung-result') {
    $script = $script -replace '\b30000\b', '450' -replace '\b500\b', '50'
}
if ($env:MODEL_SCENARIO -eq 'down-poll-budget') { $script = $script -replace '\b8000\b', '1600' }
& ([scriptblock]::Create($runner)) 5 $prelude $script
exit $LASTEXITCODE
EOF
  console="$r/tests/rekordbox/win/kit/console.ps1"
  console_model() {
    : > "$tmp/model-calls"
    MODEL_SCENARIO="$1" MODEL_CALLS="$tmp/model-calls" MODEL_ACTION="$tmp/tscon-action.ps1" \
      pwsh -NoProfile -NonInteractive -File "$tmp/console-model.ps1" "$run" "$console" < /dev/null > "$tmp/model-out" 2> "$tmp/model-err"
  }
  console_model up
  [ "$?" = 0 ] && grep -qx 'CONSOLE ok 2' "$tmp/model-out" && ! grep -q 'console:' "$tmp/model-out" && ok || no 'console trace changed the stdout protocol'
  phases="$(sed -E 's/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{7}Z console: //' "$tmp/model-err" | paste -sd ',' -)"
  [ "$phases" = 'before admin identity check,before test account lookup,before Add-Type,model: Add-Type,before WTS session snapshot,before register tscon task,before start tscon task,model: tscon,before WTS session snapshot,before unregister tscon task' ] &&
    ok || no "console timestamps missing or out of phase ($phases)"

  cat > "$tmp/tscon-model.ps1" << 'EOF'
. "$PSScriptRoot/job-model.ps1"
function Invoke-ModelTscon($id, $destination) {
    if ($id -ne 2 -or $destination -ne '/dest:console') { exit 8 }
    [IO.File]::WriteAllText($env:MODEL_CHILD, 'running')
    Start-Sleep -Milliseconds 1600
    exit 9
}
$action = Use-ModelJob (Get-Content -Raw -LiteralPath $args[0])
$action = $action.Replace('& (Join-Path ($PSHOME) ''tscon.exe'')', 'Invoke-ModelTscon').Replace('20000', '600')
& ([scriptblock]::Create($action))
exit $LASTEXITCODE
EOF
  MODEL_CHILD="$tmp/tscon-child" pwsh -NoProfile -NonInteractive -File "$tmp/tscon-model.ps1" "$tmp/tscon-action.ps1" < /dev/null > "$tmp/action-out" 2> "$tmp/action-err"
  [ "$?" = 142 ] && [ "$(cat "$tmp/tscon-child" 2>/dev/null)" = 'terminated with job' ] &&
    ok || no "the scheduled action did not enforce its own tscon timeout ($(cat "$tmp/action-err"))"

  for step in compile register start snapshot result unregister; do
    console_model "hung-$step"; rc=$?
    [ "$rc" = 142 ] && ! grep -q '^CONSOLE ok' "$tmp/model-out" &&
      ok || no "blocked console phase $step escaped its deadline (exit $rc)"
    if [ "$step" = start ]; then
      [[ "$(tail -1 "$tmp/model-err")" == *'console: before start tscon task' ]] &&
        ok || no 'console task start trace was buffered when the job was killed'
    fi
  done

  console_model down-slow; rc=$?
  grep -qx 'disconnect 2 False' "$tmp/model-calls" &&
    ok || no 'console down asked Windows to wait synchronously'
  [ "$rc" = 0 ] && [ "$(tail -1 "$tmp/model-out")" = 'CONSOLE down' ] && [ "$(grep -c '^poll$' "$tmp/model-calls")" = 3 ] &&
    ok || no "console down failed to poll until the delayed disconnect (exit $rc)"

  console_model down-stuck; rc=$?
  [ "$rc" = 1 ] && [ "$(tail -1 "$tmp/model-out")" = 'CONSOLE still connected' ] && [ "$(grep -c '^poll$' "$tmp/model-calls")" -gt 2 ] &&
    ok || no "console down did not poll the unchanged state to its deadline (exit $rc)"

  console_model down-poll-budget; rc=$?
  [ "$rc" = 142 ] && grep -qx 'poll' "$tmp/model-calls" &&
    ok || no "console down let a snapshot exceed the remaining poll budget (exit $rc)"

  cat > "$tmp/cleanup-model.ps1" << 'EOF'
$env:COMPUTERNAME = 'TESTPC'
$legacy = '$ProgressPreference = "SilentlyContinue"; $s = [Console]::In.ReadToEnd(); & ([scriptblock]::Create($s)); exit $LASTEXITCODE'
$prefix = 'powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand '
$old = $prefix + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($legacy))
$current = (Get-Content -LiteralPath $args[1]) -replace '^powershell ', ('"' + (Join-Path $PSHOME 'powershell.exe') + '" ')
function Row($id, $minutes, $command) {
    [pscustomobject]@{ ProcessId = $id; CreationDate = [DateTime]::UtcNow.AddMinutes(-$minutes); CommandLine = $command }
}
$rows = @(
    (Row 401 21 $old), (Row 402 21 $current), (Row 403 19 $old),
    (Row 404 21 ($prefix + 'aaaa')), (Row 405 21 ($old + ' -Other')),
    (Row 406 21 ($prefix + $old.Substring($prefix.Length).ToLowerInvariant())),
    (Row 407 21 $old), (Row $PID 21 $old), (Row 408 21 $null),
    (Row 409 21 ($old.Replace('-NoProfile ', '-NoLogo -NoProfile '))),
    (Row 410 21 ($prefix + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('$name = [Reflection.AssemblyName]::new(''RbkitWatchdog''); $s = [Console]::In.ReadToEnd()')))),
    (Row 411 21 ($prefix + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("Write-Output 'other'"))))
)
function Get-CimInstance($class, $Filter) {
    if ($Filter -eq "Name = 'powershell.exe'") { $rows; return }
    if ($Filter -eq 'ProcessId = 407') { Row 407 0 ($prefix + 'aaaa'); return }
    $rows | Where-Object { $Filter -eq "ProcessId = $($_.ProcessId)" }
}
function Get-Process($Id) {
    [pscustomobject]@{ Id = $Id; Handle = 1; HasExited = $false } |
        Add-Member -MemberType ScriptMethod -Name Dispose -Value {} -PassThru
}
function Stop-Process($InputObject, [switch]$Force) { "MODEL STOP $($InputObject.Id)" }
& ([scriptblock]::Create((Get-Content -Raw -LiteralPath $args[0])))
EOF
  pwsh -NoProfile -NonInteractive -File "$tmp/cleanup-model.ps1" "$F/runner.ps1" "$tmp/console-command" > "$tmp/model-out" 2> "$tmp/model-err"
  [ "$?" = 0 ] && [ "$(grep '^MODEL STOP ' "$tmp/model-out" | paste -sd ',' -)" = 'MODEL STOP 401,MODEL STOP 402,MODEL STOP 410' ] &&
    [ "$(sed -n 's/^STOPPED kit runner PID \([0-9]*\) (started .*)$/\1/p' "$tmp/model-out" | paste -sd ',' -)" = '401,402,410' ] &&
    ok || no 'cleanup-runners did not stop only the exact stale legacy and current runners'
fi
# --- end kitps models
# session up: an existing session put on the console by the owner's login, for the test account's SID on its PC
reset; expect 0 "session up" session up
grep -q "^owner@pc.example	.*-EncodedCommand .*	.*COMPUTERNAME -ne 'TESTPC'.*\$Mode = 'up'; \$Sid = 'S-1-5-21-11-22-33-1001'.*tscon.exe" "$F/calls" && ok || no "session up: console.ps1 up not sent to the owner's login with the SID and PC check"
reset; printf 'REFUSED: another account is active on the console (someone at the PC): left as it is\n' > "$F/console-up-out"; echo 2 > "$F/console-up-rc"
expect 1 "session up, someone at the PC" session up; has "session up refused" "session up:" "$F/notified"
# console.ps1 timing out (142, the runner's watchdog): retried once with a fresh runner, nothing else
ups() { grep -c "\$Mode = 'up'" "$F/calls" 2> /dev/null; }
reset; printf '%s\n' '142 ' '0 SESSION 2 test Active console\nCONSOLE ok 2' > "$F/console-up-seq"
expect 0 "session up, a timeout then OK" session up
has "the retry in the log" "session: console up: retry after timeout" "$(ls -t "$tmp/out"/*-session-up*.log | head -1)"
[ "$(ups)" = 2 ] && ok || no "session up, a timeout then OK: $(ups) console up calls, not 2"
reset; printf '%s\n' '142 ' '142 ' > "$F/console-up-seq"
expect 1 "session up, two timeouts" session up; has "two timeouts STOP" "session up:" "$F/notified"
[ "$(ups)" = 2 ] && ok || no "session up, two timeouts: $(ups) console up calls, not 2"
reset; printf '%s\n' '1 REFUSED: no' > "$F/console-up-seq"
expect 1 "session up, exit 1" session up; hasnt "exit 1 not retried" "retry after timeout" "$tmp/o"
[ "$(ups)" = 1 ] && ok || no "session up, exit 1: $(ups) console up calls, not 1"
# the admin login's kit folder: copied (scp) when missing, refused and copied again when changed, and
# otherwise used as it is
reset; rm -rf "$F/adminfs"; expect 0 "session state, no admin copy yet" session state
has "missing: refused first" "RBKIT missing kitps-run.ps1" "$F/calls"; has "missing: copied" "/tests/rekordbox/win/kitps-run.ps1 " "$F/scp"; has "missing: copied" "/kit/console.ps1 owner@pc.example:rbsp-kit-admin/" "$F/scp"
cmp -s "$F/adminfs/console.ps1" "$r/tests/rekordbox/win/kit/console.ps1" && cmp -s "$F/adminfs/kitps-run.ps1" "$r/tests/rekordbox/win/kitps-run.ps1" && ok || no "the admin copy isn't the Mac's files"
reset; expect 0 "session state, the admin copy current" session state; [ ! -e "$F/scp" ] && ok || no "a current admin copy was copied again"
reset; printf '\n"tampered"\n' >> "$F/adminfs/console.ps1"; expect 0 "session state, a changed admin copy" session state
has "changed: refused first" "RBKIT mismatch console.ps1" "$F/calls"; hasnt "changed: never run" '"tampered"' "$F/calls"
cmp -s "$F/adminfs/console.ps1" "$r/tests/rekordbox/win/kit/console.ps1" && ok || no "a changed admin copy wasn't replaced"
reset; printf '%s\n' '142 ' '0 CONSOLE ok 2' > "$F/console-state-seq"
expect 0 "session state, a timeout then OK" session state; has "state retried" "session: console state: retry after timeout" "$tmp/o"
reset; printf '%s\n' '142 ' '0 SESSION 2 test Disconnected\nCONSOLE down' > "$F/console-down-seq"
expect 0 "session down, a timeout then OK" session down; has "down retried" "session: console down: retry after timeout" "$tmp/o"
# no session, no RDP client / no stored password: nothing tried, the owner told what to do once
mkdir -p "$F/rdp"; printf '#!/bin/bash\nexit 0\n' > "$F/rdp/fake-freerdp"; chmod +x "$F/rdp/fake-freerdp"
reset; echo '3 CONSOLE none' > "$F/console-up-seq"; expect 1 "session up, no session and no client" env RBKIT_FREERDP="$F/none" session up
has "no client" "no RDP client" "$tmp/o"; calls > "$tmp/c"; hasnt "no client" "tunnel" "$F/calls"
reset; echo '3 CONSOLE none' > "$F/console-up-seq"
expect 1 "session up, existing session only" env RBKIT_EXISTING_SESSION_ONLY=1 session up
has "existing session only stops before sign-in" "has no existing desktop session" "$tmp/o"
printf '#!/bin/bash\nexit 44\n' > "$F/bin/security"; chmod +x "$F/bin/security"
reset; echo '3 CONSOLE none' > "$F/console-up-seq"; expect 1 "session up, no stored password" env RBKIT_FREERDP="$F/rdp/fake-freerdp" session up
has "no password" "Keychain has no password" "$tmp/o"; hasnt "no password" "tunnel" "$F/calls"
# the sign-in: the password only on FreeRDP's stdin (never its arguments, a log or the calls), then on the console
cat > "$F/bin/security" << 'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$FAKE/security-args"
case " $* " in *' -w '*) echo 'pw-SENTINEL-7' ;; esac
EOF
cat > "$F/rdp/fake-freerdp" << 'EOF'
#!/bin/bash
printf '%s\n' "$@" > "$FAKE/rdp-argv"; cat > "$FAKE/rdp-stdin"; env > "$FAKE/rdp-env"; sleep 1
EOF
printf '#!/bin/bash\nexit 0\n' > "$F/bin/nc"; chmod +x "$F/bin/security" "$F/rdp/fake-freerdp" "$F/bin/nc"
reset; rm -f "$F/rdp-argv" "$F/rdp-stdin"; printf '3 CONSOLE none\n0 SESSION 2 test Active console\\nCONSOLE ok 2\n' > "$F/console-up-seq"
printf '0 CONSOLE none\n0 CONSOLE down\n' > "$F/console-state-seq"
expect 0 "session up, signed in over RDP" env RBKIT_POLL=1 RBKIT_FREERDP="$F/rdp/fake-freerdp" session up
[ "$(cat "$F/rdp-argv" 2> /dev/null)" = "/args-from:stdin" ] && ok || no "sign-in: FreeRDP's arguments aren't only /args-from:stdin: $(cat "$F/rdp-argv" 2> /dev/null)"
grep -qx '/p:pw-SENTINEL-7' "$F/rdp-stdin" 2> /dev/null && grep -qx '/u:rbtest' "$F/rdp-stdin" && ok || no "sign-in: the login not on FreeRDP's stdin"
grep -q '^SDL_VIDEO_DRIVER=dummy$' "$F/rdp-env" && ok || no "sign-in: FreeRDP would open a window on the Mac"
grep -q "tunnel	.*-L 127.0.0.1:[0-9]*:127.0.0.1:3389 -- rbtest@pc.example" "$F/calls" && ok || no "sign-in: no SSH tunnel as the test account"
if grep -rqF 'pw-SENTINEL-7' "$F/calls" "$tmp/out" "$F/security-args" "$F/notified" 2> /dev/null; then no "sign-in: the password reached a log, an argument or the calls"; else ok; fi
# cache: the worker through the owner's login (rekordbox quit first), the DLL checked after
reset; echo 'other 33d96af6' > "$F/ort-out"; expect 0 "cache install" cache install; calls > "$tmp/c"
has "cache install" "-Do quit" "$tmp/c"; has "cache install" "\$Verb = 'install'; \$Sid = 'S-1-5-21-11-22-33-1001'; \$Check = \$false" "$tmp/c"
grep -q "^owner@pc.example	.*Verb = 'install'" "$F/calls" && ok || no "cache install: the worker not on the owner's login"
[ "$(grep -n -- '-Do quit' "$tmp/c" | cut -d: -f1)" -lt "$(grep -n "Verb = 'install'" "$tmp/c" | cut -d: -f1)" ] && ok || no "cache install: the worker ran before rekordbox was quit"
reset; echo 'other 33d96af6' > "$F/ort-out"; expect 0 "cache install --chosen" cache install --chosen; calls > "$tmp/c"
has "cache install --chosen" "chosen.ps1') -Mode set" "$tmp/c"; has "cache install --chosen" "OK    the kit set cache=1" "$tmp/o"
reset; echo 'other 33d96af6' > "$F/ort-out"; echo 1 > "$F/chosenset-rc"; expect 1 "cache install --chosen, not set" cache install --chosen
reset; echo 'other 33d96af6' > "$F/ort-out"; expect 0 "cache install, no --chosen" cache install; calls > "$tmp/c"; hasnt "cache install, no --chosen" "chosen.ps1') -Mode set" "$tmp/c"
expect 2 "cache uninstall --chosen" cache uninstall --chosen
reset; echo 'pioneer' > "$F/ort-out"; expect 1 "cache install, the DLL still Pioneer's" cache install; has "cache install" "cache install: not confirmed" "$F/notified"
reset; printf 'worker: x\nWORKER exit 5\n' > "$F/worker-out"; echo 'other 1' > "$F/ort-out"; expect 1 "cache install, worker exit 5" cache install
reset; printf 'REFUSED: rekordbox is running (session 3)\n' > "$F/worker-out"; echo 2 > "$F/worker-rc"; echo 'other 1' > "$F/ort-out"; expect 1 "cache install, refused" cache install
reset; printf 'x  quit: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/quit-out"; echo 1 > "$F/quit-rc"
expect 1 "cache install, the quit STOPs" cache install; calls > "$tmp/c"; hasnt "cache quit STOP" "\$Verb = " "$tmp/c"
reset; expect 0 "cache uninstall" cache uninstall; calls > "$tmp/c"; has "cache uninstall" "\$Verb = 'uninstall'" "$tmp/c"
reset
printf 'chosen: C:\fixture\installed put back as it was\n' > "$F/chosenrestore-out"
expect 0 "cache uninstall restores the temporary choice at the phase boundary" cache uninstall
calls > "$tmp/c"
awk '/-Do quit/ { quit = NR } /-Mode restore/ { restore = NR } /Verb = .uninstall./ { worker = NR }
     END { exit !(quit && quit < restore && restore < worker) }' "$tmp/c" &&
  ok || no "cache uninstall: choice was not restored between quit and the worker"
has "cache uninstall logs the restored choice" 'put back as it was' "$tmp/o"
reset; echo 1 > "$F/chosenrestore-rc"
expect 1 "cache uninstall stops when the temporary choice cannot be restored" cache uninstall
calls > "$tmp/c"; hasnt "failed choice restore stops before uninstall" "\$Verb = 'uninstall'" "$tmp/c"
reset; expect 0 "cache check" cache check; calls > "$tmp/c"; has "cache check" "\$Check = \$true" "$tmp/c"; hasnt "cache check" "-Do quit" "$tmp/c"
# cleanup --lock: the session disconnected (locked), not signed out
reset; expect 0 "cleanup --lock" cleanup --lock; calls > "$tmp/c"; has "cleanup --lock" "\$Mode = 'down'" "$tmp/c"; hasnt "cleanup --lock" "-Do logoff" "$tmp/c"
reset; expect 0 "cleanup --lock --no-snapshot" cleanup --lock --no-snapshot
calls > "$tmp/c"; has "cleanup without snapshot still locks" "\$Mode = 'down'" "$tmp/c"; hasnt "cleanup without snapshot skips the state copy" "rbstate.ps1" "$tmp/c"
reset; printf 'CONSOLE still connected\n' > "$F/console-down-out"; echo 1 > "$F/console-down-rc"; expect 1 "cleanup --lock, not disconnected" cleanup --lock
# run directml: loaded 0, not loaded 3 (an expectation, not a STOP)
reset; expect 0 "run directml" run directml
reset; printf 'x  modules: DirectML not loaded\n' > "$F/modules-out"; echo 3 > "$F/modules-rc"; expect 3 "run directml, not loaded" run directml
# run appswap: the TEST build in --dist, served under --base, its bootstrap byte-checked on the PC
mkdir -p "$tmp/fixdist"; echo boot > "$tmp/fixdist/bootstrap.ps1"; echo '{}' > "$tmp/fixdist/payload.json"
expect 2 "appswap without --base" run appswap --dist "$tmp/fixdist"; expect 2 "appswap without a build" run appswap --dist "$tmp" --base x
expect 2 "appswap --base with a slash" run appswap --dist "$tmp/fixdist" --base a/b
git -C "$r" config rbstemsplus.pcTestServer mac.test:8766
reset; expect 0 "appswap" run appswap --dist "$tmp/fixdist" --base fixblack; calls > "$tmp/c"
has "appswap" "-Do appswap -Argument 'https://mac.test:8766/fixblack|http://mac.test:8766/fixblack|$(shasum -a 256 "$tmp/fixdist/bootstrap.ps1" | cut -c1-64)'" "$tmp/c"
reset; expect 0 "appswap at the root" run appswap --dist "$tmp/fixdist" --base /; calls > "$tmp/c"; has "appswap at the root" "-Argument 'https://mac.test:8766|http://mac.test:8766|" "$tmp/c"
reset; echo 1 > "$F/appswap-rc"; expect 1 "appswap, a STOP" run appswap --dist "$tmp/fixdist" --base fixblack
reset; printf '0 leftover\n0 clear\n' > "$F/swap-cache-seq"
expect 0 "appswap clears a leftover root" run appswap --dist "$tmp/fixdist" --base fixblack
phases="$(awk -F '\t' '/Get-AppSwapCacheState/ { print "probe" }
  $1 == "owner@pc.example" && /Verb = .uninstall./ { print "admin uninstall" }
  /-Do appswap/ { print "swap" }' "$F/calls")"
[ "$phases" = $'probe\nadmin uninstall\nprobe\nswap' ] && ok || no "appswap leftover: expected probe, admin uninstall, recheck, swap (got: $phases)"
reset; echo leftover > "$F/swap-cache-out"
printf 'REFUSED: fixture-private-path\n' > "$F/worker-out"; echo 2 > "$F/worker-rc"
expect 1 "appswap stops when admin uninstall refuses" run appswap --dist "$tmp/fixdist" --base fixblack
has "appswap refusal" "appswap: STOP: the admin cache uninstall could not clear" "$tmp/o"
hasnt "appswap refusal" "fixture-private-path" "$tmp/o"
hasnt "appswap refusal" "-Do appswap" "$F/calls"
reset; echo leftover > "$F/swap-cache-out"
expect 1 "appswap stops when the leftover root remains" run appswap --dist "$tmp/fixdist" --base fixblack
has "appswap root remains" "appswap: STOP: the Stems Cache root could not be confirmed absent" "$tmp/o"
hasnt "appswap root remains" "-Do appswap" "$F/calls"
git -C "$r" config --unset rbstemsplus.pcTestServer
# run stopkeep: our app's Stop Keeping… on the kept set; run prepare --pause-test passes -PauseTest
expect 2 "rowkeys without --from" run rowkeys
expect 2 "loc without --step" run loc
expect 2 "loc, an unknown step" run loc --step format
expect 2 "loc set without --arg" run loc --step set
expect 2 "loc, an argument with a quote" run loc --step set --arg "D:\\x'y"
reset; expect 0 "loc set" run loc --step set --arg 'D:\rbsp-test'; calls > "$tmp/c"; has "loc set" "-Do loc -LocStep set -LocArg 'D:\rbsp-test'" "$tmp/c"
reset; expect 0 "loc texts" run loc --step texts; calls > "$tmp/c"; has "loc texts" "-Do loc -LocStep texts -LocArg ''" "$tmp/c"
reset; expect 0 "rowkeys" run rowkeys --from 06-aac-48k; calls > "$tmp/c"; has "rowkeys" "-Do rowkeys -From '06-aac-48k'" "$tmp/c"
reset; expect 0 "keptinfo" run keptinfo; calls > "$tmp/c"; has "keptinfo" "-Do keptinfo" "$tmp/c"
reset; expect 0 "stopkeep" run stopkeep; calls > "$tmp/c"; has "stopkeep" "-Do stopkeep -Playlist 'RBSP Tests'" "$tmp/c"
reset; echo 1 > "$F/stopkeep-rc"; expect 1 "stopkeep, refused" run stopkeep
reset; expect 0 "prepare --pause-test" run prepare --minutes 5 --pause-test; calls > "$tmp/c"; has "prepare --pause-test" "-Minutes 5 -PauseTest" "$tmp/c"
# run playlists: the installed helper's --playlists alone, rekordbox closed (quit first) or open
reset; expect 0 "playlists" run playlists; calls > "$tmp/c"; has "playlists" "-Do playlists -Playlist 'RBSP Tests'" "$tmp/c"; hasnt "playlists" "-Do start" "$tmp/c"
reset; expect 0 "playlists --open" run playlists --open; calls > "$tmp/c"; [ "$(grep -n -- '-Do start' "$tmp/c" | cut -d: -f1)" -lt "$(grep -n -- '-Do playlists' "$tmp/c" | cut -d: -f1)" ] && [ "$(grep -n -- '-Do quit' "$tmp/c" | tail -1 | cut -d: -f1)" -gt "$(grep -n -- '-Do playlists' "$tmp/c" | cut -d: -f1)" ] && ok || no "playlists --open: start, then playlists, then quit"
reset; echo 3 > "$F/playlists-rc"; expect 3 "playlists, not listed" run playlists; has "playlists, not listed" "NOTE  the helper's --playlists: see its lines above" "$tmp/o"
# run blackrepro: a fresh app, then cycles of minimize, session down/up, restore; checked every 5
reset; expect 0 "blackrepro" run blackrepro --cycles 6; calls > "$tmp/c"
[ "$(grep -c -- '-Do appmin' "$tmp/c")" = 6 ] && [ "$(grep -c -- '-Do appcheck' "$tmp/c")" = 3 ] && [ "$(grep -c "\$Mode = 'down'" "$tmp/c")" = 6 ] && ok || no "blackrepro: 6 minimizes, 6 downs and 3 checks (fresh, 5, 6)"
has "blackrepro" "OK    blackrepro (minimized each time): no repro after 6 cycles" "$tmp/o"
reset; expect 0 "blackrepro --no-minimize" run blackrepro --cycles 5 --no-minimize; calls > "$tmp/c"; hasnt "blackrepro --no-minimize" "-Do appmin" "$tmp/c"
reset; printf '0 x  appcheck: drawn\n3 x  appcheck: black 0.98\n' > "$F/appcheck-seq"; expect 3 "blackrepro, black" run blackrepro --cycles 10
has "blackrepro, black" "blackrepro (minimized each time): black first seen at the check after cycle 5 of 10" "$tmp/o"
reset; echo 3 > "$F/appcheck-rc"; expect 3 "blackrepro, a black fresh app" run blackrepro --cycles 5; has "blackrepro, a black fresh app" "blackrepro: the fresh app is BLACK" "$tmp/o"
has "blackrepro, a black fresh app" "checks drawn 0, white 0, black 2" "$tmp/o"
reset; echo 4 > "$F/appcheck-rc"; expect 3 "blackrepro, white" run blackrepro --cycles 2 --every 1; has "blackrepro, white" "checks drawn 0, white 3, black 0" "$tmp/o"
# run preconditions: every failed one in one BAD row; an auto plan runs it right after preflight and
# stops there, before anything changes on the PC
expect 2 "preconditions without --plan" run preconditions; expect 2 "preconditions --plan bogus" run preconditions --plan bogus
reset; expect 0 "preconditions" run preconditions --plan session2; calls > "$tmp/c"; has "preconditions" "-Do preconditions -Plan session2" "$tmp/c"
reset; printf "x  preconditions: BAD the probe's C:\\\\rbtest is there\nx  preconditions: 2 failed: the probe's C:\\\\rbtest is there | rekordbox's onnxruntime.dll is not Pioneer's (other 1)\n" > "$F/preconditions-out"; echo 1 > "$F/preconditions-rc"
expect 1 "preconditions failed" run preconditions --plan prepare
has "preconditions failed" "BAD   auto prepare: 2 failed: the probe's C:\\rbtest is there | rekordbox's onnxruntime.dll is not Pioneer's (other 1)" "$tmp/o"
reset; echo 1 > "$F/preconditions-rc"; expect 1 "auto session2, preconditions failed" auto session2; calls > "$tmp/c"
hasnt "auto preconditions failed" "-Do autoanalysis" "$tmp/c"; hasnt "auto preconditions failed" "\$Verb = 'install'" "$tmp/c"; has "auto preconditions failed" "auto session2: preconditions not met" "$F/notified"
# run autoanalysis-off (owner decision 2026-11-19): nothing when already 0; else start, Preferences,
# quit, read back; a read-back that isn't 0 is a STOP
reset; expect 0 "autoanalysis-off, already off" run autoanalysis-off; calls > "$tmp/c"
hasnt "autoanalysis-off, already off" "-Do start" "$tmp/c"; hasnt "autoanalysis-off, already off" "-Do autoanalysis;" "$tmp/c"
reset; printf '3 x  autoanalysis: rekordbox3.settings AutoAnalysis=1\n0 x  autoanalysis: rekordbox3.settings AutoAnalysis=0\n' > "$F/autoanalysis-read-seq"
expect 0 "autoanalysis-off" run autoanalysis-off; calls > "$tmp/c"
[ "$(grep -o -- '-Do [a-z-]*' "$tmp/c" | tr '\n' ' ')" = "-Do autoanalysis-read -Do start -Do autoanalysis -Do quit -Do autoanalysis-read " ] && ok || no "autoanalysis-off's order: $(grep -o -- '-Do [a-z-]*' "$tmp/c" | tr '\n' ' ')"
has "autoanalysis-off" "OK    rekordbox's Auto Analysis off" "$tmp/o"
reset; printf '3 x  AutoAnalysis=1\n3 x  AutoAnalysis=1\n' > "$F/autoanalysis-read-seq"; expect 1 "autoanalysis-off, not read back" run autoanalysis-off
has "autoanalysis-off, not read back" "the setting did not read back as off" "$F/notified"
reset; printf '3 x  AutoAnalysis=1\n' > "$F/autoanalysis-read-seq"; printf 'x  autoanalysis: STOP: expected one Enable/Disable check box\n' > "$F/autoanalysis-out"; echo 1 > "$F/autoanalysis-rc"
expect 1 "autoanalysis-off, Preferences STOP" run autoanalysis-off; calls > "$tmp/c"; hasnt "autoanalysis-off, Preferences STOP" "-Do quit" "$tmp/c"
# a model change: our app closed by its own Exit first, started again after (owner decision 2026-11-19)
reset; expect 0 "t2-model-on" run t2-model-on; calls > "$tmp/c"
[ "$(grep -oE -- "-Do appexit|-Step 't2-model-on'|-Do appopen" "$tmp/c" | tr '\n' ' ')" = "-Do appexit -Step 't2-model-on' -Do appopen " ] && ok || no "t2-model-on's order: $(grep -oE -- "-Do [a-z]+|-Step '[a-z0-9-]+'" "$tmp/c" | tr '\n' ' ')"
reset; echo 1 > "$F/appexit-rc"; expect 1 "t2-model-on, the app stays" run t2-model-on; calls > "$tmp/c"; hasnt "t2-model-on, the app stays" "-Step 't2-model-on'" "$tmp/c"
# run dml-bench: rekordbox quit, the timing, run hits on the same rows, the playback check, quit
reset; expect 0 "dml-bench" run dml-bench; calls > "$tmp/c"
[ "$(grep -o -- '-Do [a-z]*' "$tmp/c" | grep -v -- '-Do start' | tr '\n' ' ')" = "-Do quit -Do dmlbench -Do run -Do dmlstall -Do quit " ] && ok || no "dml-bench's order: $(grep -o -- '-Do [a-z]*' "$tmp/c" | tr '\n' ' ')"
has "dml-bench rows" "-Do dmlbench -From '19-mp3-cbr128' -To '20-wav-16-30s' -Minutes 30" "$tmp/c"
has "dml-bench hits" "-Do run -Expect hits -From '19-mp3-cbr128' -To '20-wav-16-30s'" "$tmp/c"
has "dml-bench stall row" "-Do dmlstall -From '09-flac-16' -Minutes 15" "$tmp/c"
reset; echo 3 > "$F/dmlbench-rc"; expect 3 "dml-bench, DirectML fell back" run dml-bench --from 01-mp3-cbr320 --to 02-mp3-vbr2
has "dml-bench fell back" "NOTE  DirectML fell back to the CPU" "$tmp/o"; calls > "$tmp/c"; has "dml-bench --from" "-From '01-mp3-cbr320' -To '02-mp3-vbr2'" "$tmp/c"
reset; echo 1 > "$F/dmlbench-rc"; expect 1 "dml-bench, a failed run" run dml-bench; calls > "$tmp/c"; hasnt "dml-bench, a failed run" "-Do dmlstall" "$tmp/c"
reset; echo 3 > "$F/run-rc"; expect 3 "dml-bench, hits not met" run dml-bench; has "dml-bench, hits not met" "BAD   run hits on the --dml entries: exit 3" "$tmp/o"
reset; expect 2 "dml-bench --helper not a helper" run dml-bench --helper "$tmp/c"
# auto: the plan's steps in order; a STOP ends it there (no cleanup) and notifies
reset; expect 0 "auto session2" auto session2 --max 2; a="$(ls -t "$tmp/out"/*-auto-session2*.log | head -1)"
order="$(sed -n 's/^auto: --- \([^:]*\):.*/\1/p' "$a" | tr '\n' ' ')"
[ "$order" = "cleanup-runners session preflight preconditions autoanalysis t2-facts watchtest t2-model-on cache-install T2 hits directml cache-uninstall t2-model-off collect cleanup " ] && ok || no "auto session2's steps: $order"
has "auto session2" "pc-session.sh run T2 --max 2" "$a"; has "auto session2" "pc-session.sh cleanup --lock" "$a"; has "auto session2" "SKIP  T11 FMA3 proof: not built yet" "$a"
reset; printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 0x40; shot x\n' > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "auto session2, a STOP in T2" auto session2; calls > "$tmp/c"; hasnt "auto STOP" "\$Mode = 'down'" "$tmp/c"; hasnt "auto STOP" "rbstate.ps1" "$tmp/c"; hasnt "auto STOP" "-Do bundle" "$tmp/c"
has "auto STOP" "run T2: new window(s): rekordbox | Sign in" "$F/notified"
reset; echo 3 > "$F/run-rc"; expect 3 "auto session2, expectations not met" auto session2; calls > "$tmp/c"; has "auto exit 3 goes on" "\$Mode = 'down'" "$tmp/c"
reset; echo '3 CONSOLE none' > "$F/console-up-seq"; expect 1 "auto prepare, no session" env RBKIT_FREERDP="$F/none" auto prepare; calls > "$tmp/c"; hasnt "auto no session" "verify-kit" "$tmp/c"
reset; expect 0 "auto prepare" auto prepare; a="$(ls -t "$tmp/out"/*-auto-prepare*.log | head -1)"
order="$(sed -n 's/^auto: --- \([^:]*\):.*/\1/p' "$a" | tr '\n' ' ')"
[ "$order" = "cleanup-runners session preflight preconditions t2-model-on cache-install prepare hits cache-uninstall t2-model-off collect cleanup " ] && ok || no "auto prepare's steps: $order"
pending="$(sed -n 's/^SKIP  \(W13b [^:]*\): not built yet .*/\1/p' "$a" | tr '\n' '|')"
[ "$pending" = "W13b settings-location|W13b bridge-release|W13b watcher-reminder|W13b watcher-dedupe|W13b watcher-active-set|W13b watcher-set-filter|" ] &&
  ok || no "auto prepare's owner-only W13b steps: $pending"
# auto record: pc-win.ok only for a clean plan, bound to the commit and the TEST payload the PC installed
cp "$repo/tests/win/gate-record.py" "$r/tests/win/"; git -C "$r" add tests/win/gate-record.py
git -C "$r" -c user.name=t -c user.email=t@t commit -q --allow-empty -m rc; rc_c="$(git -C "$r" rev-parse HEAD)"
mkdir -p "$tmp/dist"; echo '{"x":1}' > "$tmp/dist/payload.json"; want="$(shasum -a 256 "$tmp/dist/payload.json" | cut -d' ' -f1)"
export RBKIT_GATES="$tmp/gates"
reset; echo "$want" > "$F/payload-out"; expect 0 "auto record" auto record --commit "$rc_c" --dist "$tmp/dist"
g="$tmp/gates/$rc_c/pc-win.ok"
grep -qx "commit $rc_c" "$g" 2> /dev/null && grep -qx "payload_sha256 $want" "$g" && grep -qx 'result pass' "$g" && ok || no "auto record: pc-win.ok not as release.sh's records are (commit, payload_sha256, result pass)"
python3 -I "$repo/tests/win/gate-record.py" check "$g" "$rc_c" "$want" > /dev/null && ok || no "auto record: the release gate (tests/win/gate-record.py check) refuses the pc-win.ok auto record wrote"
! grep -q '^step \(cleanup-runners\|preconditions\) ' "$g" && ok || no "auto record: pc-win.ok carries steps the gate doesn't list"
reset; echo 'ffff' > "$F/payload-out"; expect 1 "auto record, another payload on the PC" auto record --commit "$rc_c" --dist "$tmp/dist"
[ ! -e "$g" ] && ok || no "auto record: pc-win.ok written for another payload, or the earlier passing one kept"
! ls -a "$tmp/gates/$rc_c" | grep -q '^\.pc-win' && ok || no "auto record: its temp folder left in the gates folder"
reset; echo "$want" > "$F/payload-out"; echo 3 > "$F/run-rc"; expect 3 "auto record, an expectation not met" auto record --commit "$rc_c" --dist "$tmp/dist"
[ ! -e "$g" ] && ok || no "auto record: pc-win.ok written after an expectation failed"
# --cpu-stems-ok (the owner's ruling): directml's NOTE alone doesn't block the record, any other expectation still does
reset; echo "$want" > "$F/payload-out"; printf 'x  modules: DirectML not loaded\n' > "$F/modules-out"; echo 3 > "$F/modules-rc"
expect 3 "auto record, DirectML not loaded" auto record --commit "$rc_c" --dist "$tmp/dist"
[ ! -e "$g" ] && ok || no "auto record: pc-win.ok written without DirectML and without --cpu-stems-ok"
reset; echo "$want" > "$F/payload-out"; printf 'x  modules: DirectML not loaded\n' > "$F/modules-out"; echo 3 > "$F/modules-rc"
expect 0 "auto record --cpu-stems-ok, DirectML not loaded" auto record --commit "$rc_c" --dist "$tmp/dist" --cpu-stems-ok
grep -qx 'step directml 3' "$g" 2> /dev/null && grep -qx 'directml cpu, accepted by --cpu-stems-ok' "$g" && grep -qx 'result pass' "$g" && ok ||
  no "auto record --cpu-stems-ok: pc-win.ok doesn't show the accepted CPU stems"
rm -rf "$tmp/gates"; reset; echo "$want" > "$F/payload-out"; expect 0 "auto record --cpu-stems-ok, DirectML loaded" auto record --commit "$rc_c" --dist "$tmp/dist" --cpu-stems-ok
grep -qx 'step directml 0' "$g" 2> /dev/null && ! grep -q '^directml cpu' "$g" && ok || no "auto record --cpu-stems-ok: the CPU line written though DirectML loaded"
rm -rf "$tmp/gates"; reset; echo "$want" > "$F/payload-out"; echo 3 > "$F/run-rc"; echo 3 > "$F/modules-rc"
expect 3 "auto record --cpu-stems-ok, another expectation not met" auto record --commit "$rc_c" --dist "$tmp/dist" --cpu-stems-ok
[ ! -e "$g" ] && ok || no "auto record --cpu-stems-ok: pc-win.ok written though another expectation failed"
reset; expect 2 "--cpu-stems-ok on session2" auto session2 --cpu-stems-ok
# M12 is opt-in on each plan, with the server switched between install and self-update.
mkdir -p "$F/share/old" "$F/share/new" "$tmp/uia"
echo before > "$F/share/serve-current.txt"
for slot in old new; do
  printf '{"fixture":"%s"}\n' "$slot" > "$F/share/$slot/payload.json"
  cp "$tmp/dist/test-ca.pem" "$F/share/$slot/test-ca.pem"
  touch "$F/share/$slot/bootstrap.ps1"
done
touch "$tmp/uia/rbui.exe"
echo 'downloaded test fixture' > "$F/share/new/setup.exe"
git -C "$r" config rbstemsplus.pcTestServer mac:8766
m12args=(--from-dist "$F/share/old" --dist "$F/share/new" --uia "$tmp/uia" --bootstrap-server mac:8765 --installer setup.exe)
for plan in session2 prepare record; do
  reset; rm -f "$F/served"
  opts=(); [ "$plan" != record ] || opts=(--commit "$rc_c")
  shasum -a 256 < "$F/share/new/payload.json" | cut -d' ' -f1 > "$F/payload-out"
  expect 0 "M12 on $plan" auto "$plan" "${m12args[@]}" ${opts[@]+"${opts[@]}"}
  order="$(sed -n 's/^auto: --- \(m12-[^:]*\):.*/\1/p' "$tmp/o" | paste -sd ' ' -)"
  tail_steps='m12-smartscreen'; [ "$plan" != record ] || tail_steps="m12-restore-rc $tail_steps"
  [ "$order" = "m12-quit m12-install m12-update m12-pin m12-worker-source m12-settings-kept m12-signout m12-session m12-settings-retry $tail_steps" ] &&
    ok || no "M12 $plan order: $order"
  has "M12 $plan serves N for bootstrap" 'install-old old' "$F/served"
  has "M12 $plan serves N+1 for update" 'update new' "$F/served"
  calls > "$tmp/c"
  has "M12 $plan preflight checks TEST server" "-Server 'mac:8766' -BootstrapServer 'mac:8765'" "$tmp/c"
done
reset; printf 'x  m12-smartscreen: STOP: SmartScreen observed; smartscreen | Windows protected your PC; shot x\n' > "$F/m12-smartscreen-out"
# Even an erroneously successful transport result must preserve a STOP.
expect 1 "M12 SmartScreen STOP" auto prepare "${m12args[@]}"
has "M12 SmartScreen evidence" 'SmartScreen observed; smartscreen | Windows protected your PC; shot x' "$tmp/o"
has "M12 SmartScreen notified" 'auto prepare: SmartScreen observed' "$F/notified"
calls > "$tmp/c"; hasnt "M12 SmartScreen leaves the desktop" "-Do bundle" "$tmp/c"
git -C "$r" config --unset rbstemsplus.pcTestServer
# The B/C plan uses a fresh rekordbox for each run and leaves the dump lane alone.
mkdir -p "$tmp/t2-probe"; echo probe > "$tmp/t2-probe/onnxruntime.dll"
reset; expect 0 "T2 B/C" auto session2 --probe "$tmp/t2-probe/onnxruntime.dll"
order="$(sed -n 's/^auto: --- \(t2-[^:]*\):.*/\1/p' "$tmp/o" | paste -sd ' ' -)"
[ "$order" = 't2-facts t2-model-on t2-probe-install t2-B t2-C1 t2-C2 t2-C3 t2-probe-evidence t2-probe-remove t2-model-off' ] &&
  ok || no "T2 B/C step order: $order"
calls > "$tmp/c"
has "T2 probe keys only" "\$NoDumps = \$true" "$tmp/c"
[ "$(grep -c -- '-Do run -Expect keys .* -Aac -Warmup 90' "$tmp/c")" = 3 ] && ok || no "T2 requires three AAC starts"
[ "$(grep -- '-Do run -Expect keys .* -Warmup 90' "$tmp/c" | grep -vc -- '-Aac')" = 1 ] && ok || no "T2 B is one whole-corpus start"
awk '
  /-Do start/ { fresh = prev ~ /-Do quit/ }
  /-Do run -Expect keys .* -Warmup 90/ { n++; if (!fresh) bad++ }
  { prev = $0 }
  END { exit !(n == 4 && !bad) }
' "$tmp/c" && ok || no "T2 B/C start without a preceding quit"
reset; echo 1 > "$F/t2-facts-rc"; expect 1 "T2 pre-load facts STOP" auto session2
calls > "$tmp/c"; hasnt "T2 pre-load facts STOP" "-Do start" "$tmp/c"
has "T2 pre-load facts notified" "auto session2: t2-facts exited 1" "$F/notified"
# Run the corpus generator with capture-only encoders, keeping the actual generation arguments.
mkdir -p "$tmp/audio-bin"
cat > "$tmp/audio-bin/ffmpeg" << 'EOF'
#!/bin/bash
printf '%s\n' "$*" >> "$AUDIO_TRACE"
touch "${@: -1}"
EOF
chmod +x "$tmp/audio-bin/ffmpeg"
AUDIO_TRACE="$tmp/audio" PATH="$tmp/audio-bin:$PATH" RBKIT_HE_FFMPEG="$tmp/audio-bin/ffmpeg" "$here/gen-corpus-win.sh" "$tmp/audio-corpus" > "$tmp/audio-out" 2>&1 &&
  ok || { no "T2 corpus generator"; cat "$tmp/audio-out"; }
rg -q 's=44100:d=30.*six44.wav$' "$tmp/audio" && rg -q 'six44.wav.*-channel_layout 5.1.*w22-wav-5.1-44100.wav$' "$tmp/audio" &&
  ok || no "T2 5.1 source must be 44.1 kHz"
for w in 'between(n,482028,967127)' 'atrim=end_sample=484076' 'atrim=end_sample=484077' '-profile:a aac_he -b:a 48k' '-profile:a aac_he_v2 -b:a 32k'; do
  has "T2 corpus" "$w" "$tmp/audio"
done
for f in w17-aac-he.m4a w18-aac-he-v2.m4a w19-flac-chunk-silent.flac w20-wav-chunk-edge.wav w21-wav-chunk-edge.wav w22-wav-5.1-44100.wav; do
  has "T2 corpus manifest" "  $f" "$tmp/audio-corpus/SHA256SUMS"
done
unset RBKIT_GATES
# the probe for T2's mix dumps: placed by the owner's login (rekordbox quit first), the runs read its log
mkdir -p "$tmp/pr"; printf 'not a dll' > "$tmp/pr/onnxruntime.dll"; psha="$(shasum -a 256 "$tmp/pr/onnxruntime.dll" | cut -d' ' -f1)"
expect 2 "probe bogus" probe bogus; expect 2 "--keys on hits" run hits --keys; expect 2 "--probe not the DLL" probe install --probe "$tmp/dist/payload.json"
reset; expect 0 "probe install" probe install --probe "$tmp/pr/onnxruntime.dll"; calls > "$tmp/c"
has "probe install" "\$Mode = 'install'; \$Sid = 'S-1-5-21-11-22-33-1001'; \$ProbeSha = '$psha'" "$tmp/c"
grep -q "^owner@pc.example	.*Mode = 'install'.*# probe.ps1:" "$F/calls" && ok || no "probe install: not on the owner's login"
[ "$(grep -n -- '-Do quit' "$tmp/c" | cut -d: -f1)" -lt "$(grep -n 'ResolveOnly = \$false' "$tmp/c" | cut -d: -f1)" ] && ok || no "probe install: placed before rekordbox was quit"
reset; printf "REFUSED: rekordbox's onnxruntime.dll isn't Pioneer's (33d96af6): Stems Cache installed?\n" > "$F/probe-install-out"; echo 2 > "$F/probe-install-rc"
expect 1 "probe install refused" probe install --probe "$tmp/pr/onnxruntime.dll"; has "probe refused" "probe install: not confirmed" "$F/notified"
reset; expect 0 "probe remove" probe remove; calls > "$tmp/c"; has "probe remove" "\$Mode = 'remove'" "$tmp/c"
reset; expect 0 "run T2 --keys" run T2 --keys --from 09-flac-16 --to 09-flac-16; calls > "$tmp/c"
has "run T2 --keys" "-Do run -Expect keys -Activity 'C:\\rbtest\\probe.log' -Quiet 30 -From '09-flac-16' -To '09-flac-16'" "$tmp/c"
reset; expect 2 "malformed rekordbox pin" run T2 --rekordbox "7.2.19'; exit"
[ ! -s "$F/calls" ] && ok || no "malformed rekordbox pin reached the transport"
reset; expect 0 "run T2 pinned to 7.2.19" run T2 --rekordbox 7.2.19; calls > "$tmp/c"
for action in start run quit; do
  grep -- "-Do $action " "$tmp/c" | grep -qF -- "-RbVersion '7.2.19'" &&
    ok || no "run T2 lost the 7.2.19 pin for $action"
done
reset; expect 0 "probe restores the resolved version" probe remove; calls > "$tmp/c"
has "probe close follows the resolved version" "-Do quit -RbVersion '7.2.19'" "$tmp/c"
grep -F 'ResolveOnly = $false' "$tmp/c" | grep -qF "\$RbVersion = '7.2.19'" &&
  ok || no "probe remove lost the resolved version"
# the assembler: chunks as rekordbox cuts a track (helper/rbsp.h), out of order, back to the track
python3 - "$F/probe-fetch" << 'EOF'
import random, struct, sys
from pathlib import Path
L, HOP, PRE = 485100, 483052, 1024
random.seed(7); n = 2 * HOP + 1000
tr = [[random.uniform(-1, 1) for _ in range(n)] for _ in range(2)]
d = Path(sys.argv[1]); (d / "dump").mkdir(parents=True, exist_ok=True); rows = []
for i, ms in ((2, 1700000000300), (0, 1700000000100), (1, 1700000000200)):
    s = i * HOP - PRE
    ch = [[tr[c][j] if 0 <= j < n else 0.0 for j in range(s, s + L)] for c in range(2)]
    key = f"{i:x}" * 64
    (d / "dump" / f"{ms:013d}-{key[:64]}.mix").write_bytes(struct.pack(f"<{2 * L}f", *(ch[0] + ch[1])))
    rows.append(f"09-flac-16 {ms} {key[:64]} {L}")
(d / "rows.txt").write_text("\n".join(rows) + "\n"); (d / "probe.log").write_text("probe loaded\n")
(d / "want.f32").write_bytes(struct.pack(f"<{2 * n}f", *[tr[c][j] for j in range(n) for c in range(2)]))
EOF
reset; expect 0 "probe fetch" probe fetch; fd="$(ls -dt "$tmp/out"/dumps-* | head -1)"
cmp -s "$fd/tracks/09-flac-16.rb.f32" "$F/probe-fetch/want.f32" && ok || no "probe fetch: the assembled track isn't the original (chunk order, hop, padding)"
grep -q '"frames": 967104' "$fd/tracks/09-flac-16.json" 2> /dev/null && grep -q '"order": "shared samples"' "$fd/tracks/09-flac-16.json" && ok || no "probe fetch: the track's json (frames, order)"
reset; expect 0 "auto dumps" auto dumps --probe "$tmp/pr/onnxruntime.dll"; a="$(ls -t "$tmp/out"/*-auto-dumps*.log | head -1)"
order="$(sed -n 's/^auto: --- \([^:]*\):.*/\1/p' "$a" | tr '\n' ' ')"
[ "$order" = "cleanup-runners session stage preflight preconditions probe-install dumps 01-mp3-cbr320 dumps 09-flac-16 dumps 17-aiff-24-48k dumps 19-mp3-cbr128 dumps w10-mp4-video-first probe-fetch probe-remove collect cleanup " ] && ok || no "auto dumps' steps: $order"
has "auto dumps" "run T2 --keys --from w10-mp4-video-first --to w10-mp4-video-first" "$a"; hasnt "auto dumps: the last run quits" "--to w10-mp4-video-first --keep-open" "$a"
has "auto dumps stages the probe" "probe/onnxruntime.dll" "$(ls -dt "$tmp/out"/stage-* | head -1)/SHA256SUMS"
reset; echo 2 > "$F/probe-install-rc"; expect 1 "auto dumps, the probe refused" auto dumps --probe "$tmp/pr/onnxruntime.dll"; calls > "$tmp/c"; hasnt "auto dumps refused" "-Do run " "$tmp/c"
reset; echo 1 > "$F/run-rc"
expect 1 "auto dumps restores its pinned probe after failure" auto dumps --probe "$tmp/pr/onnxruntime.dll" --rows 09-flac-16 --rekordbox 7.2.19
calls > "$tmp/c"
for action in install remove; do
  grep -F "\$Mode = '$action'" "$tmp/c" | grep -F 'ResolveOnly = $false' | grep -qF "\$RbVersion = '7.2.19'" &&
    ok || no "auto dumps did not $action the pinned 7.2.19 probe"
done
# --rows: the given rows instead (a single ROW is ROW:ROW), the last run quits
reset; expect 0 "auto dumps --rows" auto dumps --probe "$tmp/pr/onnxruntime.dll" --rows 08-alac-24-96k,w15-mp3-22k,09-flac-16:11-flac-24-96k; a="$(ls -t "$tmp/out"/*-auto-dumps*.log | head -1)"
order="$(sed -n 's/^auto: --- \([^:]*\):.*/\1/p' "$a" | tr '\n' ' ')"
[ "$order" = "cleanup-runners session stage preflight preconditions probe-install dumps 08-alac-24-96k dumps w15-mp3-22k dumps 09-flac-16 probe-fetch probe-remove collect cleanup " ] && ok || no "auto dumps --rows' steps: $order"
has "auto dumps --rows: a single row" "run T2 --keys --from w15-mp3-22k --to w15-mp3-22k --keep-open" "$a"
has "auto dumps --rows: a range" "run T2 --keys --from 09-flac-16 --to 11-flac-24-96k" "$a"
hasnt "auto dumps --rows: the last run quits" "--to 11-flac-24-96k --keep-open" "$a"
hasnt "auto dumps --rows: the default rows" "--from 01-mp3-cbr320" "$a"
expect 2 "auto dumps --rows, a bad name" auto dumps --probe "$tmp/pr/onnxruntime.dll" --rows '08-alac,../x'
expect 2 "auto dumps --rows, an empty item" auto dumps --probe "$tmp/pr/onnxruntime.dll" --rows '08-alac,,09-flac-16'
expect 2 "--rows only for dumps" auto prepare --rows 09-flac-16
expect 2 "run T2 --aac --formats" run T2 --aac --formats
expect 2 "run keycompare --to without --from" run keycompare --to fmt-b
expect 2 "run keycompare --rows and --from" run keycompare --rows fmt-a --from fmt-a
expect 2 "auto dumps --rows and --formats-only" auto dumps --probe "$tmp/pr/onnxruntime.dll" --rows fmt-a --formats-only

mkdir -p "$tmp/kchelper"; echo helper > "$tmp/kchelper/rbsp-prepare.exe"
reset; expect 2 "run T2 --formats redirects to verified plan" run T2 --formats
reset; expect 2 "run hits cannot combine a format batch and a row range" run hits --formats --from fmt-a
reset; expect 0 "run hits --formats verifies and loads the imported batch" run hits --formats --format-batch 2 --rekordbox 7.2.20
calls > "$tmp/c"
has "format hits uses the browser readiness path" "-Do start -Formats " "$tmp/c"
has "format hits verifies through the library" "-Do format-verify -RbVersion '7.2.20'" "$tmp/c"
has "format hits selects batch with library navigation" "-Do format-hits -FormatBatch 2" "$tmp/c"
hasnt "format hits never invokes the ordinary playlist walk" "-Do run -Expect hits" "$tmp/c"
reset; expect 0 "run rowkeys --formats" run rowkeys --formats; calls > "$tmp/c"; has "run rowkeys --formats" "-Do rowkeys -Formats" "$tmp/c"
reset; expect 2 "AAC rowkeys needs one selected format row" run rowkeys --formats --test-unproved-aac
[ ! -s "$F/calls" ] && ok || no "invalid AAC rowkeys contacted the PC"
reset; expect 0 "AAC rowkeys sends the explicit opt-in" run rowkeys --formats --from fmt-aac-lc-22050-2ch-lossy-m4a --test-unproved-aac --rekordbox 7.2.20
calls > "$tmp/c"; has "AAC rowkeys forwards row and version" "-Do rowkeys -Formats -From 'fmt-aac-lc-22050-2ch-lossy-m4a' -TestUnprovedAac -RbVersion '7.2.20'" "$tmp/c"
reset; expect 2 "MP3 rowkeys needs one selected MP3 format row" run rowkeys --formats --from fmt-aac-lc-22050-2ch-lossy-m4a --test-unproved-mp3
has "wrong codec is rejected before forwarding MP3 opt-in" "one MP3 row selected with --from" "$tmp/o"
[ ! -s "$F/calls" ] && ok || no "invalid MP3 rowkeys contacted the PC"
reset; expect 0 "MP3 rowkeys sends the explicit opt-in" run rowkeys --formats --from fmt-mp3-cbr-22050-2ch-lossy-mp3 --test-unproved-mp3 --rekordbox 7.2.20
calls > "$tmp/c"; has "MP3 rowkeys forwards row and version" "-Do rowkeys -Formats -From 'fmt-mp3-cbr-22050-2ch-lossy-mp3' -TestUnprovedMp3 -RbVersion '7.2.20'" "$tmp/c"
reset; expect 0 "run keycompare --rows --helper" run keycompare --rows 01-mp3-cbr320,09-flac-16 --helper "$tmp/kchelper/rbsp-prepare.exe"; calls > "$tmp/c"
has "run keycompare stages helper" "bench/rbsp-prepare.exe" "$(ls -dt "$tmp/out"/stage-* | head -1)/SHA256SUMS"
has "run keycompare calls planned rows" "-Do keycompare -Rows '01-mp3-cbr320,09-flac-16'" "$tmp/c"
has "run keycompare OK" "OK    keycompare: rekordbox's dumped keys match our helper's" "$tmp/o"
reset; echo 3 > "$F/keycompare-rc"; expect 3 "run keycompare exit 3" run keycompare --from fmt-a
has "run keycompare exit 3" "NOTE  keycompare: mismatches or refusals found" "$tmp/o"

helper_dump=(auto dumps --probe "$tmp/pr/onnxruntime.dll" --helper "$tmp/kchelper/rbsp-prepare.exe" --rows 09-flac-16)
reset; expect 0 "auto dumps --helper" "${helper_dump[@]}"; calls > "$tmp/c"
scope="$(grep -F 'ResolveOnly = $false' "$tmp/c" | sed -n "s/.*\$EvidenceRun = '\([a-f0-9]\{32\}\)'; \$Mode = 'install'.*/\1/p")"
[ -n "$scope" ] && ok || no "auto dumps --helper did not reserve fresh evidence"
has "helper comparison uses the probe's run ID" "-Do keycompare -EvidenceRun '$scope'" "$tmp/c"
has "helper probe install forces no dumps" "\$NoDumps = \$true" "$tmp/c"
hasnt "helper fetch excludes mix dumps" 'C:/rbtest/dump' "$F/scp"
reset; touch "$F/probe-already-installed-out"
expect 1 "auto dumps --helper refuses an existing probe after preflight" "${helper_dump[@]}"; calls > "$tmp/c"
hasnt "helper plan cannot compare retained evidence" '-Do keycompare' "$tmp/c"
reset; echo 1 > "$F/keycompare-rc"
expect 1 "auto dumps --helper comparison fails" "${helper_dump[@]}"; calls > "$tmp/c"
has "helper comparison failure restores probe" "\$Mode = 'remove'" "$tmp/c"

reset; expect 2 "format measurement refuses combined staging" auto dumps --formats "$tmp/formats" --format-tier published --formats-only
[ ! -e "$F/scp" ] && ok || no "format measurement staged media"
# --- format import transport fixtures
reset; expect 0 "library check runs in the test session" run library-check --rekordbox 7.2.20
calls > "$tmp/c"
has "library check pins the version" "-Do library-check -RbVersion '7.2.20'" "$tmp/c"
has "library check copies the filtered JSON" "rbst/library-check-0123456789abcdef0123456789abcdef.json" "$F/scp"
hasnt "library check does not start rekordbox" '-Do start' "$tmp/c"
reset; expect 0 "format step forwards an allowlisted action" run format-step click playlists
calls > "$tmp/c"
has "format step pins the version and preserves the action" "-Do format-step -Argument 'click playlists' -RbVersion '7.2.20'" "$tmp/c"
has "format step shows the UI tree" '"Capture":"main"' "$tmp/o"
hasnt "format step leaves the app open" '-Do quit' "$tmp/c"
reset; expect 2 "format step refuses unverified click coordinates" run format-step click 10,20
[ ! -e "$F/calls" ] && ok || no "format step contacted the PC with an invalid action"
reset; expect 0 "format UI dump runs against the open window" run format-uidump --rekordbox 7.2.20
calls > "$tmp/c"
has "format UI dump pins the version" "-Do format-uidump -RbVersion '7.2.20'" "$tmp/c"
has "format UI dump shows the tree" '{"Name":"rekordbox","Depth":0}' "$tmp/o"
hasnt "format UI dump does not start or close rekordbox" '-Do start' "$tmp/c"
hasnt "format UI dump leaves rekordbox open" '-Do quit' "$tmp/c"
reset; expect 0 "window list fetches only its cropped shot" run winlist --rekordbox 7.2.20
calls > "$tmp/c"
has "window list pins the version" "-Do winlist -RbVersion '7.2.20'" "$tmp/c"
has "window list prints the redacted caption" '"Caption":"<redacted>"' "$tmp/o"
has "window list fetches from the kit's shots folder" "rbst/shots/winlist-0123456789abcdef0123456789abcdef-0x123A.png" "$F/scp"
has "window list reports the local crop" "/shots/winlist-0123456789abcdef0123456789abcdef-0x123A.png" "$tmp/o"
if grep -Eq -- '-Do (start|quit) ' "$tmp/c"; then no "window list started or closed rekordbox"; else ok; fi
reset
printf '%s\n' 'WINLIST SHOT D:\other\rbst\shots\winlist-0123456789abcdef0123456789abcdef-0x123A.png' > "$F/winlist-out"
expect 2 "window list refuses a crop path on another drive" run winlist
[ ! -e "$F/scp" ] && ok || no "window list copied an out-of-scope path"
reset
if "$S" run format-uidump --explore --rekordbox 7.2.20 > "$tmp/o" 2>&1 &&
   calls | grep -Fq -- "-Do format-uidump -Explore -RbVersion '7.2.20'"; then ok
else no "format UI dump forwards --explore to the desktop runner"; fi
reset; touch "$F/version-warning"
expect 0 "format plan pins the version despite SSH diagnostics and imports before measurement" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"
if [ "$(grep -c -- '-Do start -Formats ' "$tmp/c")" = 2 ]; then ok
else no "format import and probe measurement must both use the browser readiness path"; fi
awk '
  /-Do start / { if (!started) started = NR }
  /-Do format-import / { imported = NR }
  /-Do quit / { if (imported && !closed) closed = NR }
  /-Do format-verify / { verified = NR }
  /-Do format-plan / { planned = NR }
  /\$Mode = .install./ { installed = NR }
  END { exit (!(started && started < imported && imported < closed && closed < verified && verified < planned && planned < installed)) }
' "$tmp/c" && ok || no "format plan must start, import, close, and verify before installing the probe"
reset; printf 'REKORDBOX 7.2.20\nREKORDBOX 7.2.19\n' > "$F/version-out"
expect 1 "format plan rejects ambiguous versions" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"
hasnt "ambiguous version stops before preflight" '-Do ensure' "$tmp/c"
reset; printf 'format-import: STOP: unexpected rekordbox window\n' > "$F/format-import-out"; echo 1 > "$F/format-import-rc"
expect 1 "a requested fresh import stops the plan on a dialog" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --fresh-format-import
calls > "$tmp/c"
if grep -Eq -- '-Do (quit|format-plan) |\$Mode = .install.' "$tmp/c"; then
  no "format import STOP closed rekordbox or advanced the plan"
else
  ok
fi
# --- end format import transport fixtures

reset; echo 0 > "$F/format-reuse-rc"
expect 0 "format measure reuses a matching library playlist by default" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"
has "default path searches the library" "-Do format-reuse " "$tmp/c"
hasnt "matching playlist skips fresh import" "-Do format-import " "$tmp/c"
reset; echo 0 > "$F/format-reuse-rc"
expect 0 "explicit fresh import bypasses the reuse search" run format-import --fresh-format-import
calls > "$tmp/c"
hasnt "fresh import skips reuse" "-Do format-reuse" "$tmp/c"
has "fresh import submits the playlist" "-Do format-import" "$tmp/c"
reset; echo 1 > "$F/format-reuse-rc"
expect 1 "a reuse lookup failure stops the import" run format-import
calls > "$tmp/c"
hasnt "failed lookup does not import" "-Do format-import" "$tmp/c"
reset; expect 2 "fresh and explicit reuse conflict" auto dumps --formats-only --fresh-format-import --reuse-format-import
reset; expect 2 "fresh and resume conflict" run format-import --fresh-format-import --resume-import

reset; expect 0 "format import verifies the recorded submission" run format-import --resume-import --rekordbox 7.2.20
calls > "$tmp/c"
has "format resume verifies through the library" "-Do format-verify" "$tmp/c"
hasnt "format resume does not submit a playlist" "-Do format-import" "$tmp/c"
hasnt "format resume does not open rekordbox" "-Do start" "$tmp/c"

reset; printf 'format-plan: STOP: verify the imported playlist through the library first\n' > "$F/format-plan-out"; echo 1 > "$F/format-plan-rc"
expect 1 "format plan refuses a playlist mismatch" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"; hasnt "playlist check happens before probe installation" "\$Mode = 'install'" "$tmp/c"
has "playlist check shows how to verify the import" "verify the imported playlist through the library first" "$tmp/o"

reset; expect 0 "format batch 2" env RBKIT_TIMING=1 auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --format-batch 2 --confirm-import-refusals
calls > "$tmp/c"
has "format batch opts in to Windows timings" 'env:RBKIT_TIMING' "$tmp/c"
timing="$(ls -t "$tmp/out/"*-auto-dumps*.log.timing | head -1)"
has "format batch times the controller plan" 'TIMING mac auto-dumps start_ms=' "$timing"
awk '/^TIMING mac windows-keycompare / { compared = 1; next }
  compared { timed = ($0 ~ /^TIMING mac scp /); exit }
  END { exit !timed }' "$timing" && ok || no "format batch did not time the report transfer after comparison"
has "format batch counts SSH calls" 'TIMING mac ssh start_ms=' "$timing"
has "format batch forwards resumption index" "-Do format-plan -FormatBatch 2" "$tmp/c"
has "no matching playlist falls back to an import" "-Do format-import " "$tmp/c"
has "format batch forwards explicit import refusal confirmation" "-ConfirmImportRefusals" "$tmp/c"
grep -E -- '-Do format-run .* -Rows .fmt-e,fmt-f.' "$tmp/c" > /dev/null && ok || no "format batch measured different rows"
grep -E -- '-Do keycompare .* -Rows .fmt-e,fmt-f.' "$tmp/c" > /dev/null && ok || no "format batch compared different rows"
has "format batch records import refusal" "fmt-e IMPORT_REFUSED" "$tmp/o"
has "format batch continues after an unconfirmed load" "fmt-f LOAD_UNCONFIRMED" "$tmp/o"
has "format batch restores probe" "\$Mode = 'remove'" "$tmp/c"
has "format batch fetches a per-run report" "results in" "$tmp/o"
format_report="$(sed -n 's/^.*results in //p' "$tmp/o")"
[ -n "$format_report" ] && [ -f "${format_report%/*}/chunk-hashes.tsv" ] &&
  ok || no "format batch did not fetch the run-scoped content hash lists"
[ -n "$format_report" ] && grep -qxF 'filtered helper stderr' "${format_report%/*}/helper-stderr.log" &&
  ok || no "format batch did not fetch the filtered helper diagnostic log"
hasnt "format fetch excludes the raw probe log" 'C:/rbtest/probe.log' "$F/scp"
hasnt "format fetch excludes the raw chunk hashes" 'C:/rbtest/rows.txt' "$F/scp"

reset; touch "$F/keycompare-mixed"
expect 0 "format batch fetches a report after a partial CLIXML line" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
has "format batch fetches the mixed-line report" 'results in' "$tmp/o"

reset; expect 0 "format batch reuses an automated export with confirmed refusals" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --reuse-format-import --confirm-import-refusals
calls > "$tmp/c"
has "reuse verifies the automated import" "-Do format-verify " "$tmp/c"
hasnt "reuse skips the import task" "-Do format-import" "$tmp/c"

reset; expect 2 "AAC proof requires a format batch" auto dumps --test-unproved-aac
reset; expect 0 "AAC proof opts in only during comparison" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --test-unproved-aac
calls > "$tmp/c"
grep -E -- '-Do keycompare .* -TestUnprovedAac ' "$tmp/c" > /dev/null && ok || no "AAC proof opt-in missing from key comparison"
grep -E -- '-Do (format-plan|format-run) .* -TestUnprovedAac ' "$tmp/c" > /dev/null &&
  no "AAC proof opt-in reached measurement" || ok

reset; expect 2 "MP3 proof requires a format batch" auto dumps --test-unproved-mp3
has "MP3 proof requires formats-only" "format import and batch options require --formats-only" "$tmp/o"
reset; expect 0 "MP3 proof opts in only during comparison" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --test-unproved-mp3
calls > "$tmp/c"
grep -E -- '-Do keycompare .* -TestUnprovedMp3 ' "$tmp/c" > /dev/null && ok || no "MP3 proof opt-in missing from key comparison"
grep -E -- '-Do (format-plan|format-run) .* -TestUnprovedMp3 ' "$tmp/c" > /dev/null &&
  no "MP3 proof opt-in reached measurement" || ok

reset; echo 1 > "$F/format-run-rc"
expect 1 "format measurement fails" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"; has "format measurement failure restores probe" "\$Mode = 'remove'" "$tmp/c"
reset; echo 1 > "$F/keycompare-rc"
expect 1 "format comparison fails" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only
calls > "$tmp/c"
awk '/\$Mode = .remove./ { restored = 1 } /-Do keycompare / { compared = 1; if (!restored) bad = 1 } END { exit (!compared || bad) }' "$tmp/c" &&
  ok || no "format batch compared while the probe was still installed"

# --- format pass transport
reset; echo 0 > "$F/format-reuse-rc"
expect 0 "probe range shares one session" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --format-batch 1:4
calls > "$tmp/c"
awk '
  /ResolveOnly = \$true/ { next }
  /-Do format-plan / { ++plans; if (installed) bad = 1 }
  /\$Mode = .install./ { ++installed }
  /-Do start / { ++starts }
  /-Do format-run / { ++runs; if (starts != 1 || installed != 1 || removed) bad = 1 }
  /\$Mode = .remove./ { ++removed }
  /-Do keycompare / { ++reports; if (removed != 1) bad = 1 }
  END { exit (bad || plans != 4 || runs != 4 || reports != 4 || starts != 1 || installed != 1 || removed != 1) }
' "$tmp/c" && ok || no "probe range must plan four batches, use one session and restore before comparison"
if [ "$(sed -n 's/^.*results in \(.*\)$/\1/p' "$tmp/o" | sort -u | wc -l | tr -d ' ')" = 4 ]; then ok
else no "probe range did not write four separate reports"; fi

reset; echo 0 > "$F/format-reuse-rc"
printf '0 formats: batch measured\n0 formats: STOP: unexpected window\n' > "$F/format-run-seq"
expect 1 "probe range stops in its second batch" auto dumps --probe "$tmp/pr/onnxruntime.dll" --formats-only --format-batch 1:4
calls > "$tmp/c"
awk '/ResolveOnly = \$true/ { next } /-Do format-run / { ++runs } /\$Mode = .remove./ { ++removed } /-Do keycompare / { ++reports }
     END { exit (runs != 2 || removed != 1 || reports) }' "$tmp/c" &&
  ok || no "probe range did not restore and stop on the second batch"

reset
printf '3 formats: cache expectation unmet\n' > "$F/format-hits-seq"
expect 3 "cache range records an unmet expectation and checks all four batches" run hits --formats --format-batch 1:4 --rekordbox 7.2.20
calls > "$tmp/c"
awk '
  /-Do format-verify / { ++verified }
  /-Do start / { ++starts }
  /-Do quit / { ++closes }
  /-Do format-hits / { ++runs; if (verified != 1 || starts != 1 || closes != 1 || $0 !~ ("-FormatBatch " runs " ")) bad = 1 }
  END { exit (bad || runs != 4 || closes != 2) }
' "$tmp/c" && ok || no "cache range must verify, start once, check four batches, and close"
if [ "$(sed -n 's/^.*cache evidence in \(.*\)$/\1/p' "$tmp/o" | sort -u | wc -l | tr -d ' ')" = 4 ]; then ok
else no "cache range did not write four separate reports"; fi

# BSR6
reset
printf '0 formats: batch measured\n0 formats: STOP: unexpected window\n' > "$F/format-hits-seq"
expect 1 "cache range stops in its second batch" env RBKIT_TIMING=1 RBKIT_OUT="$tmp/cache-stop" \
  run hits --formats --format-batch 1:4 --rekordbox 7.2.20
calls > "$tmp/c"
awk '/-Do format-hits / { ++runs } /-Do quit / { ++closes } END { exit (runs != 2 || closes != 1) }' "$tmp/c" &&
  ok || no "cache range tried to close rekordbox after the second batch STOP"
grep -qE '^TIMING mac run-hits .* exit=1$' "$tmp/cache-stop/"*.timing &&
  ok || no "cache range did not finish timing after the second batch STOP"

if command -v pwsh > /dev/null; then
  reset
  mkdir -p "$F/quit-sim/rbst"
  printf '%s\n' 'rekordbox | rekordbox | 0x10' > "$F/quit-sim/rbst/watch-base.txt"
  printf '%s\n' 'private format quit caption' > "$F/quit-sim/dialog"
  printf '%s\n' "$k" > "$F/format-quit-sim"
  expect 1 "cache range stops on a new quit window" env RBKIT_OUT="$tmp/cache-quit" \
    run hits --formats --format-batch 1:2 --rekordbox 7.2.20
  calls > "$tmp/c"
  has "cache range requests format quit" "-Do quit -Formats " "$tmp/c"
  grep -qF 'quit: STOP: 1 window(s); handles: 0x40; new windows' "$tmp/cache-quit/"*.log &&
    ! grep -qF 'private format quit caption' "$tmp/cache-quit/"*.log &&
    ok || { no "format quit did not redact the private caption in the controller log"; sed 's/^/    /' "$tmp/cache-quit/"*.log; }
  rm -f "$F/format-quit-sim"
fi
# End BSR6
# --- end format pass transport

# A failing dumps step already notifies. The phone gets the reason and the local log keeps the paths.
reset
shot='C:\Users\rbtest\AppData\Local\Temp\rbst\shots\auto.png'
printf '10:00:00  run: STOP: new window(s): rekordbox | Sign in | 0x40; 10:00:00  shot %s\n' "$shot" > "$F/run-out"
echo 1 > "$F/run-rc"
expect 1 "auto dumps, T2 STOP" env RBKIT_OUT="$tmp/Users/logs" auto dumps --probe "$tmp/pr/onnxruntime.dll"
[ "$(grep -c . "$F/notified")" = 1 ] && ok || no "auto dumps STOP: expected exactly one notification"
grep -qxF 'run T2: new window(s): rekordbox | Sign in | 0x40' "$F/notified" && ok || no "auto dumps STOP: expected just the command and reason"
hasnt "auto dumps alert leaks local log path" '/Users/' "$F/notified"
hasnt "auto dumps alert leaks screenshot path" ':\Users' "$F/notified"
grep -F '(notified:' "$tmp/o" | grep -F -- "$shot" | grep -qF -- "$tmp/Users/logs/" && ok || no "auto dumps STOP: notification log lost the paths"

reset; printf '10:00:00  run: STOP: unexpected dialog %0180d\n' 0 > "$F/run-out"; echo 1 > "$F/run-rc"
expect 1 "run T2, long STOP reason" run T2
alert="$(cat "$F/notified")"
[ "${#alert}" -le 160 ] && ok || no "run T2 STOP alert exceeds 160 characters"

reset; printf '10:00:00  t2-facts: STOP: desktop facts unavailable\n' > "$F/t2-facts-out"; echo 1 > "$F/t2-facts-rc"
expect 1 "auto session2, a STOP without a step notification" auto session2
[ "$(cat "$F/notified")" = 'auto session2: desktop facts unavailable' ] && ok || no "auto fallback: expected one alert with the step's STOP reason"

reset; echo 'RB-KIT: no output for 600 s' > "$F/quit-out"; echo 142 > "$F/quit-rc"
expect 1 "run T2, no output before fresh start" run T2 --keys --fresh
[ "$(grep -c . "$F/notified")" = 1 ] && ok || no "fresh-start STOP: expected exactly one notification"

# --- nothing personal tracked: no IPv4 address but loopback, not the git identity, no private file's name
tr="$repo/tests/rekordbox"
ips="$(grep -rEoh '\b([0-9]{1,3}\.){3}[0-9]{1,3}\b' "$tr" | grep -vE '^(127\.0\.0\.1|0\.0\.0\.0)$' | sort -u)"
[ -z "$ips" ] && ok || no "addresses in tests/rekordbox: $ips"
for k in user.name user.email; do
  v="$(git -C "$repo" config --get "$k" || true)"
  if [ -n "$v" ] && grep -rqiF -- "$v" "$tr"; then no "tests/rekordbox holds the git $k"; else ok; fi
done
# the local settings' values, and their user, host and disk-name parts (only the key is printed)
cfg_has() {
  [ -n "$1" ] && [ "$1" != rbtest ] || return 1
  if [ "${#1}" -le 3 ]; then grep -rqiFw -- "$1" "$2"; else grep -rqiF -- "$1" "$2"; fi
}
printf '%s\n' 'x-ab! and x-cde! plus qq' > "$tmp/cfg-scan"
cfg_has 'ab' "$tmp/cfg-scan" && ok || no "config scan: missed a two-character word"
cfg_has 'cde' "$tmp/cfg-scan" && ok || no "config scan: missed a three-character word"
cfg_has 'and' "$tmp/cfg-scan" && ok || no "config scan: missed a complete word"
! cfg_has 'plu' "$tmp/cfg-scan" && ok || no "config scan: matched a short prefix"
! cfg_has 'q' "$tmp/cfg-scan" && ok || no "config scan: matched inside a word"
cfg_has 'lus q' "$tmp/cfg-scan" && ok || no "config scan: missed a longer literal substring"
git -C "$repo" config --name-only --get-regexp '^rbstemsplus\.' > "$tmp/config-keys" || true
while IFS= read -r k; do
  v="$(git -C "$repo" config --get "$k" || true)"; [ -n "$v" ] || continue
  parts=("$v"); case "$v" in *@*) parts+=("${v%@*}" "${v#*@}") ;; esac; case "$k" in *offlinedisk) parts+=("${v#*:}") ;; esac
  hit=0; for p in "${parts[@]}"; do if cfg_has "$p" "$tr"; then hit=1; fi; done
  [ "$hit" = 0 ] && ok || no "tests/rekordbox holds a configured value for $k"
done < "$tmp/config-keys"
if [ -f "$repo/.publicignore" ]; then
  while IFS= read -r p; do
    case "$p" in '' | '#'*) continue ;; esac
    # a private folder by its path (as release.sh checks folders), a private file by its name and stem
    p="${p%/}"; b="$(basename "$p")"; s="${b%.*}"
    if [ -d "$repo/$p" ]; then names=(-e "$p"); else names=(-e "$b" -e "$s"); fi
    if grep -rqwF "${names[@]}" "$tr"; then no "tests/rekordbox names the private $p (release.sh would refuse)"; else ok; fi
  done < "$repo/.publicignore"
fi

# no kit script ever went to ssh on stdin (the PC's OpenSSH could leave piped stdin undelivered)
[ ! -s "$F/stdin-used" ] && ok || no "ssh got a script on stdin: $(head -3 "$F/stdin-used")"

echo "pc-session-test: $pass passed, $fail failed"
[ "$fail" = 0 ]
