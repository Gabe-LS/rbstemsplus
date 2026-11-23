# app.md section 7: win/c's Windows test exes, unelevated (the "wintests" step) and elevated (the
# "wintests-elevated" step, with --elevated): winc-wintest (the pure rules on Windows, against the
# shared fixtures), winc-update-wintest (update_win.c's NT path match, unelevated only) and winc-app-wintest (the app's platform calls: one copy, the minimum, the token
# verdict, the Run value, the entry, the shortcut, the volumes), for each architecture here. Its
# external-drive case (--external) needs a real USB stick (vol_external(): a USB/1394/SD bus or a
# removable ancestor, which the vhd step's VHDX isn't): only when C:\rbtest\usb.letter names one
# (platform-owner.suite's usb-external step writes it; that suite runs the case itself)
$elev = $VmRun.Step -like '*elevated*'
$me = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($me)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elev -and -not $isAdmin) { Skip 'no elevated token for this user (an administrator account is needed)' }
$fixtures = Share 'vm/fixtures.txt'
$usbFile = 'C:\rbtest\usb.letter'
$ext = if (Test-Path -LiteralPath $usbFile) { (Get-Content -LiteralPath $usbFile -TotalCount 1).Trim() + ':\' } else { $null }
if (-not $ext) { 'no C:\rbtest\usb.letter: the external-drive case is platform-owner.suite''s' }
$ran = $false
foreach ($a in Get-TestArchs) {
    if (-not $elev) {
        $w = Get-TestExe $a 'winc-wintest.exe'
        if (-not $w) { Missing "vm/$a/winc-wintest.exe isn't staged ($a)" }
        elseif (-not (Test-Path -LiteralPath $fixtures)) { Missing "vm/fixtures.txt isn't staged: winc-wintest ($a)" }
        else {
            $ran = $true
            Copy-Item -LiteralPath $fixtures -Destination 'C:\rbtest\bin\fixtures.txt' -Force
            $rc = Invoke-Exe $w @('C:\rbtest\bin\fixtures.txt')
            Check ($rc -eq 0) "winc-wintest ($a): exit 0 (got $rc)"
        }
    }
    if (-not $elev) {
        # update_win.c's NT path match on Windows (review K2): listed images, the path's forms, the
        # 8.3 alias step; unelevated, no arguments. A "note:" line is a check left out because the
        # volume makes no 8.3 names
        $u = Get-TestExe $a 'winc-update-wintest.exe'
        if (-not $u) { Missing "vm/$a/winc-update-wintest.exe isn't staged ($a)" }
        else {
            $ran = $true
            # a start Windows refuses (ERROR_ELEVATION_REQUIRED: an exe UAC's installer detection
            # takes for an installer) is this arch's failure, not the step's end
            try { $rc = Invoke-Exe $u @() } catch { $rc = $null; $script:ExeOut = ''; "  it didn't start: $($_.Exception.Message)" }
            Check ($rc -eq 0) "winc-update-wintest ($a): exit 0 (got $rc)"
            Check ($script:ExeOut -match '(?m)^\d+ checks, 0 failed') "winc-update-wintest ($a): 'N checks, 0 failed'"
        }
    }
    $app = Get-TestExe $a 'winc-app-wintest.exe'
    if (-not $app) { Missing "vm/$a/winc-app-wintest.exe isn't staged ($a)" }
    else {
        $ran = $true
        $argv = @()
        if ($elev) { $argv += '--elevated' }
        if ($ext) { $argv += @('--external', $ext) }
        $rc = Invoke-Exe $app $argv
        Check ($rc -eq 0) "winc-app-wintest ($a$(if ($elev) { ', elevated' })): exit 0 (got $rc)"
    }
}
if (-not $ran) { Skip 'no win/c test exe staged (win/c/tests/run.sh, then vm-run.sh stage)' }
Done
