# Before wc10-ready, as the user: WC3's forced sign-out usually cuts its worker after the swap and
# before the receipt (wc3-after). Under the multi-version rules that row reads "Needs repair" (not
# recorded: win/c/inventory.c row_status), and Install and Reinstall have no target there
# (target_row: a bridge without its receipt; the core logs "Stems Cache: no eligible changes" and
# does nothing), so Put Back Pioneer's File comes first, as multiversion.suite's V8 does: one UAC
# prompt, answered Yes by arm-yes-3's rbui, then rekordbox's own onnxruntime.dll back. When WC3's
# worker finished instead (Stems Cache on, recorded), there is nothing to repair: said, and
# arm-yes-4 replaces the unused responder
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (an earlier step failed: its log)' }
if (@(Get-Rekordbox).Count) { Check (Close-Rekordbox 200) 'rekordbox closed first' }
Open-App
if (-not $script:app) { Done }
# one version installed: the compact layout has no rows, so the start-up's state line says it
$state = $null
[void](Wait-Until { $script:stateLine = @((Read-AppLog) -split "`r?`n" | Where-Object { $_ -match '\bstate: rekordbox ' }) | Select-Object -Last 1; [bool]$script:stateLine } 60)
$state = [string]$script:stateLine
if ($state -notmatch 'state: rekordbox 7\.2\.19 Needs repair') {
    "rekordbox 7.2.19 doesn't need repair ('$($state.Trim())': WC3's worker finished): nothing to put back"
    Done
}
"rekordbox 7.2.19 needs repair ('$($state.Trim())': WC3's worker cut before its receipt)"
Add-Type -Namespace RbspWc10 -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
# the button named $button in a top-level window of the app that also shows a name matching
# $textRegex (the confirmation, not the main window's button of the same name), invoked
function Invoke-DialogButton([string]$textRegex, [string]$button, [int]$secs) {
    $A = [Windows.Automation.AutomationElement]
    return Wait-Until {
        $cond = New-Object Windows.Automation.PropertyCondition($A::ProcessIdProperty, $script:app.Id)
        foreach ($w in $A::RootElement.FindAll([Windows.Automation.TreeScope]::Children, $cond)) {
            $hit = $false; $btn = $null
            foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
                if ($e.Current.Name -match $textRegex) { $hit = $true }
                if (-not $btn -and $e.Current.Name -eq $button -and ($e.Current.ControlType -eq [Windows.Automation.ControlType]::Button -or $e.Current.AutomationId -like 'CommandButton_*')) { $btn = $e }
            }
            if ($hit -and $btn -and $btn.Current.AutomationId -match '^CommandButton_(\d+)$') {
                # a task dialog's command button: TDM_CLICK_BUTTON with its id
                [void][RbspWc10.U]::PostMessage([IntPtr]$w.Current.NativeWindowHandle, 0x466, [IntPtr][int]$Matches[1], [IntPtr]::Zero)
                return $true
            }
            if ($hit -and $btn) {
                $pat = $null
                if ($btn.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) { $pat.Invoke(); return $true }
            }
        }
        $false
    } $secs
}
$label = Get-CoreText 'T_WIN_BTN_RESTORE'
$from = Get-AppLogSize
$ok = Wait-UiEnabled $script:app.Id $label 60
Check $ok "its $label button is there and enabled"
if (-not $ok) { Show-AppState; Done }
Check (Invoke-UiNamed $script:app.Id $label 30 'Button') "$label pressed"
Check (Invoke-DialogButton ('^' + [regex]::Escape((Get-CoreText 'T_WIN_RESTORE_Q')) + '$') $label 60) "its confirmation answered: $label"
$f = Read-WorkerFacts
Check (Wait-Until { (Get-Sha $RbOrt) -eq $f.PioneerSha } 240) "rekordbox's onnxruntime.dll is Pioneer's again ($($f.PioneerSha))"
$log = Read-AppLog $from
Check ($log -match 'admin run \(') 'Put Back ran elevated (an "admin run (" line since the click)'
Test-ConsentResult yes
Done
