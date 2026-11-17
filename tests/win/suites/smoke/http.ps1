# The Mac's HTTP server, reached from the VM: payload.json served, with its SHA-256 for the log
# (tests/win/first-run.sh's step 4 compares it with the share's)
#fake: sha payload.json
$r = Invoke-WebRequest -UseBasicParsing -Uri ($VmRun.Url + 'payload.json') -TimeoutSec 60
"GET payload.json: $($r.StatusCode), $($r.RawContentLength) bytes"
$sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($r.RawContentStream.ToArray())).Replace('-', '').ToLowerInvariant()
"sha256 $sha"
if ($r.StatusCode -ne 200) { exit 1 }
exit 0
