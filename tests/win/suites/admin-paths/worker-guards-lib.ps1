$GuardFixture = Join-Path $WorkerTraceDir 'fixture.json'
$GuardReady = Join-Path $WorkerTraceDir 'hold.ready'
$GuardStop = Join-Path $WorkerTraceDir 'hold.stop'
$GuardDllNames = 'version.dll', 'bcrypt.dll', 'wintrust.dll', 'ucrtbase.dll'
