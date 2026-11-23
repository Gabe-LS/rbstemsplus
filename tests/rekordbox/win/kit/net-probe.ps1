# The kit's network check, as the test account: the Mac's TEST server over HTTP (the one-liner's base) and
# HTTPS (the app's), each payload.json compared with the kit's copy. Writes nothing.
#   -Pinned : HTTPS checked against the kit's test-ca.cer by this script alone (no certificate
#             store touched): proves the server and the path before the CA is trusted
#   (none)  : HTTPS through Windows' own trust, as the app's WinHTTP does: passes only once the
#             test CA is in a Root store
#   -Server HOST:PORT : the Mac's TEST server (pc-session.sh passes git config rbstemsplus.pcTestServer)
param([Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9.-]+:[0-9]+$')][string]$Server, [switch]$Pinned, [ValidatePattern('^[A-Za-z0-9.-]+:[0-9]+$')][string]$BootstrapServer)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$manifest = if ($BootstrapServer) { 'm12\old.json' } else { 'dist\payload.json' }
$http = if ($BootstrapServer) { $BootstrapServer } else { $Server }
$https = if ($BootstrapServer) { "https://$Server/latest/download" } else { "https://$Server" }
$want = (Get-FileHash -LiteralPath (Join-Path $kit $manifest) -Algorithm SHA256).Hash
$fail = 0
function Get-Sha([byte[]]$b) { $s = [Security.Cryptography.SHA256]::Create(); try { return ([BitConverter]::ToString($s.ComputeHash($b)) -replace '-', '') } finally { $s.Dispose() } }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$wc = New-Object Net.WebClient
try { $h = Get-Sha ($wc.DownloadData("http://$http/payload.json")); "http  payload.json: $(if ($h -eq $want) { 'the kit''s' } else { $fail++; "DIFFERS ($h)" })" }
catch { $fail++; "http  FAILED: $($_.Exception.Message)" }
if ($Pinned) {
    if (-not (Test-Path -LiteralPath (Join-Path $kit 'dist\test-ca.cer'))) { 'BAD: no dist\test-ca.cer in the kit for -Pinned (stage --dist)'; exit 1 }
    $ca = New-Object Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $kit 'dist\test-ca.cer')
    $script:seen = ''
    [Net.ServicePointManager]::ServerCertificateValidationCallback = {
        param($s, $cert, $chain, $errors)
        $c = New-Object Security.Cryptography.X509Certificates.X509Chain
        $c.ChainPolicy.RevocationMode = 'NoCheck'
        $c.ChainPolicy.VerificationFlags = 'AllowUnknownCertificateAuthority'
        [void]$c.ChainPolicy.ExtraStore.Add($ca)
        $ok = $c.Build((New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cert))
        $root = $c.ChainElements[$c.ChainElements.Count - 1].Certificate.Thumbprint
        $script:seen = "chain built $ok, root $root, the kit's CA $($ca.Thumbprint), errors before pinning: $errors"
        return ($ok -and $root -eq $ca.Thumbprint)
    }
}
try { $h = Get-Sha ($wc.DownloadData("$https/payload.json")); "https payload.json: $(if ($h -eq $want) { 'the kit''s' } else { $fail++; "DIFFERS ($h)" })$(if ($Pinned) { " ($script:seen)" } else { ' (Windows trust)' })" }
catch { $fail++; "https FAILED$(if ($Pinned) { " ($script:seen)" } else { ' (Windows trust: expected until the test CA is trusted)' }): $($_.Exception.InnerException.Message)" }
finally { [Net.ServicePointManager]::ServerCertificateValidationCallback = $null }
if ($fail) { exit 1 }
