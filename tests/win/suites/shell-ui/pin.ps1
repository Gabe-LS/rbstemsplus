# UP2. Pin the running app through Explorer, close it, and launch the same version
# through the persisted pin. The next step switches the served release.
$l = Get-Layout
$old = (Get-Content -Raw -LiteralPath $l.Current).Trim()
Expect ($VmRun.Step -match '^pin-(native|x64)-[0-9]+$') 'the pin step names its stub variant'
$arch = if ($Matches[1] -eq 'x64') { 'x64' } else { @(Get-TestArchs)[0] }
Expect (-not (Get-AppWindow)) 'install closed the old release'
Select-ShellStub $l $old $arch
Assert-ShellDesktop
[void](Start-Process -FilePath $l.Stub)
Wait-ShellApp
Test-ShellGroup
Use-ShellUi taskbar pin $ShellName --timeout 20
Close-ShellApp $script:ShellApp
Test-PinnedStub $l.Stub
Open-ShellPin
Close-ShellApp $script:ShellApp
Done
