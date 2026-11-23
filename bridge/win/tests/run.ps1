# The Windows bridge's harness groups in the VM (bridge.md section 4, T7): harness.exe (bridge/tests/
# harness.c, imported by ordinal as rekordbox.exe does) against the TEST bridge, with the 1.17.0
# fake (fake_ort.c) or no real library at all, and Pioneer's real one when it is here. Windows
# PowerShell 5.1 syntax only.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File run.ps1 [-Group all|passthrough|missing|load|gpu|junction|network]
#                                                               [-Dir FOLDER] [-Real DLL] [-Drive LETTER]
#
# FOLDER (default: this script's own folder) holds what bridge/win/tests/stage.sh stages into the
# share's bridge-win\: onnxruntime.dll (the TEST bridge, bridge/win/build.sh --test), harness.exe,
# fake\onnxruntime.dll and model\hdemucs.onnx (bridge/tests/make_model.py's test model). The
# groups, as bridge/tests/run.sh names them, and gpu:
#   passthrough  the fake as the real library: the bridge's base forwards the fake's 1.17.0, the
#                fake's table and providers untouched
#   missing      no real library: the refusal stub (r4:I46), and the log says why
#   load         the real library (-Real, else the newest rekordbox's in Program Files): 1.18,
#                Run, CreateSession and ReleaseSession hooked. Without one: SKIP
#   gpu          (review BW1) the real library as for load, and harness run on the test model: a
#                chunk cached, then run with its mix and then a pre-allocated x marked as GPU
#                memory (harness's gpu=in/out, the TEST bridge's RBSTEMS_TEST_GPU_VALUE): passed
#                through ("model"), nothing stored; unmarked again, a hit. Without one: SKIP
#   junction     (review BX6; a regression row for the reparse refusal, review TR71) the real
#                library and the test model as for gpu, and a chosen location on another local
#                volume (-Drive: Fixed and NTFS; else the letter the suite's sys step bridge/vhd.ps1
#                leaves in C:\rbtest\vhd\bridge.letter: Fixed, NTFS, labelled RBBRIDGE, review TR73)
#                whose final component, "RB Stems Plus Cache.noindex", is a junction to a folder on
#                C: holding the marker: unavailable ("folder"), nothing written through it. Without a
#                real library, the model or the drive: SKIP
#   network      (review TR71: BW3's row) the real library and the test model as for gpu, and a
#                chosen location on the VM's share mapped as a network drive (net use, on a free
#                letter of J, I, H, G; removed again after): unavailable ("not-local", where
#                4b11e563's own check_location gives "not-connected"), nothing made on the share.
#                Without a real library, the model or the mapping: SKIP
#   all          passthrough and missing, and load, gpu, junction and network when what they need
#                is here (else MISSING, so it exits 77, review TR74)
# Each group copies the bridge and harness.exe to C:\rbtest\bridge-run\GROUP and runs it there with
# a fresh home (RBSTEMS_TEST_HOME=C:\rbtest\bridge-home\GROUP) and RBSP_TEST_PROGRAMS=C:\rbtest-pf,
# so the TEST bridge's real library is C:\rbtest-pf\RB Stems Plus\ort\onnxruntime.dll. Its two
# folders are made here, owned by this account and writable by it, SYSTEM and Administrators alone,
# as the TEST bridge's owner check asks. Run it unelevated (I89).
#
# The harness's run, as these groups drive it (bridge/tests/harness.c, reviews BX4 and BX5): drain
# or wait for the unmarked misses before the first gpu= mark, as gpu does (they are counted from
# that mark: an entry stored after it beyond them fails the next drain, wait or sleep); a cache
# folder that isn't there yet counts 0, and one there that can't be listed counts -1 and fails the
# run ("FAIL wait/drain/sleep: can't list"). A "FAIL run C: a GPU-marked run was served" line is a
# FAIL here like any other. ReleaseSession at exit with shutdown in progress (review BX1) is T10's
# row: tests/win/probes/ortprobe.c logs it in rekordbox, where Demucs3.dll's own exit path runs;
# harness.exe has no such path, so no group here checks it.
#
# Output as tests/win/suites' steps print it (lib/vmlib.ps1): PASS/FAIL per check, MISSING, SKIP;
# exit 0 when every check passed, 1 on a FAIL (or nothing checked), 77 when it can't run here. In
# a suite step (vmlib.ps1 included first) it is dot-sourced, and vmlib's Check/Skip/Missing/Done
# are the ones used; the step calls Done.
param([string]$Group = 'all', [string]$Dir = '', [string]$Real = '', [string]$Drive = '')

$standalone = -not (Get-Command Done -CommandType Function -ErrorAction SilentlyContinue)
if ($standalone) {
    $ErrorActionPreference = 'Stop'
    $script:VmChecks = 0
    $script:VmFails = 0
    $script:VmMissing = 0
    function Check([bool]$ok, [string]$what) {
        $script:VmChecks++
        if ($ok) { "PASS $what" } else { $script:VmFails++; "FAIL $what" }
    }
    function Skip([string]$why) { "SKIP $why"; exit 77 }
    function Missing([string]$what) { $script:VmMissing++; "MISSING $what" }
    function Done {
        "bridge harness: $script:VmChecks checks, $script:VmFails failed, $script:VmMissing missing"
        if ($script:VmFails -gt 0) { exit 1 }
        if ($script:VmMissing -gt 0) { "SKIP $script:VmMissing case(s) missing (above): never a pass"; exit 77 }
        if ($script:VmChecks -eq 0) { exit 1 }
        exit 0
    }
}

if (-not $Dir) { $Dir = $PSScriptRoot }
$BridgePf = 'C:\rbtest-pf'
$BridgeRealDir = Join-Path $BridgePf 'RB Stems Plus\ort'
$BridgeRealPath = Join-Path $BridgeRealDir 'onnxruntime.dll'

# the bytes of FILE hold TEXT as UTF-16LE
function Test-Utf16([string]$file, [string]$text) {
    $b = [IO.File]::ReadAllBytes($file)
    $s = [Text.Encoding]::Unicode.GetString($b)
    if ($s.Contains($text)) { return $true }
    # an odd offset: the same text one byte along
    if ($b.Length -gt 1) { $s = [Text.Encoding]::Unicode.GetString($b, 1, $b.Length - 1); return $s.Contains($text) }
    return $false
}

# a folder only this account, SYSTEM and Administrators can change (inheritance off), made when absent
function Set-BridgeProtected([string]$path) {
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $eap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $o = (& icacls.exe $path /inheritance:r /grant:r "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" "*${me}:(OI)(CI)F" 2>&1 | Out-String)
        $rc = $LASTEXITCODE
    } finally { $ErrorActionPreference = $eap }
    if ($rc -ne 0) { [Console]::Out.WriteLine("  icacls: $o") }
    return ($rc -eq 0)
}

# runs harness.exe in FOLDER with ARGV and the bridge's test variables (and EXTRA's): its exit
# code; its lines (indented) to this output and in $script:HarnessOut
function Invoke-Harness([string]$folder, [string]$bhome, [string[]]$argv, [hashtable]$extra = @{}) {
    $vars = @{ RBSTEMS_TEST_HOME = $bhome; RBSP_TEST_PROGRAMS = $BridgePf }
    foreach ($k in $extra.Keys) { $vars[$k] = $extra[$k] }
    $saved = @{}
    foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
    $eap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        Push-Location -LiteralPath $folder
        try { $lines = @(& (Join-Path $folder 'harness.exe') @argv 2>&1 | ForEach-Object { [string]$_ }); $rc = $LASTEXITCODE }
        finally { Pop-Location }
        foreach ($x in $lines) { [Console]::Out.WriteLine("  $x") }
        $script:HarnessOut = $lines
        return $rc
    } finally {
        $ErrorActionPreference = $eap
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

# one group: the harness's own ok/FAIL lines as checks (at least MIN of them), its exit code 0.
# CONFIG, when given, is the home's rbstemsplus\config.ini (LF, no BOM); EXTRA, more variables
function Invoke-BridgeGroup([string]$name, [string[]]$argv, [int]$min, [string]$config = '', [hashtable]$extra = @{}) {
    $folder = "C:\rbtest\bridge-run\$name"
    $bhome = "C:\rbtest\bridge-home\$name"
    foreach ($d in @($folder, $bhome)) {
        if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $Dir 'onnxruntime.dll') -Destination $folder -Force
    Copy-Item -LiteralPath (Join-Path $Dir 'harness.exe') -Destination $folder -Force
    if ($config) {
        New-Item -ItemType Directory -Force -Path (Join-Path $bhome 'rbstemsplus') | Out-Null
        [IO.File]::WriteAllText((Join-Path $bhome 'rbstemsplus\config.ini'), $config, (New-Object Text.UTF8Encoding $false))
    }
    $rc = Invoke-Harness $folder $bhome $argv $extra
    $oks = 0
    foreach ($x in $script:HarnessOut) {
        if ($x.StartsWith('ok ')) { $oks++; Check $true "$($name): $($x.Substring(3).Trim())" }
        elseif ($x.StartsWith('FAIL')) { Check $false "$($name): $($x.Substring(4).Trim())" }
    }
    Check ($rc -eq 0) "$($name): harness.exe exits 0 (got $rc)"
    Check ($oks -ge $min) "$($name): at least $min harness checks passed (got $oks)"
    $log = Join-Path $bhome 'rbstemsplus\Logs\bridge.log'
    $script:BridgeLog = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log) } else { '' }
    Check ($script:BridgeLog -ne '') "$($name): the bridge wrote its log ($log)"
    $state = Join-Path $bhome 'rbstemsplus\Logs\bridge-state'
    $script:BridgeState = if (Test-Path -LiteralPath $state) { @([IO.File]::ReadAllText($state) -split "`n") } else { @() }
}

# the test model copied to C:\rbtest\bridge-model\GROUP\hdemucs.onnx (named so, as the bridge
# wants): its path, or '' (GROUP SKIPs when it is the one asked for)
function Use-BridgeModel([string]$group) {
    $model = Join-Path $Dir 'model\hdemucs.onnx'
    if (-not (Test-Path -LiteralPath $model)) {
        if ($Group -eq $group) { Skip "no model\hdemucs.onnx in $Dir (bridge/win/tests/stage.sh stages it)" }
        [Console]::Out.WriteLine((Missing "$($group): not run: no model\hdemucs.onnx in $Dir"))    # not output: the function's value is the path
        return ''
    }
    $m = "C:\rbtest\bridge-model\$group\hdemucs.onnx"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $m) | Out-Null
    Copy-Item -LiteralPath $model -Destination $m -Force
    return $m
}

# the run's answers and stored counts, in order: "a model, stored 1, ..."
function Get-RunAnswers {
    return @($script:HarnessOut | ForEach-Object { if ($_ -match '^([a-z] (model|cache)|stored -?\d+)') { $Matches[1] } }) -join ', '
}

# Pioneer's real library (-Real, else the newest rekordbox's in Program Files) put where the TEST
# bridge loads it, with its DirectML.dll: its path, or '' (GROUP SKIPs when it is the one asked for)
function Use-BridgeReal([string]$group) {
    $src = $Real
    if (-not $src) {
        $rb = Join-Path $env:ProgramFiles 'rekordbox'
        $cand = @(Get-ChildItem -LiteralPath $rb -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'onnxruntime.dll' } | Where-Object { Test-Path -LiteralPath $_ })
        if ($cand.Count) { $src = $cand[0] }
    }
    if (-not $src) {
        if ($Group -eq $group) { Skip "no real library: -Real, or rekordbox installed in $env:ProgramFiles\rekordbox" }
        [Console]::Out.WriteLine((Missing "$($group): not run: no real library (-Real, or rekordbox installed)"))
        return ''
    }
    Copy-Item -LiteralPath $src -Destination $BridgeRealPath -Force
    $dml = Join-Path (Split-Path -Parent $src) 'DirectML.dll'     # the real library's own dependency, beside it
    if (Test-Path -LiteralPath $dml) { Copy-Item -LiteralPath $dml -Destination $BridgeRealDir -Force }
    [Console]::Out.WriteLine("$($group): the real library from $src")    # not output: the function's value is the path
    return $src
}

# ------------------------------------------------------------------ what this run has
foreach ($f in @('onnxruntime.dll', 'harness.exe', 'fake\onnxruntime.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $Dir $f))) { Skip "no $f in $Dir (bridge/win/tests/stage.sh stages it)" }
}
$bridgeDll = Join-Path $Dir 'onnxruntime.dll'
Check (Test-Utf16 $bridgeDll 'RBSP_TEST_PROGRAMS') 'the bridge is a TEST build (it reads RBSP_TEST_PROGRAMS)'
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if ($me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { 'note: running elevated; the bridge is meant to be run unelevated (I89)' }
foreach ($d in @("$BridgePf\RB Stems Plus", $BridgeRealDir)) {
    Check (Set-BridgeProtected $d) "$d made, writable by this account, SYSTEM and Administrators alone"
}

$groups = switch ($Group) {
    'all' { @('passthrough', 'missing', 'load', 'gpu', 'junction', 'network') }
    'passthrough' { @('passthrough') }
    'missing' { @('missing') }
    'load' { @('load') }
    'gpu' { @('gpu') }
    'junction' { @('junction') }
    'network' { @('network') }
    default { Skip "no group '$Group' (all, passthrough, missing, load, gpu, junction or network)" }
}

foreach ($g in $groups) {
    if (Test-Path -LiteralPath $BridgeRealPath) { Remove-Item -LiteralPath $BridgeRealPath -Force }
    if ($g -eq 'passthrough') {
        Copy-Item -LiteralPath (Join-Path $Dir 'fake\onnxruntime.dll') -Destination $BridgeRealPath -Force
        Invoke-BridgeGroup 'passthrough' @('passthrough', $BridgeRealPath) 7
    } elseif ($g -eq 'missing') {
        Invoke-BridgeGroup 'missing' @('missing') 5
        Check ($script:BridgeLog.Contains('is missing: rekordbox gets the stub')) 'missing: the log says the real library is missing and rekordbox gets the stub'
    } elseif ($g -eq 'load' -or $g -eq 'gpu' -or $g -eq 'junction' -or $g -eq 'network') {
        $src = Use-BridgeReal $g
        if (-not $src) { continue }
        if ($g -eq 'load') { Invoke-BridgeGroup 'load' @('load', $BridgeRealPath) 13; continue }
        $m = Use-BridgeModel $g
        if (-not $m) { continue }
        if ($g -eq 'junction') {
            # review BX6, a regression row for the reparse refusal (review TR71): the chosen
            # location's final component a junction to a folder on another volume (C:) that holds
            # the right marker. 4b11e563's copy and location.c both give "folder" here, and nothing
            # is written through it; BW3's row is network
            $letter = $Drive
            $lf = 'C:\rbtest\vhd\bridge.letter'
            if (-not $letter -and (Test-Path -LiteralPath $lf)) { $letter = ([IO.File]::ReadAllText($lf)).Trim() }
            # review TR73: a local NTFS volume (and the vhd step's, RBBRIDGE, unless -Drive) before
            # anything on it is touched
            $why = ''
            if ($letter -notmatch '^[A-Za-z]$') { $why = 'no other local volume' }
            else {
                try {
                    $di = [IO.DriveInfo]"${letter}:\"
                    $dt = [string]$di.DriveType
                    $df = if ($di.IsReady) { [string]$di.DriveFormat } else { '' }
                    $dl = if ($di.IsReady) { [string]$di.VolumeLabel } else { '' }
                } catch { $dt = ''; $df = ''; $dl = '' }
                if ($dt -ne 'Fixed' -or $df -ne 'NTFS' -or (-not $Drive -and $dl -ne 'RBBRIDGE')) {
                    $why = "${letter}: isn't the VHDX's local NTFS volume (DriveType $dt, $df, label $dl)"
                }
            }
            if ($why) {
                if ($Group -eq 'junction') { Skip "junction: $why (-Drive, or bridge.suite's vhd step, $lf)" }
                Missing "junction: not run: $why (-Drive, or bridge.suite's vhd step)"
                continue
            }
            $id = '0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F'
            $top = "${letter}:\rbtest-bx6"
            $loc = Join-Path $top 'RB Stems Plus Cache.noindex'
            $target = 'C:\rbtest\bridge-junction\real'
            # the junction removed as itself first: Windows PowerShell 5.1's Remove-Item -Recurse
            # can follow one into its target
            if (Test-Path -LiteralPath $loc) { & cmd.exe /c rmdir "$loc" | Out-Null }
            foreach ($d in @($top, $target)) {
                if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
                New-Item -ItemType Directory -Force -Path $d | Out-Null
            }
            [IO.File]::WriteAllText((Join-Path $target '.rbstemsplus-cache'), "id=$id`nformat=rbstems-v1`n", (New-Object Text.UTF8Encoding $false))
            $eap = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'                       # mklink's stderr isn't a throw
                $o = (& cmd.exe /c mklink /J "$loc" "$target" 2>&1 | Out-String).Trim()
            } finally { $ErrorActionPreference = $eap }
            $isj = (Test-Path -LiteralPath $loc) -and ([bool]((Get-Item -LiteralPath $loc -Force).Attributes -band [IO.FileAttributes]::ReparsePoint))
            Check $isj "junction: $loc is a junction to $target ($o)"
            if ($isj) {
                Invoke-BridgeGroup 'junction' @('run', $m, 'a', 'sleep') 0 "cache_dir=`"$loc`"`ncache_id=$id`n" @{ RBSTEMS_HARNESS_CACHE = $target }
                $got = Get-RunAnswers
                Check ($got -eq 'a model, stored 0') "junction: the model runs, nothing stored (got: $got; want: a model, stored 0)"
                Check (($script:BridgeState -contains 'state=unavailable') -and ($script:BridgeState -contains 'reason=folder')) "junction: bridge-state says unavailable, reason folder (got: $($script:BridgeState -join ' '))"
                Check ($script:BridgeLog.Contains('not available (its folder is missing, or something on the way to it')) 'junction: the log says the location is unavailable (folder)'
                $left = @(Get-ChildItem -LiteralPath $target -Force -Recurse | ForEach-Object { $_.Name }) -join ' '
                Check ($left -eq '.rbstemsplus-cache') "junction: nothing written through it ($target holds: $left)"
            }
            if (Test-Path -LiteralPath $loc) { & cmd.exe /c rmdir "$loc" | Out-Null }
            continue
        }
        if ($g -eq 'network') {
            # review TR71 (BW3's row in the VM): a chosen location on a network drive, the VM's share
            # mapped on a free letter. location.c gives "not-local"; 4b11e563's own check_location
            # gives "not-connected" (FindFirstVolume lists no network drive), so this row tells the
            # two apart. Nothing is made on the share
            $used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
            $nl = @('J', 'I', 'H', 'G') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
            $o = ''; $rc = 1
            if ($nl) {
                $eap = $ErrorActionPreference
                try {
                    $ErrorActionPreference = 'Continue'                   # net's stderr isn't a throw
                    $o = (& net.exe use "${nl}:" '\\Mac\win' /persistent:no 2>&1 | Out-String).Trim()
                    $rc = $LASTEXITCODE
                } finally { $ErrorActionPreference = $eap }
            }
            if (-not $nl -or $rc -ne 0) {
                $why = if ($nl) { "net use ${nl}: \\Mac\win failed (exit $rc): $o" } else { 'no free drive letter (J, I, H, G)' }
                if ($Group -eq 'network') { Skip "network: $why" }
                Missing "network: not run: $why"
                continue
            }
            try {
                $dt = [string]([IO.DriveInfo]"${nl}:\").DriveType
                Check ($dt -eq 'Network') "network: ${nl}: is \\Mac\win mapped as a network drive (DriveType $dt)"
                $id = '0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F'
                Invoke-BridgeGroup 'network' @('run', $m, 'a', 'sleep') 0 "cache_dir=`"${nl}:\rbtest-bw3\RB Stems Plus Cache.noindex`"`ncache_id=$id`n" @{ RBSTEMS_HARNESS_CACHE = 'C:\rbtest\bridge-bw3\none' }
                $got = Get-RunAnswers
                Check ($got -eq 'a model, stored 0') "network: the model runs, nothing stored (got: $got; want: a model, stored 0)"
                Check (($script:BridgeState -contains 'state=unavailable') -and ($script:BridgeState -contains 'reason=not-local')) "network: bridge-state says unavailable, reason not-local (got: $($script:BridgeState -join ' '))"
                Check ($script:BridgeLog.Contains("the drive isn't connected to this Mac")) 'network: the log says the location is unavailable (not-local)'
                Check (-not (Test-Path -LiteralPath "${nl}:\rbtest-bw3")) "network: nothing made on the share (${nl}:\rbtest-bw3)"
            } finally {
                $eap = $ErrorActionPreference
                try {
                    $ErrorActionPreference = 'Continue'
                    $o = (& net.exe use "${nl}:" /delete /y 2>&1 | Out-String).Trim()
                    if ($LASTEXITCODE -ne 0) { [Console]::Out.WriteLine("  net use ${nl}: /delete: $o") }
                } finally { $ErrorActionPreference = $eap }
            }
            continue
        }
        # gpu (review BW1): a chunk run once and cached, then run marked as GPU memory (its mix, then
        # a pre-allocated x): passed through ("model"), and nothing stored; unmarked again, a hit
        Invoke-BridgeGroup 'gpu' @('run', $m, 'a', 'wait', 'gpu=in', 'a', 'd', 'gpu=out', 'a', 'd', 'sleep', 'gpu=off', 'a', 'd', 'wait') 0
        $got = Get-RunAnswers
        $want = 'a model, stored 1, a model, d model, a model, d model, stored 1, a cache, d model, stored 2'
        Check ($got -eq $want) "gpu: a marked run is passed through and never stored (got: $got; want: $want)"
        Check ($script:BridgeLog.Contains('outside CPU memory (DirectML): passed through, not cached')) 'gpu: the log says a run with non-CPU tensors was passed through'
    }
}
# the last harness may still hold the copy for a moment as it ends (gpu's): tried for up to 15 s
for ($i = 0; $i -lt 30 -and (Test-Path -LiteralPath $BridgeRealPath); $i++) {
    try { Remove-Item -LiteralPath $BridgeRealPath -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
}
Check (-not (Test-Path -LiteralPath $BridgeRealPath)) "the copied real library is removed ($BridgeRealPath)"
if ($standalone) { Done }
