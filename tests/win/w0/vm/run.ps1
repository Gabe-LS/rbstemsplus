# W0 (as the logged-in user, through winps.sh --user): the plat_shim test for both architectures,
# then the WPF spike: AnyCPU (native arm64 on ARM64 Windows) and the x64-forced build (x64 under
# emulation), each over the test DLLs, then each over the release DLLs with --startup.
# $runs (optional, default 1): how many times each spike runs.
if (-not $runs) { $runs = 1 }
$base = "$env:LOCALAPPDATA\Programs\RB Stems Plus W0"
$fail = 0
foreach ($a in "arm64", "x64") {
  "== shimtest-$a"
  & "$base\app\shimtest-$a.exe"
  "exit $LASTEXITCODE"
  if ($LASTEXITCODE -ne 0) { $fail++ }
}
foreach ($k in "app", "app-release") {
  foreach ($exe in "RB Stems Plus W0.exe", "RB Stems Plus W0 x64.exe") {
    for ($i = 1; $i -le $runs; $i++) {
      $res = "$env:TEMP\w0-$k-$($exe -replace ' ', '_')-$i.txt"
      Remove-Item $res -ErrorAction SilentlyContinue
      $argl = @("`"$res`"")
      if ($k -eq "app-release") { $argl += "--startup" }
      "== $k\$exe run $i"
      $sw = [Diagnostics.Stopwatch]::StartNew()
      $p = Start-Process -FilePath "$base\$k\$exe" -ArgumentList $argl -PassThru
      if (-not $p.WaitForExit(180000)) { "TIMEOUT: killed"; $p.Kill(); $fail++ }
      "exit $($p.ExitCode) after $([int]$sw.Elapsed.TotalMilliseconds) ms (wall, Start-Process to exit)"
      if ($p.ExitCode -ne 0) { $fail++ }
      if (Test-Path $res) {
        # the summary here; the full results in the share (\\Mac\win\w0\results)
        Get-Content $res -Encoding UTF8 | Select-String -Pattern '^(W0 spike|FAIL|INFO process)' | ForEach-Object { $_.Line }
        New-Item -ItemType Directory -Force \\Mac\win\w0\results | Out-Null
        Copy-Item $res \\Mac\win\w0\results\ -Force
      } else { "no result file"; $fail++ }
    }
  }
}
"== W0 total failures: $fail"
exit $fail
