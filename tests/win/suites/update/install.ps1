# Install a/ via current/, or install carried/ directly after the sticky uninstall lap.
$l = Get-Layout
$name = if ($VmRun.Step -eq 'install-carried') { 'carried' } else { 'a' }
$dir = if ($name -eq 'carried') { 'carried' } else { 'current' }
$p = Get-UpdateRelease $name
$out = Invoke-OneLiner ($VmRun.Url + "$dir/")
"one-liner: $out"
Expect ($out -match "RB Stems Plus $([regex]::Escape($p.payload_version)) is installed\.") "release $name ($($p.payload_version)) installed"
Expect ((Get-Content -Raw -LiteralPath $l.Current).Trim() -eq $p.windows.app.version) "current names its app ($($p.windows.app.version))"
# The welcome precedes the update check. It is an owned window, a child of the main window in the
# UIA tree, so rbui's --title (top-level windows only) can't name it: the app's pid does.
Wait-UpdateApp
Use-UpdateUi invoke --pid $UpdateApp.Id (Get-CoreText 'T_CONTINUE') --type Button --timeout 60
Check (Wait-Until { Test-Path -LiteralPath (Join-Path $l.Support 'welcomed') } 30) 'the welcome was recorded'
Stop-App
Done
