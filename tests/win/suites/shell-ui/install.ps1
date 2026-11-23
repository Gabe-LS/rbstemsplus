# Both releases are installed via dist-test/current, as in update.suite.
Assert-ShellDesktop
$l = Get-Layout
Expect (-not (Test-Path -LiteralPath $l.Apps)) 'the previous round left a clean per-user install'
$p = Get-Content -Raw -LiteralPath (Share 'dist-test/current/payload.json') | ConvertFrom-Json
$out = Invoke-OneLiner ($VmRun.Url + 'current/')
"one-liner: $out"
Assert-ShellDesktop
Expect ($out -match "RB Stems Plus $([regex]::Escape($p.payload_version)) is installed\.") "release $($p.payload_version) installed"
Expect ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $p.windows.app.version) "current names $($p.windows.app.version)"
Wait-ShellApp
Use-ShellUi invoke --pid $script:ShellApp.Id (Get-CoreText 'T_CONTINUE') --type Button --timeout 60
Expect (Wait-Until { Test-Path -LiteralPath (Join-Path $l.Support 'welcomed') } 30) 'the welcome was recorded'
Close-ShellApp $script:ShellApp
Done
