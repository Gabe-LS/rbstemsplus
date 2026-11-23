# app.md 5.4 against GitHub itself: update_win.c's WinHTTP download (winc-update-wintest --get) of
# the latest release's payload.json (github.com's 302 to its asset host, as the app's update check
# fetches it), whole (200), then resumed from its middle over that 302 (206, Range kept), for each
# architecture here. Only when vm-run.conf says "github allowed" (Little Snitch's rules for
# Parallels' traffic to github.com, release-assets.githubusercontent.com (where GitHub's 302
# points today) and objects.githubusercontent.com, the owner's once): else SKIP.
if (-not (Test-Path -LiteralPath "\\Mac\win\job\$($VmRun.Run)\github.allowed")) {
    Skip 'GitHub not allowed for the VM (.orchestration/vm-run.conf: "github allowed", once Little Snitch lets Parallels reach github.com, release-assets.githubusercontent.com and objects.githubusercontent.com)'
}
$url = 'https://github.com/Gabe-LS/rbstemsplus/releases/latest/download/payload.json'
$max = 1048576
function Get-Sha([string]$f) { if (Test-Path -LiteralPath $f) { return (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash } return '' }
function Invoke-Get([string]$exe, [long]$from, [string]$out) {
    $rc = Invoke-Exe $exe @('--get', $url, "$from", "$max", $out)
    $m = [regex]::Match([string]$script:ExeOut, '(?m)^get result (-?\d+) status (-?\d+) err (-?\d+) at (-?\d+) total (-?\d+) bytes (-?\d+)')
    if ($rc -ne 0 -or -not $m.Success) { return $null }
    return [pscustomobject]@{ Result = [int]$m.Groups[1].Value; Status = [int]$m.Groups[2].Value; At = [long]$m.Groups[4].Value
        Total = [long]$m.Groups[5].Value; Bytes = [long]$m.Groups[6].Value }
}
$ran = $false
foreach ($a in Get-TestArchs) {
    $exe = Get-TestExe $a 'winc-update-wintest.exe'
    if (-not $exe) { Missing "vm/$a/winc-update-wintest.exe isn't staged ($a)"; continue }
    $ran = $true
    $out = "C:\rbtest\gh-$a.json"
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    $g = Invoke-Get $exe 0 $out
    $n = if (Test-Path -LiteralPath $out) { (Get-Item -LiteralPath $out).Length } else { 0 }
    Check ($g -and $g.Result -eq 0 -and $g.Status -eq 200 -and $g.Bytes -eq $n -and $n -gt 2) "$url over its 302 ($a): 200, $n bytes$(if (-not ($g -and $g.Result -eq 0)) { ' (if the VM can''t reach it: allow the host GitHub''s 302 names, today release-assets.githubusercontent.com)' })"
    if ($n -gt 2) {
        $whole = Get-Sha $out
        $k = [long][Math]::Floor($n / 2)
        $s = [IO.File]::Open($out, 'Open', 'ReadWrite'); try { $s.SetLength($k) } finally { $s.Close() }
        $g = Invoke-Get $exe $k $out
        Check ($g -and $g.Result -eq 0 -and $g.Status -eq 206 -and $g.At -eq $k -and $g.Total -eq $n -and (Get-Sha $out) -eq $whole) "resumed from byte $k over GitHub's 302 ($a): 206 from there, the file whole"
    }
}
if (-not $ran) { Skip 'no winc-update-wintest.exe staged (win/c/tests/run.sh, then vm-run.sh stage)' }
Done
