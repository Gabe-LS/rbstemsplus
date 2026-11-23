$file = 'C:\rbtest\prepare-facts.txt'
if (-not (Test-Path -LiteralPath $file)) { Skip "needs the selected negative case in $file" }
$fixture = (Get-Content -LiteralPath $file -Raw).Trim()
if ($fixture -notin 'facts-speed', 'facts-unlisted', 'facts-entry') { throw "invalid Prepare fact fixture: $fixture" }
$VmRun.Step = $fixture
& (Need 'repo/tests/win/suites/prepare/facts.ps1' 'the Prepare fact step')
