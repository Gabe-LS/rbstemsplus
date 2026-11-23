# As SYSTEM, after unplugged.ps1: stick C (NTFS, label OTHER) given A's old letter, with a folder of
# our name at A's place and no marker in it
$st = Read-Sticks
$gc = Stick 'C' 'ntfs' 'OTHER' $st.A
Require-Stick ($gc -and $gc -ne $st.GA -and (Get-LetterGuid $st.A) -eq $gc) "stick C has its own GUID path ($gc) at $($st.A):"
& icacls.exe "$($st.A):\" /grant '*S-1-5-32-545:(OI)(CI)M' | Out-Null
Require-Stick ($LASTEXITCODE -eq 0) "users may write $($st.A): (exit $LASTEXITCODE)"
foreach ($a in 'x64', 'arm64') { New-Item -ItemType Directory -Force "$($st.A):\Music-$a\$CacheName" | Out-Null }
$h = @{}; foreach ($p in $st.PSObject.Properties) { $h[$p.Name] = $p.Value }
$h.GC = $gc
Save-Sticks $h
Done
