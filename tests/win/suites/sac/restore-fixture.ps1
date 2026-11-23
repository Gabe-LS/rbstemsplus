# As SYSTEM with SAC On: build the worker's saved-set layout from the snapshot's actual Pioneer
# DLL. A foreign Microsoft DLL stands in for the live bridge, which is also unsigned by Pioneer.
# No product exe runs, and a snapshot with an existing worker root is left alone.
$pf = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion').ProgramW6432Dir
if (-not $pf) { throw 'ProgramW6432Dir is missing' }
$root = Join-Path $pf 'RB Stems Plus'
$live = Join-Path $pf 'rekordbox\rekordbox 7.2.19\onnxruntime.dll'
if (-not (Test-Path -LiteralPath $live -PathType Leaf)) { Skip 'needs the SAC On snapshot with rekordbox 7.2.19 installed' }
if (Test-Path -LiteralPath $root) { Skip 'needs a snapshot without an existing Stems Cache root' }
if (Get-Process -Name rekordbox, 'Upmgr rekordbox', 'Uninstall rekordbox' -ErrorAction SilentlyContinue) {
    Skip 'needs a snapshot with rekordbox and its installers closed'
}
Check ((Get-SacState) -eq 1) 'SAC is on during fixture setup'
$sig = Get-AuthenticodeSignature -LiteralPath $live
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch '\bO=AlphaTheta Corporation(?:,|$)') {
    throw 'The snapshot needs the original AlphaTheta onnxruntime.dll'
}
$save = Join-Path $root 'pioneer\7.2.19'
New-Item -ItemType Directory -Path $save -Force | Out-Null
$pioneer = (Get-FileHash -LiteralPath $live).Hash.ToLowerInvariant()
$saved = Join-Path $save 'onnxruntime.dll'
Copy-Item -LiteralPath $live -Destination $saved
Set-Content -LiteralPath (Join-Path $save 'SHA256SUMS') -Value "$pioneer  onnxruntime.dll`n" -Encoding Ascii -NoNewline
$foreign = Join-Path $env:WINDIR 'System32\version.dll'
Copy-Item -LiteralPath $foreign -Destination $live -Force
$foreignSha = (Get-FileHash -LiteralPath $foreign).Hash.ToLowerInvariant()
Check ((Get-FileHash -LiteralPath $saved).Hash -eq $pioneer) "the saved DLL is Pioneer's ($pioneer)"
Check ($foreignSha -ne $pioneer -and (Get-FileHash -LiteralPath $live).Hash -eq $foreignSha) 'the live DLL is the foreign fixture'
Done
