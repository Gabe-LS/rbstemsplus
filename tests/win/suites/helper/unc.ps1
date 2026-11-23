# The UNC ruling (helper.md T13, option B): a track on a share, read and decoded through the track
# path kind, in each spelling (\\server\share, //server/share, \\?\UNC\), its path over 260
# characters with Unicode names, against the same file through a mapped drive and on the system
# drive (rbsp-decode-dump.exe: the same samples each way); traversal and a device path refused; a
# share that doesn't answer bounded and remembered for the run
$rd = Get-LocalExe 'rbsp-win-readable-test.exe'
$dd = Get-LocalExe 'rbsp-decode-dump.exe'
$rel = [IO.File]::ReadAllText((Need 'helper/unc/path.txt' 'helper/tests/win-vm-stage.sh'), [Text.Encoding]::UTF8).Trim()
$unc = Share "helper/$rel"
$name = Split-Path -Leaf $unc
"the track: $($unc.Length) characters"
Check ($unc.Length -gt 260) "the share path is over 260 characters ($($unc.Length))"
function Read-Once([string]$p, [string]$secs = '5') {
    $null = Invoke-Exe $rd.Exe @('--path', $p, $secs)
    return ($script:ExeOut -split "`n")[0]
}
$forms = [ordered]@{ 'UNC' = $unc; 'slashes' = ($unc -replace '\\', '/'); 'extended' = ('\\?\UNC\' + $unc.Substring(2)) }
# a mapped drive: the first free letter from Y: down (DriveInfo's list), as the user
$used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { [string]$_.Name[0] })
$letter = $null
foreach ($c in [char[]]'YXWVUTSRQPONMLKJIHG') { if ($used -notcontains [string]$c) { $letter = [string]$c; break } }
"used drive letters: $($used -join ' ')"
if ($letter) {
    $null = & net.exe use "${letter}:" '\\Mac\win' /persistent:no 2>&1
    if (Test-Path "${letter}:\helper") { $forms['mapped'] = "${letter}:\helper\" + $rel } else { Missing "the mapped drive ${letter}: (net use failed)" }
} else { Missing 'a free drive letter to map' }
# the system drive: a copy under a short folder
$local = Join-Path $dd.Dir 'local'
New-Item -ItemType Directory -Path $local | Out-Null
[IO.File]::Copy('\\?\UNC\' + $unc.Substring(2), (Join-Path $local $name))
$forms['local'] = Join-Path $local $name
$hash = @{}
foreach ($k in $forms.Keys) {
    $p = $forms[$k]
    $first = Read-Once $p
    Check ($first -match '^first: ok') "readable, $k : $first"
    $out = Join-Path $dd.Dir "out-$k"
    New-Item -ItemType Directory -Path $out | Out-Null
    $rc = Invoke-Exe $dd.Exe @('C:\no-player-folder', $out, $p)
    $f32 = @(Get-ChildItem -LiteralPath $out -Filter '*.f32' -ErrorAction SilentlyContinue)
    Check ($rc -eq 0 -and $f32.Count -ge 1 -and $script:ExeOut -notmatch 'skipped') "decoded through $k ($($f32.Count) output(s))"
    if ($f32.Count -ge 1) { $hash[$k] = (Get-FileHash -LiteralPath $f32[0].FullName -Algorithm SHA256).Hash }
}
$distinct = @($hash.Values | Select-Object -Unique)
Check ($hash.Count -eq $forms.Count -and $distinct.Count -eq 1) "the same samples through every spelling ($($hash.Count) decoded, $($distinct.Count) distinct)"
if ($letter -and $forms.Contains('mapped')) {
    $null = Invoke-Exe $rd.Exe @('--path', $forms['mapped'], '5')
    Check ($script:ExeOut -match 'may wait \(--quick doesn''t look\): 1') "a mapped network drive: --quick doesn't look"
    $null = & net.exe use "${letter}:" /delete /y 2>&1
}
# refused: traversal and a device path, before any read
$trav = Share "helper/../helper/$rel"
$first = Read-Once $trav
Check ($first -match "^first: failed .*can't use its path") "traversal refused: $first"
$first = Read-Once '\\.\C:\Windows\win.ini'
Check ($first -match "^first: failed .*can't use its path") "a device path refused: $first"
# a share that doesn't answer (an address nothing holds on the VM's own network), in one process
# (what is remembered carries over, review B8): each first read in a folder asking or failed
# within the wait, a second read in it asking at once, and the whole share once a second folder
# failed; then a share the server doesn't have (a network error, review B3), remembered too
$dead = '\\10.211.55.250\music'
$null = Invoke-Exe $rd.Exe @('--seq', '2', "$dead\a\x.wav", "$dead\a\y.wav", "$dead\b\x.wav", "$dead\c\x.wav",
    '\\localhost\rbsp-no-such-share\a\x.wav', '\\localhost\rbsp-no-such-share\a\y.wav')
$r = @{}
foreach ($l in ($script:ExeOut -split "`n")) { if ($l -match '^\s*(\d+): (\w+) after (\d+) ms') { $r[[int]$Matches[1]] = @($Matches[2], [int]$Matches[3], $l) } }
function Slow([int]$i, [string]$what) {
    $x = $r[$i]
    Check ($x -and ($x[0] -eq 'asking' -or $x[0] -eq 'failed') -and $x[1] -lt 2600) "$what : asking or failed within 2.6 s ($(if ($x) { $x[2] } else { 'no line' }))"
}
function Fast([int]$i, [string]$what) {
    $x = $r[$i]
    Check ($x -and $x[0] -eq 'asking' -and $x[1] -lt 200) "$what : asking at once ($(if ($x) { $x[2] } else { 'no line' }))"
}
Slow 1 'a dead share, folder a'
Fast 2 'folder a again'
Slow 3 'folder b, the second folder'
Fast 4 'folder c: the whole share remembered'
Slow 5 'a share the server lacks'
Fast 6 'that share again'
Done
