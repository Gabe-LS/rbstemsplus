# As SYSTEM: stick A (NTFS, label RBSTICK), stick B (exFAT, no label) and stick D (NTFS, no
# label), each a 64 MB VHDX with a free drive letter; their letters and volume GUID paths saved
# for the user steps
New-Item -ItemType Directory -Force $LocDir | Out-Null
$la = Get-FreeLetter @('S', 'R', 'P', 'O')
$ga = Stick 'A' 'ntfs' 'RBSTICK' $la
$lb = Get-FreeLetter @('T', 'V', 'N', 'M')
$gb = Stick 'B' 'exfat' '' $lb
$ld = Get-FreeLetter @('U', 'L', 'K', 'J')
$gd = Stick 'D' 'ntfs' '' $ld
# the users may write the NTFS sticks' roots (a fresh NTFS root gives Users no write)
foreach ($l in $la, $ld) {
    & icacls.exe "${l}:\" /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
    Require-Stick ($LASTEXITCODE -eq 0) "users may write ${l}: (exit $LASTEXITCODE)"
}
Save-Sticks @{ A = $la; GA = $ga; B = $lb; GB = $gb; D = $ld; GD = $gd; SystemGuid = (Get-LetterGuid 'C') }
Done
