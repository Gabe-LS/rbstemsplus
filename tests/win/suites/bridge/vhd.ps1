# As SYSTEM: an NTFS VHDX (64 MB) attached with a drive letter, a local volume that isn't the
# system one, for bridge/win/tests/run.ps1's junction row (review BX6: NTFS, as a junction needs);
# made again from scratch each run. Its letter goes to C:\rbtest\vhd\bridge.letter for the user step,
# only once diskpart exited 0 and the letter is there, NTFS (review TR73); else no letter file, and
# the VHDX is detached again.
$dir = 'C:\rbtest\vhd'
New-Item -ItemType Directory -Force $dir | Out-Null
Remove-Item -LiteralPath "$dir\bridge.letter" -Force -ErrorAction SilentlyContinue
$vhd = "$dir\bridge.vhdx"
# diskpart with a script: its output straight to this step's stdout (not the pipeline, so the
# caller gets the exit code alone); its stderr isn't a throw
function DiskPart([string]$script) {
    $f = "$dir\dp-bridge.txt"
    Set-Content -LiteralPath $f -Value $script -Encoding ASCII
    $eap = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $o = & diskpart.exe /s $f 2>&1 | Out-String
        $rc = [int]$LASTEXITCODE
    } finally { $ErrorActionPreference = $eap }
    [Console]::Out.WriteLine($o)
    return $rc
}
if (Test-Path -LiteralPath $vhd) { [void](DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"); Remove-Item -LiteralPath $vhd -Force }
$used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
$letter = @('P', 'O', 'N', 'M', 'L', 'K') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
if (-not $letter) { Skip 'no free drive letter for the VHDX' }
$rc = DiskPart "create vdisk file=`"$vhd`" maximum=64 type=expandable`nselect vdisk file=`"$vhd`"`nattach vdisk`ncreate partition primary`nformat fs=ntfs quick label=RBBRIDGE`nassign letter=$letter"
Check ($rc -eq 0) "diskpart made and attached the NTFS VHDX (exit $rc)"
$up = ($rc -eq 0) -and (Wait-Until { Test-Path "${letter}:\" } 30)
Check $up "it is ${letter}:"
$fs = if ($up) { (Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue).FileSystem } else { '' }
Check ($fs -eq 'NTFS') "its file system is NTFS ($fs)"
if ($up -and $fs -eq 'NTFS') { Set-Content -LiteralPath "$dir\bridge.letter" -Value $letter -Encoding ASCII }
else { [void](DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk") }     # nothing left attached
Done
