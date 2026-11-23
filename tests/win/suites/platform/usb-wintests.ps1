# app.md section 7: winc-app-wintest's external-drive case (--external) on the USB stick
# usb-external found (C:\rbtest\usb.letter: vol_external() wants a USB bus, which no VHDX has),
# unelevated (usb-wintests) and elevated (usb-wintests-elevated, with --elevated), for each
# architecture here. No stick: SKIP.
$elev = $VmRun.Step -like '*elevated*'
$me = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($me)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elev -and -not $isAdmin) { Skip 'no elevated token for this user (an administrator account is needed)' }
$usbFile = 'C:\rbtest\usb.letter'
if (-not (Test-Path -LiteralPath $usbFile)) { Skip 'no USB stick here (C:\rbtest\usb.letter: usb-external SKIPped or failed)' }
$ext = (Get-Content -LiteralPath $usbFile -TotalCount 1).Trim() + ':\'
$ran = $false
foreach ($a in Get-TestArchs) {
    $app = Get-TestExe $a 'winc-app-wintest.exe'
    if (-not $app) { Missing "vm/$a/winc-app-wintest.exe isn't staged ($a)"; continue }
    $ran = $true
    $argv = @('--external', $ext)
    if ($elev) { $argv = @('--elevated') + $argv }
    $rc = Invoke-Exe $app $argv
    Check ($rc -eq 0) "winc-app-wintest --external $ext ($a$(if ($elev) { ', elevated' })): exit 0 (got $rc)"
}
if (-not $ran) { Skip 'no winc-app-wintest.exe staged (win/c/tests/run.sh, then vm-run.sh stage)' }
Done
