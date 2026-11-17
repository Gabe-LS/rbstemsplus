# As SYSTEM: the release the TEST app sees, dist-test/current/, made a copy of dist-test/<name>/
# (the step's name after "switch-"); "switch-clean" deletes current/ (the VM's own files)
$name = $VmRun.Step -replace '^switch-', ''
$cur = Share 'dist-test/current'
if (Test-Path -LiteralPath $cur) { Remove-Item -LiteralPath $cur -Recurse -Force }
if ($name -eq 'clean') { Check (-not (Test-Path -LiteralPath $cur)) 'dist-test/current removed'; Done }
$src = Need "dist-test/$name/payload.json" "the release $name"
Copy-Item -LiteralPath (Split-Path $src) -Destination $cur -Recurse -Force
Check (Test-Path -LiteralPath (Join-Path $cur 'payload.json')) "dist-test/current is $name"
"payload_version: $((Get-Content -Raw -LiteralPath (Join-Path $cur 'payload.json') | ConvertFrom-Json).payload_version)"
Done
