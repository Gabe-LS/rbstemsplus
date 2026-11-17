# As SYSTEM: an exFAT VHDX (64 MB) attached with a drive letter, for plat-win-test's fat mode
# (MQ4) and the drive cases; made again from scratch each run. Its letter goes to
# C:\rbtest\vhd\exfat.letter for the user steps.
$dir = 'C:\rbtest\vhd'
New-Item -ItemType Directory -Force $dir | Out-Null
$vhd = "$dir\exfat.vhdx"
# diskpart with a script: its output straight to this step's stdout (not the pipeline, so the
# caller gets the exit code alone)
function DiskPart([string]$script) {
    $f = "$dir\dp.txt"
    Set-Content -LiteralPath $f -Value $script -Encoding ASCII
    $o = & diskpart.exe /s $f 2>&1 | Out-String
    $rc = [int]$LASTEXITCODE
    [Console]::Out.WriteLine($o)
    return $rc
}
if (Test-Path -LiteralPath $vhd) { [void](DiskPart "select vdisk file=`"$vhd`"`ndetach vdisk"); Remove-Item -LiteralPath $vhd -Force }
$used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
$letter = @('S', 'T', 'U', 'V', 'W', 'R', 'Q') | Where-Object { $used -notcontains $_ } | Select-Object -First 1
if (-not $letter) { Skip 'no free drive letter for the VHDX' }
$rc = DiskPart "create vdisk file=`"$vhd`" maximum=64 type=expandable`nselect vdisk file=`"$vhd`"`nattach vdisk`ncreate partition primary`nformat fs=exfat quick label=RBEXFAT`nassign letter=$letter"
Check ($rc -eq 0) "diskpart made and attached the exFAT VHDX (exit $rc)"
Check (Wait-Until { Test-Path "${letter}:\" } 30) "it is ${letter}:"
$fs = (Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue).FileSystem
Check ($fs -eq 'exFAT') "its file system is exFAT ($fs)"
Set-Content -LiteralPath "$dir\exfat.letter" -Value $letter -Encoding ASCII
Done
