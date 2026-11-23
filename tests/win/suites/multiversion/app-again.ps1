# As the user, after V12: RB Stems Plus installed again by the one-liner (for V10), the welcome
# answered when it shows; Stems Cache is off (the choice was cleared) and Manage shows both
# versions (7.2.19 Ready to install).
$l = Get-Layout
$out = Invoke-OneLiner $VmRun.Url
"one-liner: $out"
Check ($out -match 'RB Stems Plus [0-9]+\.[0-9]+\.[0-9]+ is installed\.\s*$') 'installed again'
$script:app = $null
Check (Wait-Until { $script:app = Get-AppWindow; $null -ne $script:app } 90) 'the app opened (its window)'
if (-not $script:app) { Done }
if (Invoke-UiNamed $script:app.Id (Get-CoreText 'T_CONTINUE') 15 'Button') { 'the welcome answered: Continue' }
Clear-MvOffer $script:app.Id 10
Check (-not (Test-CacheOn)) 'Stems Cache is off'
# the one-liner installs the served root's M1, so 7.2.20 reads Unsupported until the next fetch
# (v10i's Install downloads M2 first)
Test-MvRows $script:app.Id @((Rx (Get-RowText 'T_WIN_ROW_READY' '7.2.19')), ('^(' + [regex]::Escape((Get-RowText 'T_WIN_ROW_READY' '7.2.20')) + '|' + [regex]::Escape((Get-RowText 'T_WIN_ROW_UNSUPPORTED' '7.2.20')) + ')$'), (Rx (Get-SummaryText 0 2))) 120
Save-MvShot 'ready'
Done
