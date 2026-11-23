# UP3: the previous stub survives the swap as .old until the next good start.
$l = Get-Layout
$arch = Get-NativeUpdateArch
$oldHash = (Get-UpdateRelease 'a').windows.app.stub_sha256.$arch
$newRelease = Get-UpdateRelease 'new'
$newHash = $newRelease.windows.app.stub_sha256.$arch
Expect ($oldHash -match '^[a-f0-9]{64}$' -and $newHash -match '^[a-f0-9]{64}$') "both fixtures name a native $arch stub hash"
if ($oldHash -eq $newHash) { Skip "dist-test/a and new have the same $arch stub hash; use a changed-stub fixture" }

Expect ((Get-FileHash -LiteralPath $l.Stub).Hash -eq $newHash) 'the stable stub path holds the new release stub'
$backup = $l.Stub + '.old'
Expect ((Test-Path -LiteralPath $backup -PathType Leaf) -and (Get-FileHash -LiteralPath $backup).Hash -eq $oldHash) 'the .old backup is the previous release stub'
Check (-not (Test-Path -LiteralPath ($l.Stub + '.new'))) 'the staged .new stub was consumed'
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
Check (Wait-Until { -not (Test-Path -LiteralPath $backup) } 60) 'the next successful start removed the .old stub'
Check ((Get-FileHash -LiteralPath $l.Stub).Hash -eq $newHash) 'the new stub is still in place'
Stop-App
Done
