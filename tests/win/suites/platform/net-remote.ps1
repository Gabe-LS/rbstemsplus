# As the user with its full token (removing a share needs it), in this one process (a mapping
# belongs to its logon session): \\127.0.0.1\rbgone (smb-share's) mapped as a drive is a network
# drive, DRIVE_REMOTE, and location_check says "not-local" there (location.c: the table of volumes
# lists no network drive); then the share is removed under the mapping, which stays (net use:
# Disconnected once the server's sessions are closed). A mapped but disconnected drive is still
# DRIVE_REMOTE and "not-local", as plat_win.h's plat_win_drive_remote note says.
$CacheName = 'RB Stems Plus Cache.noindex'
if (-not (Get-SmbShare -Name rbgone -ErrorAction SilentlyContinue)) { Skip 'no share rbgone (the smb-share step failed: its log)' }
$used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
$q = @('Q', 'P', 'O', 'N', 'M') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
if (-not $q) { Skip 'no free drive letter for the mapping' }
# location-win-test --check DIR 0 for each architecture: its verdict ('' when it printed none)
function Get-Verdicts([string]$dir) {
    $v = @{}
    foreach ($a in Get-TestArchs) {
        $exe = Get-TestExe $a 'location-win-test.exe'
        if (-not $exe) { Missing "vm/$a/location-win-test.exe isn't staged ($a)"; continue }
        $rc = Invoke-Exe $exe @('--check', $dir, '0')
        $m = [regex]::Match([string]$script:ExeOut, '(?m)^verdict (.+?)\s*$')
        $v[$a] = if ($rc -eq 0 -and $m.Success) { $m.Groups[1].Value } else { "nothing (exit $rc)" }
    }
    return $v
}
$rc = Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${q}:", '\\127.0.0.1\rbgone', '/persistent:no')
Check ($rc -eq 0) "net use ${q}: \\127.0.0.1\rbgone (exit $rc)"
if ($rc -ne 0) { Done }
$t = (New-Object IO.DriveInfo "${q}:\").DriveType
Check ($t -eq 'Network') "${q}: is a network drive (DriveType $t)"
$v = Get-Verdicts "${q}:\$CacheName"
foreach ($a in $v.Keys) { Check ($v[$a] -eq 'not-local') "a location on the mapped share ($a): not-local (got $($v[$a]))" }
Remove-SmbShare -Name rbgone -Force
Check (-not (Get-SmbShare -Name rbgone -ErrorAction SilentlyContinue)) 'the share rbgone is removed under the mapping'
# the removal leaves the mapping's session up ("OK"): the server's sessions closed, so the mapping
# is a disconnected one (its next use finds no share)
Get-SmbSession -ErrorAction SilentlyContinue | Close-SmbSession -Force -ErrorAction SilentlyContinue
[void](Test-Path -LiteralPath "${q}:\")
[void](Invoke-Exe "$env:WINDIR\System32\net.exe" @('use'))
Check ($script:ExeOut -notmatch "(?m)^\s*OK\s+${q}:") "the mapping ${q}: is no longer connected (net use: not OK)"
# a mapping is a network drive connected or not (plat_win.h's note, found so in the VM round of
# 2026-11-17): DRIVE_REMOTE, and "not-local" still
$t = (New-Object IO.DriveInfo "${q}:\").DriveType
Check ($t -eq 'Network') "the mapping with its share gone is still a network drive (DriveType $t, DRIVE_REMOTE)"
$v = Get-Verdicts "${q}:\$CacheName"
foreach ($a in $v.Keys) { Check ($v[$a] -eq 'not-local') "a location on the mapped drive with its share gone ($a): not-local (got $($v[$a]))" }
[void](Invoke-Exe "$env:WINDIR\System32\net.exe" @('use', "${q}:", '/delete', '/y'))
Done
