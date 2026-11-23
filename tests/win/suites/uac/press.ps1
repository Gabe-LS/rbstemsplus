# The sign-out cases' first half (app.md 5.9 WC1-WC10), as the user: the app open, one Stems
# Cache action pressed, by this step's name, and its UAC prompt shown; then the step ends, so the
# sign-out (arm-wc*'s rbui: Yes 15 s after the prompt, the sign-out once the worker runs) finds no
# step of ours in the session:
#   wc1*   Uninstall Stems Cache > Uninstall (the bin\ worker): meanwhile, W5 A7.1: the chain the
#          core holds during runas (bin\<sha12>\ and bin\ opened for reading, sharing READ only)
#   wc2*   Uninstall Stems Cache > Uninstall
#   wc3*   Install Stems Cache > Install (or Reinstall Stems Cache > Reinstall)
#   v8     Install Stems Cache > Install, holding the swap of rekordbox 7.2.20
#   wc10*  Uninstall RB Stems Plus Completely > Uninstall: UAC first, while the apps folder is
#          still there
# The app's pid and its log's size are left for the step after the sign-out (C:\rbtest\uac\).
$l = Get-Layout
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
$s = $VmRun.Step
$case = ($s -split '-')[0]
Save-UacValue "$case.log" (Get-AppLogSize)
# the worker is held (a TEST worker only), so the sign-out reaches the app while it runs and the
# block screen shows: unheld, it ends before the sign-out does (VM round 4). WC1 and WC2 hold it
# before its work (it finishes once the sign-out waits, or after Cancel); WC3 and WC10, whose
# "Sign out anyway" ends the session a few seconds in, hold it mid-work, so the session ends
# after its work began changing files (a hold before the work: it changed nothing, VM round 5)
if ($case -in 'wc3', 'wc10', 'v8') { Set-WorkerHold $WorkerHoldSecs -Mid } else { Set-WorkerHold $WorkerHoldSecs }
if ($case -eq 'v8') {
    Set-ItemProperty -LiteralPath 'HKCU:\Environment' -Name 'RBSP_TEST_WORKER_MIDHOLD_TARGET' -Value '7.2.20' -Type String
}
# WC10's model case: rekordbox's stems model at the press, for wc10-after (Pioneer's own: Uninstall
# All has no model of ours to put back)
if ($case -eq 'wc10') { Save-UacValue 'wc10.model' (Get-Sha (Join-Path $RbModelDir 'hdemucs.onnx')) }
# Start a fresh copy inheriting the TEST shell's trace path for this sign-out only.
Stop-App
$env:RBSP_TEST_SESSION_TRACE = Join-Path "$env:LOCALAPPDATA" "rbtest-$($VmRun.Run)-$case.session"
Remove-Item -LiteralPath $env:RBSP_TEST_SESSION_TRACE -Force -ErrorAction SilentlyContinue
Save-UacValue "$case.trace" $env:RBSP_TEST_SESSION_TRACE
try { Open-App } finally { Remove-Item Env:RBSP_TEST_SESSION_TRACE }
if (-not $script:app) { Done }
Save-UacValue "$case.pid" $script:app.Id
$script:Pressed = $false
switch -Regex ($case) {
    '^wc(1|2)$' { Invoke-AppButton 'T_BTN_UNINSTALL_CACHE' 'T_UNINSTALL' }
    '^wc3$' { Invoke-CacheInstall }
    '^v8$' { Invoke-AppButton 'T_BTN_INSTALL_CACHE' 'T_INSTALL' }
    '^wc10$' { Invoke-UninstallAll }
    default { Check $false "no case for a step named $s" }
}
if (-not $script:Pressed) { Done }
$shown = Wait-Until { Test-ConsentShown } 120
Check $shown "UAC's prompt shows (consent.exe)"
if ($case -eq 'wc10') { Check (Test-Path -LiteralPath $l.Apps) 'UAC first: the apps folder is still there while the prompt shows' }
if ($case -eq 'wc1' -and $shown) {
    $p = Read-TestPayload
    $sha12 = ([string]$p.windows.app.admin_sha256).ToLowerInvariant().Substring(0, 12)
    $exe = Join-Path $CacheRoot "bin\$sha12\RB Stems Plus.exe"
    if (-not (Test-Path -LiteralPath $exe)) { Missing "W5 A7.1: no bin\ worker ($exe): this elevation isn't one of the bin\ worker's" }
    else {
        foreach ($d in @((Join-Path $CacheRoot "bin\$sha12"), (Join-Path $CacheRoot 'bin'))) {
            $e = Test-HeldForRead $d
            if ($e -eq 5) { Missing "W5 A7.1: $d can't be opened by the user at all (access denied): the share check needs read access" }
            else { Check ($e -eq 32) "W5 A7.1: the core holds $d during runas (an open that doesn't share reading: error $e, 32 = a sharing violation)" }
        }
    }
}
"the step ends: rbui answers the prompt about 15 s after it showed, then the sign-out comes once the worker runs"
Done
