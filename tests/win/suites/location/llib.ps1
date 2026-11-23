# llib.ps1: location.suite's helpers, after lib/vmlib.ps1: the VHDX "sticks" (A: NTFS, label
# RBSTICK; B: exFAT, no label; C: NTFS, label OTHER), diskpart, and winc-location-wintest.exe
# (win/c/tests/location_wintest.c) run for one architecture, its answer line parsed. Each
# architecture this VM runs has its own support folder and its own folders on the sticks, so the
# x64 run (under Prism on ARM64) and the arm64 run don't meet.
$LocDir = 'C:\rbtest\loc-n3'
$CacheName = 'RB Stems Plus Cache.noindex'

function Get-StickVhd([string]$s) { return Join-Path $LocDir "stick-$s.vhdx" }
function LocDiskPart([string]$script) {
    New-Item -ItemType Directory -Force $LocDir | Out-Null
    $f = Join-Path $LocDir 'dp.txt'
    Set-Content -LiteralPath $f -Value $script -Encoding ASCII
    $o = & diskpart.exe /s $f 2>&1 | Out-String
    $rc = [int]$LASTEXITCODE
    Write-Host $o
    return $rc
}
function Get-StickGuid([string]$s) {
    $img = Get-DiskImage -ImagePath (Get-StickVhd $s) -ErrorAction SilentlyContinue
    if (-not $img -or -not $img.Attached) { return $null }
    $v = @($img | Get-Disk | Get-Partition | Get-Volume -ErrorAction SilentlyContinue) | Select-Object -First 1
    if ($v) { return [string]$v.Path }
    return $null
}
function Get-LetterGuid([string]$letter) {
    return [string](Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue).Path
}
$UserLetters = "$env:PUBLIC\rbsp-n3-letters.txt"   # the user's session's letters (letters.ps1)
function Get-FreeLetter([string[]]$prefer) {
    $used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
    if (Test-Path -LiteralPath $UserLetters) { $used += @(([string](Get-Content -Raw -LiteralPath $UserLetters)).Trim() -split ',') }
    return $prefer | Where-Object { $used -notcontains $_ } | Select-Object -First 1
}
function Require-Stick([bool]$ok, [string]$what) {
    Check $ok $what | Write-Host
    if (-not $ok) { exit 1 }
}
function Wait-Stick([scriptblock]$cond) {
    $until = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $until) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return [bool](& $cond)
}
# The checks go to the host; the only value returned is the volume GUID path.
function Stick([string]$s, [string]$fs, [string]$label, [string]$letter) {
    Require-Stick ($letter -match '^[A-Z]$') "a drive letter for stick $s"
    $vhd = Get-StickVhd $s
    if (Test-Path -LiteralPath $vhd) {
        if ((Get-DiskImage -ImagePath $vhd).Attached) {
            $rc = LocDiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"
            Require-Stick ($rc -eq 0) "old stick $s detached (exit $rc)"
            Require-Stick (Wait-Stick { -not (Get-StickGuid $s) }) "old stick $s's volume is gone"
        }
        Remove-Item -LiteralPath $vhd -Force
    }
    $fmt = if ($label) { "format fs=$fs quick label=$label" } else { "format fs=$fs quick" }
    $rc = LocDiskPart "create vdisk file=`"$vhd`" maximum=64 type=expandable`nselect vdisk file=`"$vhd`"`nattach vdisk`ncreate partition primary`n$fmt`nassign letter=$letter"
    Require-Stick ($rc -eq 0) "diskpart made stick $s ($fs, label '$label') at ${letter}: (exit $rc)"
    Require-Stick (Wait-Stick { Test-Path -LiteralPath "${letter}:\" }) "stick $s is mounted at ${letter}:"
    $guid = Get-StickGuid $s
    Require-Stick ($guid -match '^\\\\\?\\Volume\{[0-9a-fA-F-]{36}\}\\$' -and (Get-LetterGuid $letter) -eq $guid) "stick $s's GUID path ($guid) is at ${letter}:"
    return $guid
}
function Save-Sticks([hashtable]$h) {
    New-Item -ItemType Directory -Force $LocDir | Out-Null
    ConvertTo-Json -InputObject $h | Set-Content -LiteralPath (Join-Path $LocDir 'sticks.json') -Encoding UTF8
    "saved: $(Get-Content -Raw -LiteralPath (Join-Path $LocDir 'sticks.json'))"
}
function Read-Sticks {
    $f = Join-Path $LocDir 'sticks.json'
    if (-not (Test-Path -LiteralPath $f)) { Skip "no $f (the attach step failed: its log)" }
    return (Get-Content -Raw -LiteralPath $f | ConvertFrom-Json)
}
# the user's own scratch support folder for an architecture, and its default cache
function Get-LocSupport([string]$a) { $d = Join-Path $env:TEMP "rbsp-n3\$a\support"; New-Item -ItemType Directory -Force $d | Out-Null; return $d }
function Get-LocCache([string]$a) { return Join-Path (Get-LocSupport $a) 'Cache' }
# a second support folder whose location is on the unlabelled stick B (review N3R5)
function Get-LocSupportB([string]$a) { $d = Join-Path $env:TEMP "rbsp-n3\$a\support-b"; New-Item -ItemType Directory -Force $d | Out-Null; return $d }
$script:LocExes = @{}
function Get-LocArchs {
    foreach ($a in Get-TestArchs) {
        $exe = Get-TestExe $a 'winc-location-wintest.exe'
        if (-not $exe) { Missing "vm/$a/winc-location-wintest.exe isn't staged ($a)" | Write-Host; continue }
        $script:LocExes[$a] = $exe
        $a
    }
}
# winc-location-wintest.exe MODE ARGS for architecture a: the text after "<word> " on the line that
# starts with word ("nothing (exit N)" when there is none)
function Loc([string]$a, [string]$mode, [string]$word, [string[]]$argv) {
    # VHDX volumes are internal. Opt in for saved locations, including error cases.
    if ($mode -in @('probe', 'measure', 'measure-live', 'clear', 'remove', 'create')) { $argv += '--internal-ok' }
    $rc = Invoke-Exe $script:LocExes[$a] (@($mode) + $argv)
    $m = [regex]::Match([string]$script:ExeOut, "(?m)^$([regex]::Escape($word)) (.*?)\s*$")
    if ($rc -eq 0 -and $m.Success) { return $m.Groups[1].Value }
    return "nothing (exit $rc)"
}
# the line starting with word in the last Loc's output ('' none)
function Last-Line([string]$word) {
    $m = [regex]::Match([string]$script:ExeOut, "(?m)^$([regex]::Escape($word)) (.*?)\s*$")
    if ($m.Success) { return $m.Groups[1].Value }
    return ''
}
function Expect([string]$got, [string]$rx, [string]$what) { Check ($got -match $rx) "$what /$rx/ (got $got)" }
function Get-Hint([string]$support, [string]$name) {
    $f = Join-Path $support $name
    if (Test-Path -LiteralPath $f) { return [string](Get-Content -Raw -LiteralPath $f) }
    return ''
}
function Rx([string]$s) { return [regex]::Escape($s) }
