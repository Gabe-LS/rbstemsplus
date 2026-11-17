# The first release installed by the one-liner from dist-test/current/ (switch-a), then the app
# closed again
$l = Get-Layout
$out = Invoke-OneLiner ($VmRun.Url + 'current/')
"one-liner: $out"
$p = Get-Content -Raw -LiteralPath (Share 'dist-test/a/payload.json') | ConvertFrom-Json
Check ($out -match "RB Stems Plus $([regex]::Escape($p.payload_version)) is installed\.") "release a ($($p.payload_version)) installed"
Check ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $p.windows.app.version) "current names its app ($($p.windows.app.version))"
# the welcome first (startup_flow asks it before check_for_update): answered, so carried.ps1 and
# update.ps1 see the start's update check
Complete-Welcome 90
Stop-App
Done
