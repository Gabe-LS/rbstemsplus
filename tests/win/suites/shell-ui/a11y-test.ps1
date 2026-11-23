param([string]$Library = (Join-Path $PSScriptRoot 'a11y-lib.ps1'))
$ErrorActionPreference = 'Stop'
. $Library

$aumid = 'io.github.rbstemsplus.app'
if (-not (Test-NoticeRecord @('control') @('control', $aumid) $aumid) -or
    (Test-NoticeRecord @($aumid.ToUpperInvariant()) @($aumid) $aumid) -or
    (Test-NoticeRecord @() @("$aumid.unrelated") $aumid)) {
    throw 'Q13 ownership must require an exact, new record (registry comparison is case insensitive)'
}
'PASS Q13 ownership'
$tree = @'
  Text "off" #PlusLight class=TextBlock pid=123 help="Demucs v4 is off."
  Text "off" #CacheLight class=TextBlock pid=123 help="Stems Cache is off."
  Button "Install Stems Cache" #InstallCache pid=123
'@
if (-not (Test-A11yNode $tree 'Text' 'off' 'Demucs v4 is off.') -or
    (Test-A11yNode $tree 'Button' 'off' 'Demucs v4 is off.') -or
    (Test-A11yNode $tree 'Text' 'off' 'Demucs v4 is on.') -or
    (Test-A11yNode $tree 'Text' 'off' 'Demucs v4 is off')) {
    throw 'NAR must match the control type, name and complete help on the same node'
}
'PASS NAR UIA node'

# WPF cannot run on the host. Check the status live region's wiring and leave event delivery
# to the VM assertion in a11y.ps1.
$shell = Join-Path $PSScriptRoot '../../../../win/shell'
[xml]$window = Get-Content -Raw (Join-Path $shell 'MainWindow.xaml')
$ns = @{ x = 'http://schemas.microsoft.com/winfx/2006/xaml' }
$status = (Select-Xml -Xml $window -Namespace $ns -XPath '//*[@x:Name="StatusLine"]').Node
$windowCode = Get-Content -Raw (Join-Path $shell 'MainWindow.xaml.cs')
$statusUpdate = [regex]::Match($windowCode, '(?s)private void UpdateStatusLine\(\)\s*\{(.*?)\r?\n {8}\}').Groups[1].Value
if (-not $status -or $status.HasAttribute('Text') -or
    $windowCode -notmatch 'SetLiveSetting\(StatusLine, AutomationLiveSetting\.Polite\)' -or
    $windowCode -notmatch 'case nameof\(ManageModel\.StatusLine\):\s*UpdateStatusLine\(\);' -or
    $statusUpdate -notmatch '(?s)string text = Model\.StatusLine;.*StatusLine\.Text = text;.*CreatePeerForElement\(StatusLine\)\?\.RaiseAutomationEvent\(AutomationEvents\.LiveRegionChanged\)' -or
    $statusUpdate -notmatch '(?s)reportStatusTimer\?\.IsEnabled == true.*StatusLine\.Text = text;.*if \(text == Texts\.Get\(CoreText\.T_CREATING_REPORT\)\).*reportStatusTimer\.Stop\(\);\s*UpdateStatusLine\(\);.*reportStatusTimer\.Start\(\);') {
    throw 'The polite status live region must publish the report text before raising its event and hold it through a fast report'
}
'PASS report status live region'

# Model Windows PowerShell's process object when no handle was retained before exit.
$Rbui = $PSCommandPath
function Start-Process {
    $process = [pscustomobject]@{ Retained = $false }
    Add-Member -InputObject $process -MemberType ScriptProperty -Name Handle -Value {
        $this.Retained = $true
        return [IntPtr]1
    }
    Add-Member -InputObject $process -MemberType ScriptProperty -Name ExitCode -Value {
        if ($this.Retained) { return 0 }
    }
    return $process
}
$capture = Start-RbuiCapture $PSScriptRoot notice 'notice test'
if ($capture.Process.ExitCode -ne 0) { throw 'rbui capture lost its exit code after the child exited' }
'PASS rbui capture retains the process handle'

$control = 'rbsp.q13.control.host-test'
$state = @{ Records = @($control, $aumid); CleanupCalls = 0 }
function Get-NoticeRecords { $state.Records }
function Check([bool]$ok, [string]$what) {
    if ($ok) { "PASS $what" } else { "FAIL $what" }
}
function Invoke-Exe([string]$exe, [string[]]$argv) {
    if ($exe -ne 'wintest' -or $argv.Count -ne 1 -or $argv[0] -ne '--balloon-cleanup') {
        throw 'Q13 must call the native cleanup fixture'
    }
    $state.CleanupCalls++
    $state.Records = @($state.Records | Where-Object { $_ -ne $aumid })
    return 0
}
function Get-ItemProperty([string]$LiteralPath) {
    if ($LiteralPath -eq $control -and $state.Records -contains $control) { @{ Q13Control = 'intact' } }
}
$sender = [pscustomobject]@{ HasExited = $false }
$process = [pscustomobject]@{ ExitCode = 3; Finished = $true }
Add-Member -InputObject $process -MemberType ScriptMethod -Name WaitForExit -Value { param($milliseconds) $this.Finished }
$capture = [pscustomobject]@{ Process = $process }
foreach ($finished in $true, $false) {
    $process.Finished = $finished
    $state.Records = @($control, $aumid)
    $state.CleanupCalls = 0
    $log = @(Complete-NoticeCheck $capture $sender settings $control @() $aumid wintest)
    if ($state.CleanupCalls -ne 1 -or $log -notcontains "PASS the notification created the exact $aumid record" -or
        $log -notcontains 'PASS the exact notification record is gone' -or
        $log -notcontains 'PASS the unrelated control record stayed intact' -or
        $log -notcontains 'FAIL the banner and tray icon appeared and disappeared while the sender stayed alive') {
        throw "Q13 cleanup must run on a proven record even when the observer fails (finished=$finished)"
    }
}
'PASS Q13 cleanup runs after observer failure or timeout'
