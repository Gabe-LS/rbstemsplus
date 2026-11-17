# The Prepare tab's VM fixture (W13b), as the logged-in user: tests/harness/lib.sh's
# fixture_library and estimate_friday for the Windows TEST app. It writes the harness's fake
# helper's scripts (tests/harness/fake-prepare-win.c: <its folder>\script\<kind>[-ID].jsonl) into
# both architectures' fake-prepare folders of the TEST app, and the probe's probe.conf with mode,
# out, helper= (this machine's fake) and the stand-in cheap facts (ProbeCore.cs ProbeFacts,
# written over the shell's own) that let the core list the playlists and enable Prepare. No rekordbox library is read and no key is involved: the playlists are the fake's.
# The library is the Mac fixture's (the scenarios' row numbers match):
#   Sets (folder 1) > Friday (2, 3 tracks) and Late (5, 1 track); Warm-up (3, 2 tracks); Smart (4, intelligent)
# with local-file tracks only (never streaming tracks) under %USERPROFILE%\Music\Harness (the files
# need not exist for the outline and the estimate). Friday's estimate: 1 ready, 2 to prepare.
#   prepare-fixture.ps1 [-App DIR] [-Out DIR] [-Mode trace|perf|perf-min] [-Native arm64|x64] [-Clean]
#     -App    the TEST app's folder (default %LOCALAPPDATA%\Programs\RB Stems Plus\shell-smoke-test,
#             where vm/install.ps1 puts it). install.ps1 replaces that folder: run this again after
#             each install
#     -Out    the probe's records (default C:\rbtest\w13b)
#     -Native the fake helper.exe named in probe.conf (default: this machine's, by the OS's architecture)
#     -Clean  removes probe.conf, and the scripts and the fake's argv log under every arch folder
#             there is; always exit 0, a missing folder said as a note
# Test only: a TEST app (win/shell/build.sh --test-app, its test-hooks DLLs) is the only one that
# reads probe.conf. Exit 0, or 1 with the reason on stderr.
param(
    [string]$App = (Join-Path $env:LOCALAPPDATA 'Programs\RB Stems Plus\shell-smoke-test'),
    [string]$Out = 'C:\rbtest\w13b',
    [ValidateSet('trace', 'perf', 'perf-min')][string]$Mode = 'trace',
    [ValidateSet('', 'arm64', 'x64')][string]$Native = '',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

function Fail([string]$Text) {
    [Console]::Error.WriteLine($Text)
    exit 1
}

$utf8 = New-Object System.Text.UTF8Encoding($false)
function Put([string]$Path, [string[]]$Lines) {
    [IO.File]::WriteAllText($Path, (($Lines -join "`n") + "`n"), $utf8)
}
# a JSON string (the paths' backslashes)
function J([string]$s) { '"' + ($s -replace '\\', '\\' -replace '"', '\"') + '"' }

$confDir = Join-Path $env:LOCALAPPDATA 'rbstemsplus-probe'
$conf = Join-Path $confDir 'probe.conf'
$fakeRoot = Join-Path $App 'fake-prepare'

# -Clean first: probe.conf always goes (a TEST app left with it runs on stand-in facts), and
# every arch folder's scripts and log, its exe there or not
if ($Clean) {
    $notes = @()
    if (Test-Path -LiteralPath $conf) { Remove-Item -LiteralPath $conf -Force; $notes += "removed $conf" } else { $notes += "no $conf" }
    if (Test-Path -LiteralPath $fakeRoot) {
        foreach ($a in @(Get-ChildItem -LiteralPath $fakeRoot -Directory)) {
            foreach ($d in 'script', 'log') {
                $p = Join-Path $a.FullName $d
                if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force; $notes += "removed $p" }
            }
        }
    } else { $notes += "no app folder $fakeRoot (nothing more to remove)" }
    $notes | ForEach-Object { Write-Output $_ }
    exit 0
}

if (-not (Test-Path -LiteralPath $fakeRoot)) { Fail "prepare-fixture.ps1: no $fakeRoot (a TEST app: build.sh --test-app, put there by vm/install.ps1; or -App names the wrong folder)" }
$arches = @(Get-ChildItem -LiteralPath $fakeRoot -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'fake-prepare.exe') } | ForEach-Object { $_.Name })
if ($arches.Count -eq 0) { Fail "prepare-fixture.ps1: no fake-prepare.exe under $fakeRoot" }

# Windows paths as text (never resolved here: the files need not exist)
$harness = $env:USERPROFILE.TrimEnd('\') + '\Music\Harness'
function Track([string]$name) { "$harness\$name.mp3" }
function PlaylistLine($id, $parent, $name, $kind, $tracks) {
    '{"playlist":"' + $id + '","parent":"' + $parent + '","name":' + (J $name) + ',"kind":"' + $kind + '","tracks":' + $tracks + '}'
}
$estimateEnd = '"skipped":0,"entries":85,"runs":45,"time_s":112,"need_gb":0.13,"kept_gb":0.12,"share_gb":15,"max_gb":20,"cache_gb":1.2,"free_gb":30,"max_gb_needed":0.4,"disk_ok":true,"asking":0,"folders":[]'
$scripts = [ordered]@{
    'playlists' = @(
        (PlaylistLine 1 '' 'Sets' 'folder' 0),
        (PlaylistLine 2 1 'Friday' 'playlist' 3),
        (PlaylistLine 5 1 'Late' 'playlist' 1),
        (PlaylistLine 3 '' 'Warm-up' 'playlist' 2),
        (PlaylistLine 4 '' 'Smart' 'intelligent' 0))
    'tracks-2' = @('a', 'b', 'c' | ForEach-Object { '{"file":' + (J (Track "friday-$_")) + '}' })
    'tracks-3' = @('a', 'b' | ForEach-Object { '{"file":' + (J (Track "warmup-$_")) + '}' })
    'tracks-5' = @('{"file":' + (J (Track 'late-a')) + '}')
    'estimate-2' = @(
        '@pace 60',
        ('{"file":' + (J (Track 'friday-a')) + ',"result":"ready","entries":40,"gb":0.12}'),
        ('{"file":' + (J (Track 'friday-b')) + ',"result":"prepare","seconds":312,"entries":28,"gb":0.08,"missing":0}'),
        ('{"file":' + (J (Track 'friday-c')) + ',"result":"prepare","seconds":185,"entries":17,"gb":0.05,"missing":0}'),
        ('{"estimate":3,"ready":1,"prepare":2,"fits":true,"fit_files":3,' + $estimateEnd + '}'))
}
foreach ($a in $arches) {
    $dir = Join-Path $fakeRoot "$a\script"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    foreach ($k in $scripts.Keys) { Put (Join-Path $dir "$k.jsonl") $scripts[$k] }
}

# this machine's own fake (an ARM64 Windows runs the arm64 one, as the AnyCPU app runs natively)
$native = $Native
if (-not $native) { $native = if ((Get-CimInstance Win32_OperatingSystem).OSArchitecture -match 'ARM') { 'arm64' } else { 'x64' } }
if ($arches -notcontains $native) { Fail "prepare-fixture.ps1: no $native fake-prepare.exe under $fakeRoot" }
# the stand-in cheap facts: the playlists (helper there, a 64-bit host) and Prepare's problems
# list empty (rekordbox installed and supported, Stems Cache and Demucs v4 present, the bridge
# keeping, the root ORT copy there, the config on, max_gb 20 as the estimate's)
$facts = @(
    'fact.APPLE_SILICON=flag:1', 'fact.HELPER_THERE=flag:1',
    'fact.REKORDBOX_INSTALLED=flag:1', 'fact.REKORDBOX_SUPPORTED=flag:1', 'fact.REKORDBOX_VERSION=str:7.2.19',
    'fact.STEMS_CACHE_PRESENT=flag:1', 'fact.STEMS_PLUS_PRESENT=flag:1', 'fact.BRIDGE_KEEPS=flag:1', 'fact.ROOT_ORT_EXISTS=flag:1',
    'fact.CONFIG_ENABLED=flag:1', 'fact.CONFIG_MAX_GB=str:20')
New-Item -ItemType Directory -Force -Path $confDir | Out-Null
Put $conf (@("mode=$Mode", "out=$Out", ("helper=" + (Join-Path $fakeRoot "$native\fake-prepare.exe"))) + $facts)
Write-Output ("wrote " + $scripts.Count + " scripts for " + ($arches -join ', ') + " and $conf (helper $native, " + $facts.Count + " facts)")
exit 0
