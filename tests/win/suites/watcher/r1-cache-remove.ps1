# Clear the installed cache fixture before the foreign-file and dismissal lap.
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from worker prep' }
Open-App
if (-not $script:app) { Done }
foreach ($id in 'T_BTN_UNINSTALL_CACHE', 'T_UNINSTALL') {
    $label = Get-CoreText $id
    $rc = Invoke-Rbui invoke --pid $script:app.Id $label --type Button --timeout 60
    Check ($rc -eq 0) "$label invoked on the app (rbui exit $rc)"
    if ($rc -ne 0) { Done }
}
Check (Wait-Until { (Get-Sha $RbOrt) -eq $f.PioneerSha } 180) "Pioneer's live DLL is back"
Check (Wait-UiEnabled $script:app.Id (Get-CoreText 'T_BTN_INSTALL_CACHE') 60) 'Stems Cache is off and the action finished'
Stop-App
Done
