# W13b's 5,000-row sheets for the VM (app.md 4.2's spike with 5,000 rows, its probe test "scroll
# 5,000 rows with mixed expansion; no event reaches the core", and the Status tab's probe test), as
# the logged-in user. It runs prepare-fixture.ps1 first (its library, its scripts, its probe.conf
# and stand-in facts, unchanged) and adds to them:
#   - the outline: "Big full speed" (50) and "Big paced" (51), 5,000 tracks each (perf-start.sh's
#     and lib.sh's spike_scripts' lines: the estimate at full speed, and at 1 line/ms for 51), and
#     "Library" (100), a folder of 50 folders (101-150) of 100 playlists each, "Set 0000" to
#     "Set 4999" (1000-5999): 5,051 rows under the fixture's own five;
#   - the fake helper's defaults for every other call: tracks-default (12 local files),
#     estimate-default (12 ready) and run-default (a run that waits until it is stopped);
#   - the Status tab's sets, as the stand-in fact KEPT_SETS (probe.conf's esc: kind, ProbeCore.cs):
#     -Status big: -Sets sets (default 5,000), Set 0000 onwards; -Status small: Friday (2) and
#     Warm-up (3), the fixture's own; -Status none: no sets.
# Every set starts two helper runs when the Status tab loads (its --tracks and its check), and the
# core has 32 run slots (core/API-V1-NOTES.md M3): with -Status big most rows end as "couldn't be
# checked". The spike times the rows, not the checks.
# Local files only (never streaming tracks); no library is read and no key is involved.
#   w13b-fixture.ps1 [-App DIR] [-Out DIR] [-Mode trace|perf|perf-min] [-Status big|small|none]
#                    [-Sets N] [-Native arm64|x64] [-Clean]
#     -App, -Out, -Mode, -Native: as prepare-fixture.ps1's (passed on); -Mode defaults to perf here
#     -Clean: prepare-fixture.ps1 -Clean (probe.conf, the scripts and the fake's argv log)
# Test only: a TEST app (win/shell/build.sh --test-app) is the only one that reads probe.conf.
# Pure ASCII (Windows PowerShell 5.1 reads this file as ANSI). Exit 0, or 1 with the reason on stderr.
param(
    [string]$App = (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus\shell-smoke-test'),
    [string]$Out = 'C:\rbtest\w13b',
    [ValidateSet('trace', 'perf', 'perf-min')][string]$Mode = 'perf',
    [ValidateSet('big', 'small', 'none')][string]$Status = 'big',
    [ValidateRange(1, 5000)][int]$Sets = 5000,
    [ValidateSet('', 'arm64', 'x64')][string]$Native = '',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

function Fail([string]$Text) {
    [Console]::Error.WriteLine($Text)
    exit 1
}

$base = Join-Path $PSScriptRoot 'prepare-fixture.ps1'
if ($Clean) { & $base -App $App -Clean; exit 0 }
& $base -App $App -Out $Out -Mode $Mode -Native $Native
if ($LASTEXITCODE -ne 0) { Fail "w13b-fixture.ps1: prepare-fixture.ps1 failed ($LASTEXITCODE)" }

$utf8 = New-Object System.Text.UTF8Encoding($false)
function Put([string]$Path, [string[]]$Lines) {
    [IO.File]::WriteAllText($Path, (($Lines -join "`n") + "`n"), $utf8)
}
function J([string]$s) { '"' + ($s -replace '\\', '\\' -replace '"', '\"') + '"' }

$harness = $env:USERPROFILE.TrimEnd('\') + '\Music\Harness'
function Track([string]$name) { "$harness\$name.mp3" }
function PlaylistLine($id, $parent, $name, $kind, $tracks) {
    '{"playlist":"' + $id + '","parent":"' + $parent + '","name":' + (J $name) + ',"kind":"' + $kind + '","tracks":' + $tracks + '}'
}

# the outline's added rows, after the fixture's five (their row numbers stay the scenarios')
$added = New-Object System.Collections.Generic.List[string]
$added.Add((PlaylistLine 50 '' 'Big full speed' 'playlist' 5000))
$added.Add((PlaylistLine 51 '' 'Big paced' 'playlist' 5000))
$added.Add((PlaylistLine 100 '' 'Library' 'folder' 0))
for ($f = 0; $f -lt 50; $f++) {
    $folder = 101 + $f
    $added.Add((PlaylistLine $folder 100 ('Folder ' + $f.ToString('00')) 'folder' 0))
    for ($k = 0; $k -lt 100; $k++) {
        $n = $f * 100 + $k
        $added.Add((PlaylistLine (1000 + $n) $folder ('Set ' + $n.ToString('0000')) 'playlist' 12))
    }
}

# the spike's 5,000 tracks (perf-start.sh's lines, Windows paths)
$big = J ($harness + '\big-{n}.mp3')
$bigEst = '{"file":' + $big + ',"result":"prepare","seconds":240,"entries":22,"gb":0.06,"missing":0}'
$bigEnd = '{"estimate":5000,"ready":0,"prepare":5000,"skipped":0,"entries":110000,"runs":110000,"time_s":275000,"need_gb":300,"kept_gb":0,"share_gb":150,"max_gb":200,"cache_gb":1,"free_gb":30,"fits":false,"fit_files":2500,"max_gb_needed":400,"disk_ok":false,"asking":0,"folders":[]}'
$set = J ($harness + '\set-{n}.mp3')
$scripts = [ordered]@{
    'tracks-50' = @('@repeat 5000 {"file":' + $big + '}')
    'tracks-51' = @('@repeat 5000 {"file":' + $big + '}')
    'estimate-50' = @(('@repeat 5000 ' + $bigEst), $bigEnd)
    'estimate-51' = @('@pace 1', ('@repeat 5000 ' + $bigEst), $bigEnd)
    'tracks-default' = @('@repeat 12 {"file":' + $set + '}')
    'estimate-default' = @(
        ('@repeat 12 {"file":' + $set + ',"result":"ready","entries":20,"gb":0.05}'),
        '{"estimate":12,"ready":12,"prepare":0,"skipped":0,"entries":240,"runs":0,"time_s":0,"need_gb":0,"kept_gb":0.6,"share_gb":15,"max_gb":20,"cache_gb":1.2,"free_gb":30,"fits":true,"fit_files":12,"max_gb_needed":0.6,"disk_ok":true,"asking":0,"folders":[]}')
    'run-default' = @('@sleep 600000', '@onterm', '{"done":0,"stopped":"stopped","runs":0,"run_s":0,"kept":"","until":1800000000,"kept_gb":0}')
}
$fakeRoot = Join-Path $App 'fake-prepare'
$arches = @(Get-ChildItem -LiteralPath $fakeRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'script\playlists.jsonl') } | ForEach-Object { $_.Name })
if ($arches.Count -eq 0) { Fail "w13b-fixture.ps1: no playlists.jsonl under $fakeRoot\<arch>\script (prepare-fixture.ps1's)" }
foreach ($a in $arches) {
    $dir = Join-Path $fakeRoot "$a\script"
    $pl = Join-Path $dir 'playlists.jsonl'
    $own = @([IO.File]::ReadAllText($pl, $utf8).TrimEnd("`n") -split "`n")
    Put $pl ($own + $added)
    foreach ($k in $scripts.Keys) { Put (Join-Path $dir "$k.jsonl") $scripts[$k] }
}

# the Status tab's sets: "name\tuntil\tfiles" lines (FACT_KEPT_SETS), one esc: line in probe.conf
$conf = Join-Path $env:LOCALAPPDATA 'rbstemsplus-probe\probe.conf'
$lines = @()
if ($Status -eq 'big') { $lines = 0..($Sets - 1) | ForEach-Object { [string](1000 + $_) + '\t13 Dec 2026\t12' } }
elseif ($Status -eq 'small') { $lines = @('2\t13 Dec 2026\t3', '3\t20 Dec 2026\t2') }
if ($lines.Count -gt 0) { [IO.File]::AppendAllText($conf, ('fact.KEPT_SETS=esc:' + ($lines -join '\n') + "`n"), $utf8) }
Write-Output ("added " + $added.Count + " outline rows and " + $scripts.Count + " scripts for " + ($arches -join ', ') + "; Status: $Status (" + $lines.Count + " sets), mode $Mode")
exit 0
