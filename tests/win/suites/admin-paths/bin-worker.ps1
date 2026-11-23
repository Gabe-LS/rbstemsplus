# AF12 / AR4, the other half: with the app folder's admin\ worker gone, the verbs that need no
# source run with the bin\ worker. By the step's name, after an arm-details* step (UAC answered
# Yes after "Show more details", so the prompt names the program's location):
#   restore-bin    R1's Put Back Pioneer's File (foreign: a foreign live DLL), its confirmation's
#                  Put Back Pioneer's File: the prompt's program is Stems Cache's bin\ worker,
#                  nothing from the apps folder; rekordbox's onnxruntime.dll Pioneer's again
#   uninstall-bin  Uninstall Stems Cache > Uninstall: the same prompt check; Stems Cache off, its
#                  root gone (or only boot-time deletes left), Pioneer's DLL in place
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$s = $VmRun.Step
# the first enabled button named button inside the Window element named dialog of process pid
# (its descendants searched, the main window's own buttons never), invoked; whether it was
function Invoke-InDialog([int]$procId, [string]$dialog, [string]$button, [int]$secs) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [Windows.Automation.AutomationElement]
    $win = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $procId)),
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Window)),
        (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $dialog)))
    $btn = New-Object Windows.Automation.AndCondition(
        (New-Object Windows.Automation.PropertyCondition($A::ControlTypeProperty, [Windows.Automation.ControlType]::Button)),
        (New-Object Windows.Automation.PropertyCondition($A::NameProperty, $button)))
    return Wait-Until {
        $d = $A::RootElement.FindFirst([Windows.Automation.TreeScope]::Descendants, $win)
        $b = if ($d) { $d.FindFirst([Windows.Automation.TreeScope]::Descendants, $btn) } else { $null }
        if (-not $b -or -not $b.Current.IsEnabled) { return $false }
        $b.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
        $true
    } $secs
}
$from = Get-AppLogSize
Open-App
if (-not $script:app) { Done }
$script:Pressed = $false
if ($s -eq 'restore-bin') {
    $line = Get-CoreText 'T_WIN_R1_LIVE_FOREIGN'
    Check (Find-UiText $script:app.Id ('^' + [regex]::Escape($line) + '$') 60) "R1's line: $line"
    $put = Get-CoreText 'T_WIN_BTN_RESTORE'
    Check (Wait-UiEnabled $script:app.Id $put 60) "$put shows, enabled"
    Check (Invoke-UiNamed $script:app.Id $put 30 'Button') "$put pressed"
    # its confirmation's button has the same words as the window's: pressed inside the dialog's
    # own Window element (named by its title; UI Automation lists an owned dialog under the main
    # window, so rbui's top-level --title doesn't reach it)
    $q = Get-CoreText 'T_WIN_RESTORE_Q'
    $script:Pressed = Invoke-InDialog $script:app.Id $q $put 60
    Check $script:Pressed "its confirmation ($q) answered: $put"
} elseif ($s -eq 'uninstall-bin') {
    Invoke-AppButton 'T_BTN_UNINSTALL_CACHE' 'T_UNINSTALL'
} else { Check $false "no case for a step named $s" }
if (-not $script:Pressed) { Show-AppState; Stop-App; Done }
Check (Wait-Until { Test-ConsentShown } 120) "UAC's prompt shows (consent.exe)"
Test-ConsentResult yes
$prompt = $script:ConsentResult.Prompt
Check ($prompt -match [regex]::Escape("$CacheRoot\bin\")) "the prompt's program is the bin\ worker in $CacheRoot ($prompt)"
Check ($prompt -notmatch [regex]::Escape($l.Apps)) 'and nothing from the apps folder'
if ($s -eq 'restore-bin') { Check (Wait-Until { (Read-AppLog $from) -match 'restore: (done|not done)' } 240) 'the action ended (app.log: restore: ...)' }
else { Check (Wait-Until { -not (Test-CacheOn) -and @(Get-WorkerProcesses).Count -eq 0 } 240) 'the action ended (Stems Cache off, no worker)' }
$said = Read-AppLog $from
"the app's log meanwhile:"
foreach ($x in ($said -split "`r?`n")) { if ($x) { "  $x" } }
Check ((Get-Sha $RbOrt) -eq $f.PioneerSha) "rekordbox's onnxruntime.dll is Pioneer's"
if ($s -eq 'restore-bin') {
    Check ($said -match 'restore: done') 'app.log: restore: done'
} else {
    Check (-not (Test-CacheOn)) 'Stems Cache is off'
    $gone = Wait-Until { -not (Test-Path -LiteralPath $CacheRoot) } 60
    $pending = (-not $gone) -and (Test-PendingOnly $CacheRoot)
    Check ($gone -or $pending) "Stems Cache's root is gone, or holds only boot-time deletes ($CacheRoot)"
}
Check (Wait-Until { @(Get-WorkerProcesses).Count -eq 0 } 60) 'no worker left running'
Stop-App
Done
