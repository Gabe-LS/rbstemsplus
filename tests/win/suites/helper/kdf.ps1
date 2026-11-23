# T13 / P-H5: CNG's PBKDF2 against SQLCipher's LibTomCrypt, byte for byte (never printed), and the
# indicative timings: the derivation alone, and a test database opened with each (the VM emulates
# x64: the real numbers come from the owner's PC)
$x = Get-LocalExe 'rbsp-win-library-test.exe'
$rc = Invoke-Exe $x.Exe @('--kdf-check')
Check ($rc -eq 0) "--kdf-check: known answers and 256,000 rounds, CNG and LibTomCrypt the same; kdf_cng_install 0 (exit $rc)"
$db = Join-Path $x.Dir 'test.db'
$rc = Invoke-Exe $x.Exe @('--make', $db)
Check ($rc -eq 0) "--make: a test database (a test key, not rekordbox's) (exit $rc)"
$rc = Invoke-Exe $x.Exe @('--open', $db)
Check ($rc -eq 0) "--open: LibTomCrypt's derivation (exit $rc)"
$rc = Invoke-Exe $x.Exe @('--open-alt', $db)
Check ($rc -eq 0) "--open-alt: SQLCipher derived through the CNG hook, no call fell back, the database opens (exit $rc)"
Done
