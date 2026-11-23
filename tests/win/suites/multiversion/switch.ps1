# As SYSTEM: the release the TEST app sees, dist-mv/current/, made a copy of dist-mv/<m1|m2>/
# (the step's name after "switch-"); "switch-clean" deletes current/ (the VM's own files: the Mac
# can't delete what the VM wrote in the share). A run that stopped before switch-clean leaves
# current/ there: delete it from the VM (Remove-Item -Recurse -Force '\\Mac\win\dist-mv\current').
$name = $VmRun.Step -replace '^switch-', ''
$cur = Share "$MvServe/current"
if (Test-Path -LiteralPath $cur) { Remove-Item -LiteralPath $cur -Recurse -Force }
if ($name -eq 'clean') { Check (-not (Test-Path -LiteralPath $cur)) "$MvServe/current removed"; Done }
$src = Need "$MvServe/$name/payload.json" "the manifest $name"
Copy-Item -LiteralPath (Split-Path $src) -Destination $cur -Recurse -Force
$j = Get-Content -Raw -LiteralPath (Join-Path $cur 'payload.json') | ConvertFrom-Json
Check ($null -ne $j) "$MvServe/current is ${name}: payload_version $($j.payload_version), windows list $(@($j.windows.compatible.rekordbox) -join ', ')"
Done
