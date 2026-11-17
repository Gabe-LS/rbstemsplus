# The Windows driver's volume-arrival helper (app.md §7, W10): a VHDX attached and detached, so
# Windows sends the app WM_DEVICECHANGE's volume arrival and removal (DBT_DEVICEARRIVAL and
# DBT_DEVICEREMOVECOMPLETE with a drive letter), as the Mac's scenarios use tests/vm-usb.swift's USB
# stick (2 GB, RBSPSTICK: the same size, so cache-paused crosses the core's disk/10 floor, and the
# same label, so the texts naming the drive match). Windows' own diskpart and Storage cmdlets only:
# nothing is downloaded, no Hyper-V module needed. Run elevated (diskpart attaches a virtual disk
# only for an administrator: the VM's job runs it as SYSTEM).
#
#   volume.ps1 create  -Path C:\rbtest\stick.vhdx [-SizeMB 2048] [-Label RBSPSTICK] [-Letter R]
#                      made once, NTFS, given its letter (Mount Manager keeps it), detached
#   volume.ps1 attach  -Path C:\rbtest\stick.vhdx [-Letter R]    arrival; prints the drive root
#   volume.ps1 detach  -Path C:\rbtest\stick.vhdx                removal
#   volume.ps1 fill    -Path C:\rbtest\stick.vhdx -LeaveMB 4     the filler first removed, then a new
#                      one leaving LeaveMB free (cache-paused); -LeaveMB 0 only removes it (fill_stick)
#
# The volume is always the one this VHDX holds (Get-DiskImage -ImagePath, its disk, its partition),
# never one found by label. Exit 0, or 1 with the reason on stderr (diskpart's output when a step
# failed); a failed create or attach detaches what it attached (a failed create also deletes its
# VHDX), keeping the first error. Untested until the VM's W10 round.
param(
    [Parameter(Mandatory = $true, Position = 0)][ValidateSet('create', 'attach', 'detach', 'fill')][string]$Verb,
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$SizeMB = 2048,
    [string]$Label = 'RBSPSTICK',
    [string]$Letter = 'R',
    [int]$LeaveMB = 4
)
$ErrorActionPreference = 'Stop'

function Fail([string]$Text) {
    [Console]::Error.WriteLine($Text)
    exit 1
}

# diskpart with these lines as its script: @{ ok; out } (out: everything it printed)
function Invoke-DiskPart([string[]]$Lines) {
    $ErrorActionPreference = 'Continue'   # its stderr, if any, is output, not a terminating error
    $script = [IO.Path]::Combine($env:TEMP, "rbsp-volume-$PID.txt")
    [IO.File]::WriteAllLines($script, $Lines, [Text.Encoding]::ASCII)
    try {
        $out = & "$env:SystemRoot\System32\diskpart.exe" /s $script 2>&1 | Out-String
        return @{ ok = ($LASTEXITCODE -eq 0); out = $out }
    }
    finally { Remove-Item -LiteralPath $script -Force -ErrorAction SilentlyContinue }
}

# after a failure: detach whatever is attached, best effort (its own failure is not the error shown)
function Undo-Attach {
    Invoke-DiskPart @("select vdisk file=""$full""", "detach vdisk") | Out-Null
}

# this VHDX's image (Attached: whether it is), or $null (no such file)
function Get-Image {
    try { return Get-DiskImage -ImagePath $full -ErrorAction SilentlyContinue } catch { return $null }
}

# the volume on this VHDX's one partition, or $null (not attached, not ready, not one partition)
function Get-StickVolume {
    $img = Get-Image
    if ($null -eq $img -or -not $img.Attached) { return $null }
    try {
        $disk = $img | Get-Disk -ErrorAction SilentlyContinue
        if ($null -eq $disk) { return $null }
        $parts = @($disk | Get-Partition -ErrorAction SilentlyContinue | Where-Object { $_.Type -ne 'Reserved' })
        if ($parts.Count -ne 1) { return $null }
        return $parts[0] | Get-Volume -ErrorAction SilentlyContinue
    }
    catch { return $null }
}

# a volume's drive letter, upper case, or "" (none: Storage gives a null or NUL character then)
function Get-Letter($v) {
    if ($null -eq $v) { return '' }
    $l = "$($v.DriveLetter)"
    if ($l -match '^[A-Za-z]$') { return $l.ToUpper() }
    return ''
}

# after an attach: the volume once it has a letter (Mount Manager's, a moment after the volume),
# or the volume without one after 5 s, or $null
function Wait-StickVolume {
    $seen = $null
    for ($i = 0; $i -lt 50; $i++) {
        $v = Get-StickVolume
        if ((Get-Letter $v) -ne '') { return $v }
        if ($null -ne $v) { $seen = $v }
        Start-Sleep -Milliseconds 100
    }
    return $seen
}

$full = [IO.Path]::GetFullPath($Path)
if ($full.Contains('"')) { Fail "volume.ps1: a path with a quote: refused" }
# diskpart's script is ASCII: a character outside it would name another file
if ($full -match '[^\x20-\x7E]') { Fail "volume.ps1: a path with a character outside ASCII: refused" }
if ($Letter -notmatch '^[D-Z]$') { Fail "volume.ps1: -Letter is one letter, D to Z" }
if ($Label -notmatch '^[A-Za-z0-9_]{1,11}$') { Fail "volume.ps1: -Label is up to 11 letters, digits or _" }

switch ($Verb) {
    'create' {
        if (Test-Path -LiteralPath $full) { Fail "volume.ps1: $full exists already" }
        if (Test-Path -LiteralPath "${Letter}:\") { Fail "volume.ps1: ${Letter}: is taken" }
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($full)) | Out-Null
        # the letter assigned here, so every attach finds it kept: one arrival, never a move
        $r = Invoke-DiskPart @(
            "create vdisk file=""$full"" maximum=$SizeMB type=expandable",
            "select vdisk file=""$full""",
            "attach vdisk",
            "create partition primary",
            "format fs=ntfs quick label=$Label",
            "assign letter=$Letter",
            "detach vdisk"
        )
        if (-not $r.ok) {
            Undo-Attach
            Remove-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue
            Fail $r.out
        }
        Write-Output "created $full ($SizeMB MB, NTFS, $Label, ${Letter}:)"
    }
    'attach' {
        if (-not (Test-Path -LiteralPath $full)) { Fail "volume.ps1: no $full (create it first)" }
        $img = Get-Image
        if ($null -ne $img -and $img.Attached) { Fail "volume.ps1: $full is attached already" }
        if (Test-Path -LiteralPath "${Letter}:\") { Fail "volume.ps1: ${Letter}: is taken" }
        $r = Invoke-DiskPart @("select vdisk file=""$full""", "attach vdisk")
        if (-not $r.ok) { Undo-Attach; Fail $r.out }
        $v = Wait-StickVolume
        if ($null -eq $v) { Undo-Attach; Fail "volume.ps1: $full attached, but its volume didn't appear" }
        # Mount Manager gives back the letter create assigned: assign only when it didn't
        if ((Get-Letter $v) -ne $Letter.ToUpper()) {
            $r = Invoke-DiskPart @("select vdisk file=""$full""", "select partition 1", "assign letter=$Letter")
            # refused, but the letter came meanwhile (Mount Manager's late): that is the letter wanted
            if (-not $r.ok -and (Get-Letter (Get-StickVolume)) -ne $Letter.ToUpper()) { Undo-Attach; Fail $r.out }
        }
        for ($i = 0; $i -lt 50 -and -not (Test-Path -LiteralPath "${Letter}:\"); $i++) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath "${Letter}:\")) { Undo-Attach; Fail "volume.ps1: ${Letter}:\ didn't appear" }
        Write-Output "${Letter}:\"
    }
    'detach' {
        $r = Invoke-DiskPart @("select vdisk file=""$full""", "detach vdisk")
        if (-not $r.ok) { Fail $r.out }
        Write-Output "detached $full"
    }
    'fill' {
        if ($LeaveMB -lt 0) { Fail "volume.ps1: -LeaveMB is 0 or more" }
        $drive = Get-Letter (Get-StickVolume)
        if ($drive -eq '') { Fail "volume.ps1: $full isn't attached with a drive letter" }
        $file = "${drive}:\fill.bin"
        # the old filler out first, and the free space read after it (never the stale figure)
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $file) { Fail "volume.ps1: $file can't be removed" }
        if ($LeaveMB -eq 0) { Write-Output "removed the filler from ${drive}:"; exit 0 }
        $free = (Get-Volume -DriveLetter $drive).SizeRemaining - [int64]$LeaveMB * 1MB
        if ($free -le 0) { Write-Output "volume.ps1: ${drive}: has $LeaveMB MB free or less already"; exit 0 }
        $f = [IO.File]::Create($file)
        try { $f.SetLength($free) } finally { $f.Dispose() }
        Write-Output "filled ${drive}: to $LeaveMB MB free"
    }
}
exit 0
