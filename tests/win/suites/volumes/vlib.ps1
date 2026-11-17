# vlib.ps1: volumes.suite's own helpers, after lib/vmlib.ps1 (the suite's second include): the
# VHDX of the step's file system (its name's first word: ntfs-..., exfat-...), diskpart, the
# volume's GUID path, and location-win-test's one-location modes (--make, --check, --gone:
# common/tests/location_args.h) run for each architecture this VM runs.
$VolDir = 'C:\rbtest\vol'
$CacheName = 'RB Stems Plus Cache.noindex'

function Get-VolFs {
    $fs = ($VmRun.Step -split '-', 2)[0]
    if (@('ntfs', 'exfat') -notcontains $fs) { Skip "no file system in the step's name ($($VmRun.Step))" }
    return $fs
}
function Get-VolVhd([string]$fs) { return Join-Path $VolDir "$fs.vhdx" }
function Get-VolMountFolder([string]$fs) { return "C:\mnt\x-$fs" }
# diskpart with a script: its output straight to this step's stdout (not the pipeline, so the
# caller gets the exit code alone)
function DiskPart([string]$script) {
    New-Item -ItemType Directory -Force $VolDir | Out-Null
    $f = Join-Path $VolDir 'dp.txt'
    Set-Content -LiteralPath $f -Value $script -Encoding ASCII
    $o = & diskpart.exe /s $f 2>&1 | Out-String
    $rc = [int]$LASTEXITCODE
    [Console]::Out.WriteLine($o)
    return $rc
}
# the VHDX's volume GUID path ("\\?\Volume{...}\"), or $null when it isn't attached (as SYSTEM)
function Get-VolGuid([string]$fs) {
    $img = Get-DiskImage -ImagePath (Get-VolVhd $fs) -ErrorAction SilentlyContinue
    if (-not $img -or -not $img.Attached) { return $null }
    $v = @($img | Get-Disk | Get-Partition | Get-Volume -ErrorAction SilentlyContinue) | Select-Object -First 1
    if ($v) { return [string]$v.Path }
    return $null
}
# what the sys steps leave for the user steps: C:\rbtest\vol\<fs>.json (Letter, Guid)
function Save-VolInfo([string]$fs, [hashtable]$h) {
    New-Item -ItemType Directory -Force $VolDir | Out-Null
    ConvertTo-Json -InputObject $h | Set-Content -LiteralPath (Join-Path $VolDir "$fs.json") -Encoding UTF8
    "saved: $(Get-Content -Raw -LiteralPath (Join-Path $VolDir "$fs.json"))"
}
function Read-VolInfo([string]$fs) {
    $f = Join-Path $VolDir "$fs.json"
    if (-not (Test-Path -LiteralPath $f)) { Skip "no $f (the step that attaches the $fs VHDX failed: its log)" }
    return (Get-Content -Raw -LiteralPath $f | ConvertFrom-Json)
}
# the GUID path in upper case (its hex digits; "\\?\Volume" as it is)
function ConvertTo-UpperGuid([string]$g) {
    return [regex]::Replace($g, '\{[0-9A-Fa-f-]+\}', [Text.RegularExpressions.MatchEvaluator] { param($m) $m.Value.ToUpperInvariant() })
}
# location-win-test ARGS for each architecture here: its verdict (the "verdict V" line) wanted,
# one Check each (Missing when an architecture's exe isn't staged); $want '' = said only (a case
# whose outcome is information); $mount: the volume an ok is pinned to (its "mount M" line), checked
# in each architecture's run (review WA10)
function Test-Loc([string]$what, [string[]]$argv, [string]$want, [string]$mount = '') {
    foreach ($a in Get-TestArchs) {
        $exe = Get-TestExe $a 'location-win-test.exe'
        if (-not $exe) { Missing "vm/$a/location-win-test.exe isn't staged ($a)"; continue }
        $rc = Invoke-Exe $exe $argv
        $m = [regex]::Match([string]$script:ExeOut, '(?m)^(verdict|made) (.+?)\s*$')
        $got = if ($m.Success) { if ($m.Groups[1].Value -eq 'made') { 'made' } else { $m.Groups[2].Value } } else { "nothing (exit $rc)" }
        if ($want) { Check ($rc -eq 0 -and $got -eq $want) "$what ($a): $want (got $got)" }
        else { "INFO $what ($a): $got (exit $rc)" }
        if ($mount) {
            $pins = @([regex]::Matches([string]$script:ExeOut, '(?m)^mount (.+?)\s*$') | ForEach-Object { $_.Groups[1].Value })
            Check (@($pins | Where-Object { $_ -ieq $mount }).Count -eq 1 -and $pins.Count -eq 1) "$what ($a): pinned to its own volume ($mount; got $($pins -join ', '))"
        }
    }
}
