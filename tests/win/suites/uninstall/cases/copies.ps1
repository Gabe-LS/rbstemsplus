# Round 3: the stub and the core DLL copied elsewhere and started with --after-exit, as the
# uninstall's own copy is (app.md 5.9): in a temp folder of another name (15 hex: the stub's folder
# check, exit 10), outside %TEMP% in a well-named folder (the core's parent check, exit 1), and in
# a well-named temp folder that is a junction (the core's reparse check, exit 1). Each exits with
# exactly that code (handle 1 makes a broken guard's wait fail: exit 2, never a delete) and deletes
# nothing: the apps folder's tree and the records as they were. The case folders and the junction
# go at the end.
$l = Get-Layout
if (-not (Test-Path -LiteralPath $l.Stub)) { Skip 'not installed (the install step failed or was skipped)' }
Stop-App
$ver = (Get-Content -Raw -LiteralPath $l.Current).Trim()
$arch = if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$stub = Join-Path $l.Apps "$ver\stub\$arch\RB Stems Plus.exe"
$dll = Join-Path $l.Apps "$ver\$arch\rbsp-core.dll"
if (-not (Test-Path -LiteralPath $stub) -or -not (Test-Path -LiteralPath $dll)) { Skip "no stub or core DLL to copy ($stub, $dll)" }
$tmp = Join-Path $l.Local 'Temp'
$hex = -join ((1..16) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
$elsewhere = Join-Path $CaseDir "rbsp-uninstall-$hex"
$jtarget = Join-Path $CaseDir 'copy-junction-target'
$cases = @(
    @{ What = 'a temp folder of another name'; Dir = (Join-Path $tmp "rbsp-uninstall-$($hex.Substring(1))"); Name = 'other.exe'; Code = 10 },
    @{ What = 'outside %TEMP%'; Dir = $elsewhere; Name = 'RB Stems Plus.exe'; Code = 1 },
    @{ What = 'a temp folder that is a junction'; Dir = (Join-Path $tmp "rbsp-uninstall-$($hex.Substring(1))0"); Name = 'RB Stems Plus.exe'; Junction = $jtarget; Code = 1 })
$records = { @((Get-AppTree), (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus' -ErrorAction SilentlyContinue | Out-String), (Test-Path -LiteralPath $l.Shortcut)) -join '|' }
foreach ($c in $cases) {
    if ($c.Junction) {
        New-Item -ItemType Directory -Force $c.Junction | Out-Null
        New-Item -ItemType Junction -Path $c.Dir -Target $c.Junction | Out-Null
    } else { New-Item -ItemType Directory -Force $c.Dir | Out-Null }
    $exe = Join-Path $c.Dir $c.Name
    Copy-Item -LiteralPath $stub -Destination $exe -Force
    Copy-Item -LiteralPath $dll -Destination (Join-Path $c.Dir 'rbsp-core.dll') -Force
    $before = & $records
    $p = Start-Process -FilePath $exe -ArgumentList '--after-exit', '1', "`"$($l.Local)`"" -PassThru -WindowStyle Hidden
    $null = $p.Handle   # keeps its exit code readable
    $done = $p.WaitForExit(30000)
    $code = if ($done) { $p.ExitCode } else { $null }
    Check ($done -and $null -ne $code -and $code -eq $c.Code) "$($c.What): refused with exit $($c.Code) ($($exe), exit $(if ($done) { $code } else { 'still running' }))"
    Check ((& $records) -ceq $before) "$($c.What): nothing deleted"
    if (-not $done) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $null = $p.WaitForExit(5000) }
}
foreach ($c in $cases) {
    if ($c.Junction) {
        if ([IO.Directory]::Exists($c.Dir)) { [IO.Directory]::Delete($c.Dir) }   # the junction itself, never its target
        Remove-Item -LiteralPath $c.Junction -Recurse -Force -ErrorAction SilentlyContinue
    } else { Remove-Item -LiteralPath $c.Dir -Recurse -Force -ErrorAction SilentlyContinue }
}
Check (-not (Test-Path -LiteralPath $cases[0].Dir) -and -not (Test-Path -LiteralPath $cases[2].Dir)) 'the case folders and the temp junction removed'
Done
