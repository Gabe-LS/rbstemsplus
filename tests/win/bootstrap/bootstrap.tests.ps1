# scripts/bootstrap.ps1's tests in pwsh on the Mac (app.md W12, release.md T8): the file's rules
# (ASCII, no BOM, no exit), its texts against the core's, versions, the DER signature and ECDSA
# check with keys made here, plainJSONKeys, payload.json's windows block, the layout and the
# records the updater reads (current, installed-release\, payload\, payload-version), the unpacked
# app's checks on a zip made here (its entry names first, the version resource through $sys), and
# the install on a temp folder with a fake Windows ($sys).
#
#   pwsh -NoProfile -File tests/win/bootstrap/bootstrap.tests.ps1     (tests/win/bootstrap/run.sh)
#
# It also runs in Windows PowerShell 5.1 on Windows (.NET Framework), the one bootstrap.ps1 runs in:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\win\bootstrap\bootstrap.tests.ps1
# There the keys are exported and the signatures made without .NET 5's APIs, the facts are the
# machine's, and the one-liner isn't run (on a supported Windows it would install from GitHub).
$ErrorActionPreference = 'Stop'
$onWindows = $env:OS -eq 'Windows_NT'
$root = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..', '..', '..'))
$script:checks = 0
$script:failures = 0
function Check([bool]$cond, [string]$what) {
    $script:checks++
    if (-not $cond) { $script:failures++; Write-Host "FAIL $what" }
}

# ------------------------------------------------------------------ the file
$path = [IO.Path]::Combine($root, 'scripts', 'bootstrap.ps1')
$bytes = [IO.File]::ReadAllBytes($path)
$bad = @($bytes | Where-Object { ($_ -gt 0x7e) -or ($_ -lt 0x20 -and $_ -ne 0x0a -and $_ -ne 0x0d -and $_ -ne 0x09) })
Check ($bad.Count -eq 0) 'bootstrap.ps1 is pure ASCII'
Check (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf)) 'no BOM'
$src = [Text.Encoding]::ASCII.GetString($bytes)
Check (-not ($src -match '(?m)^\s*exit\b')) 'never exit (it would close the window under iex)'
$at = $src.IndexOf("`n& {`n")
Check ($at -gt 0) 'the whole script runs inside & { }'
Check ($src.TrimEnd().EndsWith('}')) 'and ends with it'

# the functions, without running the install (the block dot-sourced; it returns before Main)
$RbspTestMode = $true
. ([scriptblock]::Create($src.Substring(0, $at) + "`n. {`n" + $src.Substring($at + 5)))

# ------------------------------------------------------------------ texts
$defs = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'core', 'texts.def'))
function Get-CoreText([string]$id) {
    $m = [regex]::Match($defs, "TEXT\($id,\s*win,\s*`"`",\s*`"((?:[^`"\\]|\\.)*)`"\)")
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value.Replace('\"', '"')
}
Check ($Texts.SacOn -ceq (Get-CoreText 'T_WIN_SAC_ON')) "the Smart App Control refusal is the core's T_WIN_SAC_ON"
Check ($Texts.OldWindows -ceq (Get-CoreText 'T_WIN_START_OLD_WINDOWS')) "the old-Windows refusal is the core's T_WIN_START_OLD_WINDOWS"
Check ($Publisher -ceq 'Gabe-LS') 'the Apps & features Publisher (owner decided)'
$aumidH = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'win', 'c', 'aumid.h'))
Check ($aumidH -match "#define RBSP_AUMID_UTF8 `"$([regex]::Escape($Aumid))`"") 'the AppUserModelID is win/c/aumid.h''s'

# ------------------------------------------------------------------ versions
foreach ($v in @('1.4.0', '08.09.10', '123456789.1.1')) { Check (Test-RbspReleaseVersion $v) "a release version: $v" }
foreach ($v in @('', '1.4', 'v1.4.0', '1.4.0 ', '1234567890.1.1', '1.4.0-beta.1', "1.4.0`n", "1.$([char]0x0661).0")) {
    Check (-not (Test-RbspReleaseVersion $v)) "not a release version: '$v'"
}
$cmp = @(
    @('1.0.9', '1.0.10', -1), @('1.0.10', '1.0.9', 1), @('1.1.0', '1.1.0', 0), @('08.09.10', '8.9.10', 0),
    @('1.5.0', '1.4.9', 1), @('1.5.0-beta.2', '1.5.0', 2), @('1.5.0', '1.5.0-beta.2', 2), @('x', '1.0.0', 2), @('1.0.0', '', 2))
foreach ($c in $cmp) { Check ((Compare-RbspVersion $c[0] $c[1]) -eq $c[2]) "compare $($c[0]) $($c[1]) = $($c[2])" }

# ------------------------------------------------------------------ the signature
$curve = [System.Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256')
$k1 = [System.Security.Cryptography.ECDsa]::Create($curve)
$k2 = [System.Security.Cryptography.ECDsa]::Create($curve)
# a P-256 key's SubjectPublicKeyInfo (DER); .NET Framework has no ExportSubjectPublicKeyInfo
function Spki($key) {
    if ($key.PSObject.Methods['ExportSubjectPublicKeyInfo']) { return ,$key.ExportSubjectPublicKeyInfo() }
    $q = $key.ExportParameters($false).Q
    $head = [byte[]](0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                     0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04)
    return ,[byte[]]($head + $q.X + $q.Y)
}
$pub1 = [Convert]::ToBase64String((Spki $k1))
$pub2 = [Convert]::ToBase64String((Spki $k2))
# a DER INTEGER of a big-endian unsigned value: leading zeros dropped, one 0 back when the top bit is set
function DerInt([byte[]]$v) {
    $i = 0
    while ($i -lt $v.Length - 1 -and $v[$i] -eq 0) { $i++ }
    $b = [byte[]]$v[$i..($v.Length - 1)]
    if ($b[0] -ge 0x80) { $b = [byte[]](@(0) + $b) }
    return ,[byte[]](@(0x02, $b.Length) + $b)
}
function Sign($key, [byte[]]$data) {
    $fmt = 'System.Security.Cryptography.DSASignatureFormat' -as [type]
    if ($fmt) { return ,$key.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256, $fmt::Rfc3279DerSequence) }
    $rs = $key.SignData($data, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # r || s (IEEE P1363)
    $body = [byte[]]((DerInt $rs[0..31]) + (DerInt $rs[32..63]))
    return ,[byte[]](@(0x30, $body.Length) + $body)
}
$data = [Text.Encoding]::UTF8.GetBytes('{"payload_version": "1.5.0"}')
$s1 = Sign $k1 $data
Check ((Test-RbspPayloadSignature $data $s1 @($pub2, $pub1)) -eq 1) 'signed by the second key: 1'
Check ((Test-RbspPayloadSignature $data $s1 @($pub2)) -eq -1) 'another key: -1'
$other = [byte[]]$data.Clone(); $other[3] = 0x41
Check ((Test-RbspPayloadSignature $other $s1 @($pub1)) -eq -1) 'changed bytes: -1'
Check ((Test-RbspPayloadSignature $data ([byte[]]$s1[0..19]) @($pub1)) -eq -1) 'a truncated signature: -1'
Check ((Test-RbspPayloadSignature $data ([byte[]]@()) @($pub1)) -eq -1) 'an empty signature: -1'
Check ((Test-RbspPayloadSignature $data $s1 @('not base64!', 'AAAA', $pub1)) -eq 2) 'keys that are not P-256 SPKIs are skipped'
# the DER rules (sig.c's sig_der_to_rs)
for ($t = 0; $t -lt 20; $t++) {
    $s = Sign $k1 $data
    $rs = ConvertFrom-RbspDerSignature $s
    Check ($null -ne $rs -and $rs.Length -eq 64) "a real DER signature reads ($t)"
}
$r = [byte[]](@(0x01) * 32)
$okDer = [byte[]](@(0x30, 0x44, 0x02, 0x20) + $r + @(0x02, 0x20) + $r)
Check ($null -ne (ConvertFrom-RbspDerSignature $okDer)) 'a hand-made DER reads'
$bads = @(
    [byte[]](@(0x30, 0x45, 0x02, 0x21, 0x00) + $r + @(0x02, 0x20) + $r),       # a non-minimal leading zero
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + @(0x81) + $r[1..31] + @(0x02, 0x20) + $r),   # negative
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + @(0) * 32 + @(0x02, 0x20) + $r),     # zero (and non-minimal)
    [byte[]](@(0x30, 0x45, 0x02, 0x20) + $r + @(0x02, 0x20) + $r + @(0x00)),  # bytes after
    [byte[]](@(0x31, 0x44, 0x02, 0x20) + $r + @(0x02, 0x20) + $r),            # not a sequence
    [byte[]](@(0x30, 0x44, 0x02, 0x20) + $r + @(0x02, 0x21) + $r))            # a length past the end
foreach ($b in $bads) { Check ($null -eq (ConvertFrom-RbspDerSignature $b)) 'a DER refused' }

# ------------------------------------------------------------------ plainJSONKeys and payload.json
function B([string]$s) { return ,[Text.Encoding]::UTF8.GetBytes($s) }
$plain = @(
    @('{}', $true), @('{"a": 1, "b": [1, {"a": 2}]}', $true), @('{"a": "x\"y"}', $true),
    @('{"a": 1, "a": 2}', $false), @('{"a": {"b": 1, "b": 2}}', $false), @(('{"a' + [char]0x5c + 'u0062": 1}'), $false), @('{"": 1}', $false),
    @('[1]', $false), @('{"a": 1} x', $false), @('{"a": 1', $false), @("{`"a`": `"x`ny`"}", $false),
    @(('{"a": ' + ('[' * 31) + (']' * 31) + '}'), $true), @(('{"a": ' + ('[' * 32) + (']' * 32) + '}'), $false))
foreach ($c in $plain) { Check ((Test-RbspPlainJson (B $c[0])) -eq $c[1]) "plain keys: $($c[0].Substring(0, [Math]::Min(40, $c[0].Length)))" }

function New-Payload([string]$pv, [string]$wv, [hashtable]$over = @{}) {
    $h = @{ zip = 'rbstemsplus-windows.zip'; sha256 = 'a' * 64; admin = 'b' * 64; x64 = 'c' * 64; arm64 = 'd' * 64 }
    foreach ($k in $over.Keys) { $h[$k] = $over[$k] }
    $mac = '"app": {"version": "1.4.0", "zip": "a.zip", "sha256": "' + ('e' * 64) + '"}, '
    $win = if (-not $wv) { '' } else {
        ', "windows": {"app": {"version": "' + $wv + '", "zip": "' + $h.zip + '", "sha256": "' + $h.sha256 + '", "admin_sha256": "' + $h.admin +
        '", "stub_sha256": {"x64": "' + $h.x64 + '", "arm64": "' + $h.arm64 + '"}}, "bridge": {"file": "b.dll", "sha256": "' + ('f' * 64) + '"}, ' +
        '"compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["' + ('8' * 64) + '"], "stems_engine": ["0002"]}}'
    }
    return '{"payload_version": "' + $pv + '", ' + $mac + '"model": {"file": "m.onnx", "sha256": "' + ('9' * 64) + '", "size": 9}' + $win + '}'
}
$p = Read-RbspPayload (B (New-Payload '1.5.0' '1.5.0'))
Check ($p.PayloadVersion -eq '1.5.0' -and $p.AppVersion -eq '1.5.0' -and $p.Zip -eq 'rbstemsplus-windows.zip' -and $p.StubArm64 -eq ('d' * 64)) 'a payload read'
Check ($null -eq (Read-RbspPayload (B (New-Payload '1.6.0' $null)))) 'no windows block: $null'
$p = Read-RbspPayload (B (New-Payload '1.5.1' '1.5.0'))
Check ($p.AppVersion -eq '1.5.0' -and $p.PayloadVersion -eq '1.5.1') 'a carried app'
function Refused([string]$json, [string]$what) {
    $ok = $false
    try { [void](Read-RbspPayload (B $json)) } catch { $ok = $true }
    Check $ok "refused: $what"
}
Refused (New-Payload '1.5.0-beta.2' '1.4.0') 'a suffixed payload_version (no beta channel)'
Refused (New-Payload '1.5.0' '1.5.1') 'an app newer than the payload'
Refused (New-Payload '1.5.0' '1.5.0-beta.1') 'a suffixed windows.app.version'
Refused (New-Payload '1.5.0' '1.5.0' @{ zip = '../x.zip' }) 'a zip name with a path'
Refused (New-Payload '1.5.0' '1.5.0' @{ admin = 'B' * 64 }) 'an uppercase hash'
Refused (New-Payload '1.5.0' '1.5.0' @{ x64 = 'c' * 63 }) 'a short hash'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"model"', '"payload_version": "9.9.9", "model"')) 'payload_version twice'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"bridge": {"file": "b.dll", ', '"bridge": {')) 'a bridge with no file'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"file": "b.dll"', '"file": 7')) 'a bridge file that is not a string'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"bridge": {', '"bridges": {')) 'no bridge'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"compatible": {', '"compat": {')) 'no compatible'
Refused ((New-Payload '1.5.0' '1.5.0').Replace('"compatible": {"rekordbox": ["7.2.19"], "ort_sha256": ["' + ('8' * 64) + '"], "stems_engine": ["0002"]}', '"compatible": []')) 'a compatible that is not an object'

# ------------------------------------------------------------------ where the zip comes from
$pz = [pscustomobject]@{ PayloadVersion = '1.5.2'; Zip = 'w.zip' }
Check ((Get-RbspZipUrl $BaseUrl $pz) -ceq 'https://github.com/Gabe-LS/rbstemsplus/releases/download/v1.5.2/w.zip') 'from the latest release: the zip from its own tag'
Check ((Get-RbspZipUrl 'http://127.0.0.1:8080/r/' $pz) -ceq 'http://127.0.0.1:8080/r/w.zip') 'a test server: the zip beside payload.json'
$tagged = 'https://github.com/Gabe-LS/rbstemsplus/releases/download/v1.4.0/'
Check ((Get-RbspZipUrl $tagged $pz) -ceq ($tagged + 'w.zip')) 'a release named by its tag: the zip beside payload.json'

# ------------------------------------------------------------------ the refusals before any download
function Can([hashtable]$over) {
    $f = @{ Sac = 0; Language = 'FullLanguage'; Build = 22631; Net = 533320; UacOff = $false; Admin = $false }
    foreach ($k in $over.Keys) { $f[$k] = $over[$k] }
    try { Test-RbspCanInstall $f; return $null } catch { return $_.Exception.Message }
}
Check ($null -eq (Can @{})) 'a standard user on Windows 11 22H2: installs'
Check ($null -eq (Can @{ UacOff = $true })) 'UAC off with a standard token: installs'
Check ((Can @{ Admin = $true }) -ceq $Texts.Elevated) 'an elevated window: refused'
Check ((Can @{ Admin = $true; UacOff = $true }) -ceq $Texts.UacOff) 'an administrator with UAC off: the UAC reason'
Check ((Can @{ Sac = 1; Build = 0; Admin = $true }) -ceq $Texts.SacOn) 'Smart App Control on: said first'
Check ($null -eq (Can @{ Sac = 2 }))'Smart App Control in evaluation: installs'
Check ((Can @{ Language = 'ConstrainedLanguage'; Build = 0 }) -ceq $Texts.Language) 'ConstrainedLanguage: refused before the build'
Check ((Can @{ Build = 22000 }) -ceq $Texts.OldWindows) 'Windows 11 21H2: refused'
Check ((Can @{ Net = 528040 }) -ceq $Texts.OldNet) '.NET Framework older than 4.8.1: refused'
$facts = Get-RbspFacts
if ($onWindows) {
    Check ($facts.Build -eq [Environment]::OSVersion.Version.Build -and $facts.Net -gt 0) "the facts read on Windows: build $($facts.Build), .NET release $($facts.Net)"
} else {
    Check ($facts.Build -eq 0 -and -not $facts.Admin -and $facts.Net -eq 0) 'the facts read on the Mac: no build, no admin'
}

# what runs before the refusals runs in any language mode (ConstrainedLanguage allows only core
# types): no [ref], no static or instance method call, no New-Object, no Add-Type in it
foreach ($fn in @('Get-RbspSacState', 'Get-RbspFacts', 'Test-RbspCanInstall')) {
    $body = (Get-Command $fn).ScriptBlock.ToString()
    Check (-not ($body -match '\[ref\]|::|New-Object|Add-Type|\.[A-Za-z]+\(')) "$fn uses only what ConstrainedLanguage allows"
}

# ------------------------------------------------------------------ the one-liner (irm | iex), with no test variables
# The child gets its command as -EncodedCommand (Windows PowerShell 5.1 drops the double quotes
# embedded in a native command's argument), with $ErrorActionPreference Continue around the call
# (on 5.1 a line on the child's stderr through 2>&1 would otherwise be a terminating
# NativeCommandError that stops this suite). -OutputFormat Text: an encoded, non-interactive child
# with stderr redirected writes CLIXML, which pwsh 7.6's parent dies deserializing. Off Windows the
# constrained child gets a Get-ItemProperty stub with a Windows registry's values first, so the
# facts' parsing really runs there (with no values, the old [ref] line was never reached).
function Invoke-OneLiner([string]$exe, [bool]$constrained) {
    $cmd = '$ProgressPreference = ''SilentlyContinue''; '
    if ($constrained -and -not $onWindows) {
        $cmd += 'function global:Get-ItemProperty { param([string]$LiteralPath, [string]$Name, $ErrorAction) ' +
                '[pscustomobject]@{ CurrentBuildNumber = ''22631''; Release = 533320; EnableLUA = 1; VerifiedAndReputablePolicyState = 0; ' +
                'PROCESSOR_ARCHITECTURE = ''AMD64'' } }; '
    }
    if ($constrained) { $cmd += '$ExecutionContext.SessionState.LanguageMode = ''ConstrainedLanguage''; ' }
    $cmd += 'Get-Content -Raw -LiteralPath $env:RBSP_BOOT_PATH | iex'
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $savedBase = $env:RBSTEMSPLUS_BASE_URL
    $savedEap = $ErrorActionPreference
    $env:RBSTEMSPLUS_BASE_URL = $null
    $env:RBSP_BOOT_PATH = $path
    $o = ''
    try {
        $ErrorActionPreference = 'Continue'
        $o = (& $exe -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $enc 2>&1 | Out-String)
    } finally { $ErrorActionPreference = $savedEap; $env:RBSTEMSPLUS_BASE_URL = $savedBase; $env:RBSP_BOOT_PATH = $null }
    return ($o -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
}
if ($onWindows) {
    # in a ConstrainedLanguage child: refused before any download (section 4's order: SAC first,
    # so a machine with Smart App Control on gets that text instead). The FullLanguage one-liner
    # isn't run here: on a supported Windows it would go on towards the download.
    $out = Invoke-OneLiner 'powershell' $true
    Check ($out -ceq $Texts.Language -or $out -ceq $Texts.SacOn) "under iex in ConstrainedLanguage: the language (or SAC) refusal alone (got: $out)"
    Check (-not $out.Contains('cannot be retrieved')) 'under iex in ConstrainedLanguage: no StrictMode error'
} else {
    $out = Invoke-OneLiner 'pwsh' $false
    Check ($out -ceq $Texts.OldWindows) "under iex on the Mac: the old-Windows refusal alone (got: $out)"
    Check (-not $out.Contains('cannot be retrieved')) 'under iex: no StrictMode error'
    # the same in a ConstrainedLanguage child (pwsh lets a session lower its own mode on any OS)
    $out = Invoke-OneLiner 'pwsh' $true
    Check ($out -ceq $Texts.Language) "under iex in ConstrainedLanguage on the Mac: the language refusal alone (got: $out)"
}

# ------------------------------------------------------------------ the install on a temp folder
$tmpRoot = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'rbsp-boot-test')   # one fixed name: a failed run's folder goes at the next
# a run stopped in the link case leaves its link: deleted alone first (non-recursive), as below
$bentLink = [IO.Path]::Combine($tmpRoot, 'bent', 'link')
if ([IO.Directory]::Exists($bentLink)) { [IO.Directory]::Delete($bentLink) }
if ([IO.Directory]::Exists($tmpRoot)) { [IO.Directory]::Delete($tmpRoot, $true) }
[void][IO.Directory]::CreateDirectory($tmpRoot)
function Sha([byte[]]$b) { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b))).Replace('-', '').ToLowerInvariant() }

# an unpacked app (release.md 3.1's files), with known worker and stub bytes
function New-Unpacked([string]$dir, [string]$stubTag) {
    $files = @{}
    foreach ($rel in $Required) { $files[$rel] = "bytes of $rel" }
    $files["stub/x64/$AppExe"] = "stub x64 $stubTag"
    $files["stub/arm64/$AppExe"] = "stub arm64 $stubTag"
    foreach ($rel in $files.Keys) {
        $f = Join-RbspPath $dir $rel
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($f))
        [IO.File]::WriteAllBytes($f, [Text.Encoding]::ASCII.GetBytes($files[$rel]))
    }
}
function New-P([string]$pv, [string]$v, [string]$stubTag) {
    return [pscustomobject]@{
        PayloadVersion = $pv; AppVersion = $v; Zip = 'w.zip'; ZipSha256 = 'a' * 64
        AdminSha256 = Sha ([Text.Encoding]::ASCII.GetBytes("bytes of admin/$AppExe"))
        StubX64 = Sha ([Text.Encoding]::ASCII.GetBytes("stub x64 $stubTag")); StubArm64 = Sha ([Text.Encoding]::ASCII.GetBytes("stub arm64 $stubTag"))
    }
}
$script:calls = New-Object System.Collections.ArrayList
$script:watcherRunning = $false
$script:appRunning = $false
$script:openOnStop = $false      # the app opens while the watchers stop
$script:shortcutThrows = $false
$fakeSys = @{
    AppRunning = { param($apps) return $script:appRunning }
    StopWatchers = {
        param($apps)
        $r = $script:watcherRunning; [void]$script:calls.Add("stop $apps")
        if ($script:openOnStop) { $script:appRunning = $true }
        return $r
    }
    SetRegistry = { param($key, $name, $kind, $data) [void]$script:calls.Add("reg $key|$name|$kind|$data") }
    NewShortcut = {
        param($lnk, $target, $aumid)
        if ($script:shortcutThrows) { throw 'no shortcut' }
        [void]$script:calls.Add("lnk $lnk|$target|$aumid")
    }
    Start = { param($exe, $argv) [void]$script:calls.Add("start $exe|$($argv -join ' ')") }
    ProductInfo = { param($exe) [void]$script:calls.Add("info $exe"); return $script:productInfo }
}
$script:productInfo = @{ Version = '1.5.0'; Name = 'RB Stems Plus' }
$keys = @($pub1)
$layout = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local')) ([IO.Path]::Combine($tmpRoot, 'Programs'))
Check ($layout.Apps.EndsWith([IO.Path]::Combine('Local', 'Programs', 'RB Stems Plus')) -and $layout.Support.EndsWith([IO.Path]::Combine('Local', 'rbstemsplus'))) 'the layout'
# an unpacked app in a folder of its own, as in the script's .bootstrap-<pid>\app
function Stage([string]$name, [string]$tag) {
    $d = [IO.Path]::Combine($tmpRoot, $name, 'app')
    New-Unpacked $d $tag
    return $d
}
function Starts { return ,@($script:calls | Where-Object { $_ -like 'start *' }) }

# a first install
$json = B (New-Payload '1.5.0' '1.5.0')
$sig = Sign $k1 $json
$un = Stage 'staging1' 'one'
Test-RbspUnpacked $un (New-P '1.5.0' '1.5.0' 'one')
Install-RbspApp $layout (New-P '1.5.0' '1.5.0' 'one') $json $sig $un 'arm64' $fakeSys $keys
function Text([string]$f) { return [IO.File]::ReadAllText($f) }
function Bytes([string]$f) { return [IO.File]::ReadAllBytes($f) }
function Same([byte[]]$a, [byte[]]$b) { return [Convert]::ToBase64String($a) -ceq [Convert]::ToBase64String($b) }
$v150 = [IO.Path]::Combine($layout.Apps, '1.5.0')
Check ([IO.File]::Exists([IO.Path]::Combine($v150, $AppExe)) -and -not [IO.Directory]::Exists($un)) 'the version folder holds the app, staging moved'
Check ((Text $layout.Stub) -ceq 'stub arm64 one') 'the stub of the arch'
$cur = Bytes $layout.Current
Check ([Text.Encoding]::ASCII.GetString($cur) -ceq "1.5.0`n" -and $cur.Length -eq 6) 'current: the version and a line end, no BOM'
foreach ($d in @($layout.Installed, $layout.Payload)) {
    Check (Same (Bytes ([IO.Path]::Combine($d, 'payload.json'))) $json) "payload.json in $([IO.Path]::GetFileName($d))"
    Check (Same (Bytes ([IO.Path]::Combine($d, 'payload.json.sig'))) $sig) "payload.json.sig in $([IO.Path]::GetFileName($d))"
}
$prev = [IO.Path]::Combine($layout.Installed, 'prev')
Check (-not [IO.Directory]::Exists($prev)) 'a first install: no prev\'
Check ((Text $layout.Floor) -ceq "1.5.0`n") 'the floor'
$seen = @($script:calls)
Check ($seen -contains "lnk $($layout.Shortcut)|$($layout.Stub)|io.github.rbstemsplus.app") 'the shortcut starts the stub, with the AppUserModelID'
Check ($seen -contains "reg $RunKey|$RunValue|String|`"$($layout.Stub)`" --watcher") 'the Run value: the stub with --watcher'
Check ($seen -contains "reg $UninstallKey|Publisher|String|Gabe-LS") 'Publisher Gabe-LS'
Check ($seen -contains "reg $UninstallKey|DisplayVersion|String|1.5.0") 'DisplayVersion: the release'
Check ($seen -contains "reg $UninstallKey|UninstallString|String|`"$($layout.Stub)`" --uninstall") 'UninstallString: the stub with --uninstall'
Check (@($seen | Where-Object { $_ -like "reg $UninstallKey|EstimatedSize|DWord|*" }).Count -eq 1) 'EstimatedSize'
$iRun = [Array]::IndexOf($seen, "reg $RunKey|$RunValue|String|`"$($layout.Stub)`" --watcher")
$iLnk = [Array]::IndexOf($seen, "lnk $($layout.Shortcut)|$($layout.Stub)|io.github.rbstemsplus.app")
$iUn = [Array]::IndexOf($seen, "reg $UninstallKey|NoRepair|DWord|1")
Check ($iRun -ge 0 -and $iRun -lt $iUn -and $iUn -lt $iLnk) 'the order: the Run value, the Apps & features entry, then the shortcut'
$starts = Starts
Check ($starts.Count -eq 2 -and $starts[0] -eq "start $($layout.Stub)|--watcher" -and $starts[1] -eq "start $($layout.Stub)|") 'a first install starts the watcher, then the app, both through the stub'

# an update of the app by the script: a carried release and a new stub (its signature is the
# first's, so this pair doesn't verify)
$script:calls.Clear()
[void][IO.Directory]::CreateDirectory([IO.Path]::Combine($layout.Apps, '.bootstrap-1'))
$un = Stage 'staging2' 'two'
$json16 = B (New-Payload '1.6.0' '1.6.0')
Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $fakeSys $keys
Check (-not [IO.Directory]::Exists($v150) -and [IO.Directory]::Exists([IO.Path]::Combine($layout.Apps, '1.6.0'))) 'the older version folder removed'
Check (-not [IO.Directory]::Exists([IO.Path]::Combine($layout.Apps, '.bootstrap-1'))) 'a leftover of the script removed'
Check ((Text $layout.Stub) -ceq 'stub x64 two' -and (Text $layout.Current) -ceq "1.6.0`n" -and (Text $layout.Floor) -ceq "1.6.0`n") 'stub, current and floor moved on'
Check ((Same (Bytes ([IO.Path]::Combine($prev, 'payload.json'))) $json) -and (Same (Bytes ([IO.Path]::Combine($prev, 'payload.json.sig'))) $sig)) 'the pair installed-release\ held, verified, kept in prev\'
$starts = Starts
Check ($starts.Count -eq 2 -and $starts[0] -eq "start $($layout.Stub)|--watcher" -and $starts[1] -eq "start $($layout.Stub)|") 'no watcher was running: one started all the same, then the app'

# a watcher running: stopped first, started again; a reinstall of the version `current` names
$script:calls.Clear()
$script:watcherRunning = $true
$updateDir = [IO.Path]::Combine($layout.Support, 'update')
[void][IO.Directory]::CreateDirectory($updateDir)
$updateFiles = @('pending', 'started', 'payload.json', 'payload.json.sig', 'watcher', 'result') |
    ForEach-Object { [IO.Path]::Combine($updateDir, $_) }
$otherFile = [IO.Path]::Combine($updateDir, 'unrelated')
foreach ($f in $updateFiles + @($otherFile)) { [IO.File]::WriteAllText($f, 'left by an earlier run') }
$recordingSys = $fakeSys.Clone()
$recordingSys.Start = {
    param($exe, $argv)
    foreach ($f in $updateFiles) { Check (-not [IO.File]::Exists($f)) "a reinstall clears $([IO.Path]::GetFileName($f)) before starting the stub" }
    & $fakeSys.Start $exe $argv
}
$un = Stage 'staging3' 'two'
Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $recordingSys $keys
Check ([IO.File]::Exists($otherFile)) 'a reinstall keeps unrelated files in the update folder'
$c = @($script:calls)
Check ($c[0] -like 'stop *' -and $c -contains "start $($layout.Stub)|--watcher") 'the running watcher stopped first and started again'
Check ([IO.File]::Exists([IO.Path]::Combine($layout.Apps, '1.6.0', $AppExe)) -and -not [IO.Directory]::Exists($un)) 'a reinstall: the version folder replaced'
Check (Same (Bytes ([IO.Path]::Combine($prev, 'payload.json'))) $json) 'a pair that does not verify is not kept in prev\'
$script:watcherRunning = $false

# refusals: the app open; the app opened while the watcher stopped; a stub that isn't the payload's
$script:calls.Clear()
$script:appRunning = $true
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'x') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($threw -eq $Texts.AppOpen -and (Text $layout.Current) -ceq "1.6.0`n" -and $script:calls.Count -eq 0) 'the app open: refused, nothing changed, no watcher stopped'
$script:appRunning = $false
$script:calls.Clear()
$script:openOnStop = $true
$un = Stage 'staging4' 'three'
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'three') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$starts = Starts
Check ($threw -eq $Texts.AppOpen -and (Text $layout.Current) -ceq "1.6.0`n" -and [IO.Directory]::Exists($un)) 'the app opened while the watcher stopped: refused before anything moved'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'and the watcher started again, not the app'
$script:openOnStop = $false
$script:appRunning = $false
$script:calls.Clear()
$un = Stage 'staging5' 'three'
$threw = $null
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'other') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($null -ne $threw -and (Text $layout.Current) -ceq "1.6.0`n" -and (Text $layout.Stub) -ceq 'stub x64 two') 'a stub that is not the payload''s: refused before current or the stub moved'
$starts = Starts
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'and the watcher started again, not the app'

# a reinstall whose move in fails: the old version folder moved back, current unchanged
$v160 = [IO.Path]::Combine($layout.Apps, '1.6.0')
[IO.File]::WriteAllText([IO.Path]::Combine($v160, 'marker'), 'old')
$script:calls.Clear()
$un = [IO.Path]::Combine($tmpRoot, 'staging6', 'app')   # missing: the move in fails
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($un))
$threw = $null
try { Install-RbspApp $layout (New-P '1.6.0' '1.6.0' 'two') $json16 $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
Check ($null -ne $threw -and (Text ([IO.Path]::Combine($v160, 'marker'))) -ceq 'old' -and [IO.File]::Exists([IO.Path]::Combine($v160, $AppExe))) 'the move in failed: the old version folder back in place'
Check ((Text $layout.Current) -ceq "1.6.0`n" -and -not [IO.Directory]::Exists([IO.Path]::Combine($tmpRoot, 'staging6', 'old'))) 'current unchanged, nothing left aside'
Check ((Starts) -contains "start $($layout.Stub)|--watcher") 'and the watcher started again'

# an update that stops late (the shortcut fails): current moved on, the watcher started, not the app
$script:calls.Clear()
$script:shortcutThrows = $true
$un = Stage 'staging7' 'three'
$json17 = B (New-Payload '1.7.0' '1.7.0')
$threw = $null
$oldResult = [IO.Path]::Combine($updateDir, 'result')
[IO.File]::WriteAllText($oldResult, 'left by an earlier run')
try { Install-RbspApp $layout (New-P '1.7.0' '1.7.0' 'three') $json17 $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$starts = Starts
Check ($threw -eq 'no shortcut' -and (Text $layout.Current) -ceq "1.7.0`n") 'an update stopped at the shortcut: current names the new version'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout.Stub)|--watcher") 'an update stopped late: the watcher started, the app not'
Check ([IO.File]::Exists($oldResult)) 'an install stopped at the shortcut keeps the previous update result'

# a first install that stops late: the Apps & features entry is there, the watcher started
$layout2 = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local2')) ([IO.Path]::Combine($tmpRoot, 'Programs2'))
$script:calls.Clear()
$un = Stage 'staging8' 'one'
$threw = $null
try { Install-RbspApp $layout2 (New-P '1.5.0' '1.5.0' 'one') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
$seen = @($script:calls)
$starts = Starts
Check ($threw -eq 'no shortcut' -and $seen -contains "reg $UninstallKey|UninstallString|String|`"$($layout2.Stub)`" --uninstall") 'a first install stopped at the shortcut: it can be removed in Settings > Apps'
Check ($starts.Count -eq 1 -and $starts[0] -eq "start $($layout2.Stub)|--watcher") 'a first install stopped late: the watcher started, the app not'
$script:shortcutThrows = $false

# a first install that stops before the stub: no app folder left (Invoke-RbspMain's finally)
$layout3 = Get-RbspLayout ([IO.Path]::Combine($tmpRoot, 'Local3')) ([IO.Path]::Combine($tmpRoot, 'Programs3'))
$created = -not [IO.Directory]::Exists($layout3.Apps)
[void][IO.Directory]::CreateDirectory($layout3.Apps)
$tmp3 = [IO.Path]::Combine($layout3.Apps, '.bootstrap-7')
$un = [IO.Path]::Combine($tmp3, 'app')
New-Unpacked $un 'one'
$script:calls.Clear()
$threw = $null
try { Install-RbspApp $layout3 (New-P '1.5.0' '1.5.0' 'other') $json $sig $un 'x64' $fakeSys $keys } catch { $threw = $_.Exception.Message }
finally { Remove-RbspTemp $layout3 $tmp3 $created }
Check ($created -and $null -ne $threw -and -not [IO.Directory]::Exists($layout3.Apps)) 'a first install stopped before the stub: the app folder removed'
Check ((Starts).Count -eq 0) 'and nothing started (no stub)'
# an existing app folder is kept
[void][IO.Directory]::CreateDirectory($layout3.Apps)
Remove-RbspTemp $layout3 $tmp3 $false
Check ([IO.Directory]::Exists($layout3.Apps)) 'an app folder this run did not make: kept'

# the install lock (audit b-a2b-F2): afterexit.c's AX_INSTALL_LOCK, taken before .bootstrap-<pid> is
# made and held until after the app's start; while an uninstall's after-exit holds it the install
# waits, then refuses with nothing made. The holder here: another thread (a mutex is per thread)
$afterH = [IO.File]::ReadAllText([IO.Path]::Combine($root, 'win', 'c', 'afterexit.h'))
Check ($afterH -match ('#define AX_INSTALL_LOCK "' + [regex]::Escape($InstallLock.Replace('\', '\\')) + '"')) 'the install lock is afterexit.h''s AX_INSTALL_LOCK'
$mainSrc = (Get-Command Invoke-RbspMain).ScriptBlock.ToString()
$versionAt = $mainSrc.IndexOf('Test-RbspAppVersion $unpacked $p $sys')
Check ($versionAt -gt $mainSrc.IndexOf('Test-RbspUnpacked $unpacked $p') -and $versionAt -lt $mainSrc.IndexOf('Install-RbspApp $layout')) 'IN3: the unpacked app version is checked before installation'
$lockAt = $mainSrc.IndexOf('Open-RbspInstallLock')
Check ($lockAt -ge 0 -and $lockAt -lt $mainSrc.IndexOf('.bootstrap-$PID') -and $lockAt -lt $mainSrc.IndexOf('CreateDirectory($layout.Apps)')) 'the lock taken before the app folder and .bootstrap-<pid> are made'
Check ($mainSrc.IndexOf('Close-RbspInstallLock') -gt $mainSrc.IndexOf('Install-RbspApp')) 'and released after the install (the app started)'
$lockName = "Local\rbsp-test-lock-$PID"
$ready = New-Object Threading.ManualResetEvent($false)
$release = New-Object Threading.ManualResetEvent($false)
$holder = [PowerShell]::Create()
[void]$holder.AddScript({
    param($n, $ready, $release)
    $m = New-Object Threading.Mutex($false, $n)
    [void]$m.WaitOne()
    try {
        [void]$ready.Set()
        [void]$release.WaitOne(30000)
        Start-Sleep -Milliseconds 300
    } finally { $m.ReleaseMutex(); $m.Dispose() }
}).AddArgument($lockName).AddArgument($ready).AddArgument($release)
$async = $holder.BeginInvoke()
try {
    Check ($ready.WaitOne(30000)) 'the holder took the lock'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $result = @(& {
        try { $l = Open-RbspInstallLock $lockName 700; Close-RbspInstallLock $l } catch { $_.Exception.Message }
    } 6>&1)
    $waited = $clock.ElapsedMilliseconds
    $threw = $result | Where-Object { $_ -is [string] }
    Check ($threw -ceq $Texts.Uninstalling -and $waited -ge 600) "the lock held by an uninstall: waited, then refused ($threw, $waited ms)"
    $notice = @($result | Where-Object { $_ -is [System.Management.Automation.InformationRecord] })
    Check ($notice.Count -eq 1 -and $notice[0].ToString() -ceq "Waiting for RB Stems Plus's uninstall to finish...") 'the lock held by an uninstall: one waiting line'
    [void]$release.Set()
    $threw = $null
    try { $l = Open-RbspInstallLock $lockName 10000; Close-RbspInstallLock $l } catch { $threw = $_.Exception.Message }
    Check ($null -eq $threw) "released within the wait: the install goes on ($threw)"
} finally {
    [void]$release.Set()
    try { [void]$holder.EndInvoke($async) } finally { $holder.Dispose(); $ready.Dispose(); $release.Dispose() }
}
$result = @(& {
    try { $l = Open-RbspInstallLock $lockName 0; Close-RbspInstallLock $l } catch { $_.Exception.Message }
} 6>&1)
Check ($result.Count -eq 0) "the lock free: taken at once, no waiting line ($result)"

# the unpacked app's checks, on a zip made here and unpacked as the script does
$un = [IO.Path]::Combine($tmpRoot, 'zipsrc')
New-Unpacked $un 'z'
$zip = [IO.Path]::Combine($tmpRoot, 'w.zip')
Add-Type -AssemblyName System.IO.Compression.FileSystem
# entry per file with '/' names, as a release zip has them (.NET Framework's CreateFromDirectory
# writes '\', which the zip check rightly refuses)
$za = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    $base = [IO.Path]::GetFullPath($un)
    foreach ($f in Get-ChildItem -LiteralPath $un -Recurse -File -Force) {
        $rel = $f.FullName.Substring($base.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $rel)
    }
} finally { $za.Dispose() }
$out = [IO.Path]::Combine($tmpRoot, 'unzipped')
Expand-RbspZip $zip $out
$pz = New-P '1.5.0' '1.5.0' 'z'
$ok = $true
try { Test-RbspUnpacked $out $pz } catch { $ok = $false; Write-Host $_.Exception.Message }
Check $ok 'a zip of the app checks out'

# the zip's entry names, read before Expand-RbspZip (unzip.c's unzip_name_ok and its link rule)
foreach ($n in @('x64/rbsp-core.dll', 'admin/', 'NOTICE-windows.txt', 'CONSOLE.txt', 'COM10', 'a/b/c/d/e/f/g/h', ('a' * 255))) {
    Check (Test-RbspZipName $n) "a plain zip name: '$($n.Substring(0, [Math]::Min(40, $n.Length)))'"
}
foreach ($n in @('', '/', '/x', '../x', 'a/../b', 'a/./b', 'a//b', 'a\b', 'C:x', 'a*b', 'a?b', 'a"b', 'a<b', 'a|b', 'x ', 'x.',
                 'a /b', 'a./b', 'CON', 'con.txt', 'Lpt1', 'COM9.dll', 'NUL .txt', 'aux/x', 'CONIN$', 'conin$', 'CONiN$', 'conin$.txt', 'conout$.log', "a`tb",
                 "caf$([char]0xe9)", ('a' * 256), 'a/b/c/d/e/f/g/h/i')) {
    Check (-not (Test-RbspZipName $n)) "not a plain zip name: '$($n.Substring(0, [Math]::Min(40, $n.Length)))'"
}
# the device names upper-cased invariantly and matched case-sensitively: a case-insensitive match
# folds by the culture (tr-TR), and PowerShell's regex cache ignores a culture changed in-process,
# so the text is the guard
Check (-not ((Get-Command Test-RbspZipName).ScriptBlock.ToString() -match '-imatch')) 'Test-RbspZipName has no culture-dependent -imatch'
$ok = $true
try { Test-RbspZip $zip } catch { $ok = $false; Write-Host $_.Exception.Message }
Check $ok 'the app''s zip: its entries pass'
# a zip made here with the entries $fill adds
function New-TestZip([string]$name, [scriptblock]$fill) {
    $f = [IO.Path]::Combine($tmpRoot, "$name.zip")
    if ([IO.File]::Exists($f)) { [IO.File]::Delete($f) }
    $a = [IO.Compression.ZipFile]::Open($f, 'Create')
    try { [void](& $fill $a) } finally { $a.Dispose() }
    return $f
}
function Add-Entry($a, [string]$n, [string]$text) {
    $e = $a.CreateEntry($n)
    if ($text) {
        $s = $e.Open()
        try { $b = [Text.Encoding]::ASCII.GetBytes($text); $s.Write($b, 0, $b.Length) } finally { $s.Dispose() }
    }
    return $e
}
function Zip-Refused([scriptblock]$fill, [string]$what, [string]$like) {
    $f = New-TestZip 'bent' $fill
    $r = $false
    try { Test-RbspZip $f } catch { $r = $_.Exception.Message -like $like; if (-not $r) { Write-Host $_.Exception.Message } }
    Check $r "zip refused: $what"
}
Zip-Refused { param($a) [void](Add-Entry $a 'ok.txt' 'x'); [void](Add-Entry $a '../x' 'x') } 'a name with ..' '*isn''t plain*'
Zip-Refused { param($a) [void](Add-Entry $a 'a\b' 'x') } 'a backslash' '*isn''t plain*'
Zip-Refused { param($a) [void](Add-Entry $a 'C:/x' 'x') } 'a drive' '*isn''t plain*'
Zip-Refused { param($a) $e = Add-Entry $a 'lnk' 'target'; $e.ExternalAttributes = 0xA1FF -shl 16 } 'a symbolic link entry' '*holds a link*'
Zip-Refused { param($a) [void](Add-Entry $a 'A.txt' 'x'); [void](Add-Entry $a 'a.txt' 'y') } 'a name twice, ignoring case' '*twice*'
Zip-Refused { param($a) [void](Add-Entry $a 'd/' ''); [void](Add-Entry $a 'D/' '') } 'a folder twice' '*twice*'
Zip-Refused { param($a) [void](Add-Entry $a 'a' 'x'); [void](Add-Entry $a 'A/b' 'y') } 'a file that is a folder of another' '*same name*'
Zip-Refused { param($a) [void](Add-Entry $a 'd/' 'data') } 'a folder entry with data' '*folder with data*'
$f = New-TestZip 'unixmode' { param($a) $e = Add-Entry $a 'x/f.txt' 'x'; $e.ExternalAttributes = 0x81A4 -shl 16; [void](Add-Entry $a 'x/' '') }
$ok = $true
try { Test-RbspZip $f } catch { $ok = $false; Write-Host $_.Exception.Message }
Check $ok 'a regular file with a Unix mode and its folder''s entry: pass'

# the app's version resource (update.c's check_app), read through $sys.ProductInfo
function Version-Ok([string]$ver, [string]$name) {
    $script:productInfo = @{ Version = $ver; Name = $name }
    try { Test-RbspAppVersion $out $pz $fakeSys; return $true } catch { return $false }
}
$script:calls.Clear()
Check (Version-Ok '1.5.0' 'RB Stems Plus') 'ProductVersion the payload''s'
Check (@($script:calls) -contains "info $([IO.Path]::Combine($out, $AppExe))") 'the version read from the unpacked app'
Check (Version-Ok '1.5.0+3f2a9c1' 'RB Stems Plus') 'ProductVersion with a +build suffix'
Check (Version-Ok ' 1.5.0 ' ' RB Stems Plus ') 'trimmed'
foreach ($v in @('1.5.0.0', '1.5.01', '1.5', '1.4.0', '1.5.0-beta.1', '')) {
    Check (-not (Version-Ok $v 'RB Stems Plus')) "ProductVersion refused: '$v'"
}
Check (-not (Version-Ok '1.5.0' 'rb stems plus')) 'ProductName refused: another case'
Check (-not (Version-Ok '1.5.0' '')) 'ProductName refused: none'
$script:productInfo = @{ Version = '1.5.0'; Name = 'RB Stems Plus' }
$refused = $false
try {
    $badVersion = Read-RbspPayload (B (New-Payload '1.5.0' '0.0.0'))
    Test-RbspAppVersion $out $badVersion $fakeSys
} catch { $refused = $_.Exception.Message -ceq "the app's version (1.5.0) isn't the one the payload names (0.0.0)" }
Check $refused 'bad-version payload reaches the app version check and is refused there'
function Unpacked-Refused([scriptblock]$bend, [string]$what, [string]$like = '*') {
    $d = [IO.Path]::Combine($tmpRoot, 'bent')
    if ([IO.Directory]::Exists($bentLink)) { [IO.Directory]::Delete($bentLink) }
    if ([IO.Directory]::Exists($d)) { [IO.Directory]::Delete($d, $true) }
    New-Unpacked $d 'z'
    & $bend $d
    $r = $false
    try { Test-RbspUnpacked $d $pz } catch { $r = $_.Exception.Message -like $like }
    Check $r "unpacked refused: $what"
}
Unpacked-Refused { param($d) [IO.File]::Delete([IO.Path]::Combine($d, 'LICENSE')) } 'LICENSE missing'
Unpacked-Refused { param($d) [IO.File]::WriteAllText([IO.Path]::Combine($d, 'admin', $AppExe), 'another worker') } 'another worker'
Unpacked-Refused { param($d) [IO.File]::WriteAllText([IO.Path]::Combine($d, 'stub', 'arm64', $AppExe), 'another stub') } 'another arm64 stub'
# a link inside (on Windows a junction: a symbolic link needs an elevated or developer-mode account)
$linkType = if ($onWindows) { 'Junction' } else { 'SymbolicLink' }
# to a folder that is not above the link, so the refusal can only be the link check's
$lt = [IO.Path]::Combine($tmpRoot, 'linktarget')
[void][IO.Directory]::CreateDirectory($lt)
Unpacked-Refused { param($d) [void](New-Item -ItemType $linkType -Path ([IO.Path]::Combine($d, 'link')) -Target $lt) } 'a link inside' '*holds a link*'
# the link alone first (non-recursive): .NET Framework's recursive delete can throw on a nested mount point
[IO.Directory]::Delete($bentLink)

# ------------------------------------------------------------------ the capped extraction (BS2)
# zips written byte by byte here: one stored or deflated entry whose sizes the header declares as
# given (the review's size-mismatch zips, small: the caps are the same code at any size)
# (CRC-32 in [long]s: a hex literal over 0x7FFFFFFF is a negative [int] in PowerShell)
$crcTable = New-Object 'long[]' 256
for ($i = 0; $i -lt 256; $i++) {
    $c = [long]$i
    for ($k = 0; $k -lt 8; $k++) { if ($c -band 1) { $c = [long]3988292384 -bxor ($c -shr 1) } else { $c = $c -shr 1 } }
    $crcTable[$i] = $c
}
function Crc32([byte[]]$b) {
    $c = [long]4294967295
    foreach ($x in $b) { $c = $crcTable[($c -bxor $x) -band 0xFF] -bxor ($c -shr 8) }
    return [uint32]($c -bxor [long]4294967295)
}
function New-RawZip([string]$name, [object[]]$entries) {
    # $entries: @(name, data bytes, method (0 stored, 8 deflate), declared uncompressed size)
    $ms = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter($ms)
    $central = New-Object IO.MemoryStream
    $cw = New-Object IO.BinaryWriter($central)
    foreach ($e in $entries) {
        $nb = [Text.Encoding]::ASCII.GetBytes([string]$e[0])
        [byte[]]$raw = $e[1]
        $data = $raw
        if ($e[2] -eq 8) {
            $dm = New-Object IO.MemoryStream
            $ds = New-Object IO.Compression.DeflateStream($dm, [IO.Compression.CompressionMode]::Compress, $true)
            $ds.Write($raw, 0, $raw.Length); $ds.Dispose()
            $data = $dm.ToArray()
        }
        $off = [uint32]$ms.Position
        $crc = Crc32 $raw
        foreach ($h in @(@($w, 0x04034b50), @($cw, 0x02014b50))) {
            $x = $h[0]
            $x.Write([uint32]$h[1])
            if ($h[1] -eq 0x02014b50) { $x.Write([uint16]20) }
            $x.Write([uint16]20); $x.Write([uint16]0); $x.Write([uint16]$e[2]); $x.Write([uint16]0); $x.Write([uint16]0x21)
            $x.Write([uint32]$crc); $x.Write([uint32]$data.Length); $x.Write([uint32]$e[3])
            $x.Write([uint16]$nb.Length); $x.Write([uint16]0)
            if ($h[1] -eq 0x02014b50) { $x.Write([uint16]0); $x.Write([uint16]0); $x.Write([uint16]0); $x.Write([uint32]0); $x.Write([uint32]$off) }
            $x.Write($nb)
        }
        $w.Write($data)
    }
    $cdOff = [uint32]$ms.Position
    $cd = $central.ToArray()
    $w.Write($cd)
    $w.Write([uint32]0x06054b50); $w.Write([uint16]0); $w.Write([uint16]0)
    $w.Write([uint16]$entries.Count); $w.Write([uint16]$entries.Count); $w.Write([uint32]$cd.Length); $w.Write($cdOff); $w.Write([uint16]0)
    $w.Flush()
    $f = [IO.Path]::Combine($tmpRoot, "$name.zip")
    [IO.File]::WriteAllBytes($f, $ms.ToArray())
    return $f
}
function Expand-Result([string]$zip, [long]$max = 512MB) {
    $d = [IO.Path]::Combine($tmpRoot, 'expanded')
    if ([IO.Directory]::Exists($d)) { [IO.Directory]::Delete($d, $true) }
    try { Expand-RbspZip $zip $d $max; return '' } catch { return $_.Exception.Message }
}
function Written { $d = [IO.Path]::Combine($tmpRoot, 'expanded'); $n = [long]0; foreach ($f in Get-ChildItem -LiteralPath $d -Recurse -File -Force) { $n += $f.Length }; return $n }
$ab = [Text.Encoding]::ASCII.GetBytes('A' * 4096)
$f = New-RawZip 'raw-ok' @(,@('d/a.txt', $ab, 8, 4096))
Check ((Expand-Result $f) -ceq '' -and (Written) -eq 4096) 'a zip written here unpacks (the test zip writer itself)'
$f = New-RawZip 'raw-stored' @(,@('a.txt', $ab, 0, 1))
$r = Expand-Result $f
Check ($r -ne '' -and (Written) -le 1) "a stored entry declaring 1 byte of 4096: refused, at most 1 byte written ($r)"
$f = New-RawZip 'raw-deflated' @(,@('a.txt', $ab, 8, 1))
$r = Expand-Result $f
# (.NET's reader may itself stop a deflated entry at its declared size: then nothing is refused, but
# no more than that is written either)
Check ((Written) -le 1) "a deflated entry declaring 1 byte of 4096: at most 1 byte written ($r)"
$f = New-RawZip 'raw-short' @(,@('a.txt', $ab, 8, 8192))
$r = Expand-Result $f
Check ($r -like '*isn''t the size the zip declares*') "an entry declaring more than its data: refused ($r)"
$f = New-RawZip 'raw-total' @(@('a.txt', $ab, 8, 4096), @('b.txt', $ab, 8, 4096))
$r = Expand-Result $f 6000
Check ($r -ceq 'the download unpacks too large' -and (Written) -le 6000) "the bytes written in all over the cap: refused ($r)"
$f = New-RawZip 'raw-twice' @(@('a.txt', $ab, 8, 4096), @('a.txt', $ab, 8, 4096))
Check ((Expand-Result $f) -ne '') 'a name twice reaching the unpacking: the second never written over the first'
$f = New-RawZip 'raw-dotdot' @(,@('../x.txt', $ab, 8, 4096))
Check ((Expand-Result $f) -like '*isn''t plain*' -and -not [IO.File]::Exists([IO.Path]::Combine($tmpRoot, 'x.txt'))) 'a ".." name reaching the unpacking: refused, nothing outside'
$main = (Get-Command Invoke-RbspMain).ScriptBlock.ToString()
Check ($main -match 'Test-RbspZip \$zip\s+Expand-RbspZip \$zip \$unpacked' -and $main -notmatch 'Expand-Archive') 'the install unpacks with Expand-RbspZip after Test-RbspZip, never Expand-Archive'

# ------------------------------------------------------------------ -Restore (release.md section 4)
# the restore route runs in ConstrainedLanguage: no [ref], method call, New-Object, Add-Type in it
$restoreFns = @('Test-RbspHighToken', 'Get-RbspProgramFiles', 'Test-RbspReparse', 'Get-RbspSddlMask', 'Test-RbspSddlAdminOnly',
                'Test-RbspAdminOnly', 'Test-RbspRekordboxOpen', 'Test-RbspRestoreBusy', 'Get-RbspFileSha',
                'Test-RbspAlphaTheta', 'Get-RbspSavedSha', 'Restore-RbspVersion', 'Invoke-RbspRestore')
foreach ($fn in $restoreFns) {
    $body = (Get-Command $fn).ScriptBlock.ToString()
    Check (-not ($body -match '\[ref\]|::|New-Object|Add-Type|\.[A-Za-z]+\(')) "$fn uses only what ConstrainedLanguage allows"
}
$tail = $src.Substring($src.IndexOf('# ------------------------------------------------------------------ -Restore'))
$code = ($tail -split "`n" | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"
Check (-not ($code -match '\[ref\]|::|New-Object|Add-Type|\.[A-Za-z]+\(')) 'the restore section and the top level after it use only what ConstrainedLanguage allows'
Check ($src -match '\$restore = \$env:RBSTEMSPLUS_RESTORE -eq ''1''') 'selected by RBSTEMSPLUS_RESTORE=1'
$mainAt = $src.IndexOf('function Invoke-RbspMain')
Check ($src.IndexOf('Test-RbspCanInstall', $mainAt) -gt $mainAt -and (Get-Command Invoke-RbspRestore).ScriptBlock.ToString() -notmatch 'Test-RbspCanInstall|SecurityProtocol') 'the SAC, language and Tls12 lines are on the install route only'

# SDDL: the admin-only rule (win_rules.h wr_acl_ok)
$ti = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'
Check ((Get-RbspSddlMask '0x1200a9') -eq 0x1200a9 -and (Get-RbspSddlMask 'FA') -eq 0x1F01FF -and (Get-RbspSddlMask 'GXGR') -eq 2684354560) 'SDDL masks: hex, letters'
Check ((Get-RbspSddlMask 'ZZ') -eq -1 -and (Get-RbspSddlMask 'fa') -eq -1 -and (Get-RbspSddlMask '') -eq -1) 'SDDL masks: unknown letters unread'
$rbAcl = "O:BAG:SYD:AI(A;OICIIO;GA;;;CO)(A;OICIIO;GA;;;SY)(A;;FA;;;SY)(A;OICIIO;GA;;;BA)(A;;FA;;;BA)(A;OICIIO;GXGR;;;BU)(A;;0x1200a9;;;BU)(A;CIIO;GA;;;$ti)(A;;FA;;;$ti)(A;;0x1200a9;;;AC)(A;OICIIO;GXGR;;;AC)(A;;0x1200a9;;;S-1-15-2-2)(A;OICIIO;GXGR;;;S-1-15-2-2)"
$okAcls = @($rbAcl, 'O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)', "O:${ti}G:${ti}D:P(A;;FA;;;$ti)", 'O:SYD:(A;;FA;;;SY)',
            'O:BAG:SYD:', 'O:BAG:SYD:(D;;FA;;;WD)(A;;FA;;;BA)', 'O:BAG:SYD:(A;OICIIO;GA;;;WD)', 'O:BAG:SYD:(A;;ZZ;;;BA)', 'O:S-1-5-32-544G:SYD:(A;;FA;;;S-1-5-18)')
foreach ($a in $okAcls) { Check (Test-RbspSddlAdminOnly $a) "admin-only: $a" }
$badAcls = @('O:S-1-5-21-1-2-3-1001G:SYD:(A;;FA;;;BA)', 'O:BAG:SYD:(A;;FA;;;BU)', 'O:BAG:SYD:(A;;0x1301bf;;;AU)', 'O:BAG:SYD:(A;;FW;;;BU)',
             'O:BAG:SYD:(A;;0x2;;;WD)', 'O:BAG:SYD:(A;;0x40000;;;BU)', 'O:BAG:SYD:(A;;ZZ;;;BU)', 'O:BAG:SYD:(A;OICI;GA;;;CO)',
             'O:BAG:SYD:(OA;;FA;;;BA)', 'O:BAG:SYD:(XA;;FA;;;WD;(Member_of {SID(BA)}))', 'O:BAG:SYD:NO_ACCESS_CONTROL', 'O:BAG:SY',
             '', 'O:BAG:SYD:(A;;FA;;BU)', 'O:BAG:SYD:(A;IOCI;GA;;;BU)x', 'O:BAG:SYD:(A;CIOI;FA;;;BU)')
foreach ($a in $badAcls) { Check (-not (Test-RbspSddlAdminOnly $a)) "not admin-only: '$a'" }

# the flow on a temp Program Files, with what only Windows answers stubbed: the token's level
# (whoami), running processes, the ACL (Get-Acl's SDDL) and the signature (a file whose text
# starts "PIONEER" is AlphaTheta's, with $script:subject, unless its path is -like
# $script:badSig). Get-Item refuses $script:deniedPath (access denied) and Move-Item throws
# $script:moveFails times (deleting a -Force destination first when $script:moveDeletes, as 5.1
# does). The files, hashes, copies and moves are real.
$realSubject = 'SERIALNUMBER=0104-01-151827, OID.2.5.4.15=Private Organization, CN=AlphaTheta Corporation, O=AlphaTheta Corporation, L=Yokohama, S=Kanagawa, C=JP'
function Reset-Stubs {
    $script:high = $true; $script:procs = @(); $script:aclOf = @{}; $script:subject = $realSubject; $script:badSig = ''
    $script:deniedPath = ''; $script:moveFails = 0; $script:moveDeletes = $false
}
Reset-Stubs
function whoami { if ($script:high) { 'Mandatory Label\High Mandatory Level   Label   S-1-16-12288' } else { 'Mandatory Label\Medium Mandatory Level   Label   S-1-16-8192' } }
function Get-Process([string[]]$Name, $ErrorAction) { $script:procs | Where-Object { $Name -contains $_.ProcessName } }
function Get-Acl([string]$LiteralPath) {
    $s = 'O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)'
    if ($script:aclOf.ContainsKey($LiteralPath)) { $s = $script:aclOf[$LiteralPath] }
    return [pscustomobject]@{ Sddl = $s }
}
function Get-AuthenticodeSignature([string]$LiteralPath) {
    if ((-not $script:badSig -or $LiteralPath -notlike $script:badSig) -and [IO.File]::ReadAllText($LiteralPath).StartsWith('PIONEER')) {
        return [pscustomobject]@{ Status = 'Valid'; SignerCertificate = [pscustomobject]@{ Subject = $script:subject } }
    }
    return [pscustomobject]@{ Status = 'NotSigned'; SignerCertificate = $null }
}
function Get-Item {
    [CmdletBinding()] param([string]$LiteralPath, [switch]$Force)
    if ($script:deniedPath -and $LiteralPath -eq $script:deniedPath) {
        $e = New-Object Management.Automation.ErrorRecord ((New-Object UnauthorizedAccessException "Access to the path '$LiteralPath' is denied."),
             'ItemExistsUnauthorizedAccessError,Microsoft.PowerShell.Commands.GetItemCommand', 'PermissionDenied', $LiteralPath)
        $PSCmdlet.ThrowTerminatingError($e)
    }
    Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath -Force:$Force
}
function Move-Item([string]$LiteralPath, [string]$Destination, [switch]$Force) {
    if ($script:moveFails -gt 0) {
        $script:moveFails--
        if ($Force -and $script:moveDeletes -and [IO.File]::Exists($Destination)) { [IO.File]::Delete($Destination) }
        throw 'the move failed (test)'
    }
    Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force
}
$rsRoot = [IO.Path]::Combine($tmpRoot, 'restore')
if (-not $onWindows) { [void][IO.Directory]::CreateDirectory($tmpRoot); $rsRoot = [IO.Path]::Combine((& realpath $tmpRoot), 'restore') }   # /var, /tmp: links on the Mac
$pf = [IO.Path]::Combine($rsRoot, 'pf')
$set = [IO.Path]::Combine($pf, 'RB Stems Plus', 'pioneer', '7.2.19')
$dst = [IO.Path]::Combine($pf, 'rekordbox', 'rekordbox 7.2.19')
$live = [IO.Path]::Combine($dst, 'onnxruntime.dll')
$pioneerText = 'PIONEER onnxruntime 7.2.19'
$pioneerSha = Sha ([Text.Encoding]::ASCII.GetBytes($pioneerText))
function Reset-Restore([string]$liveText = 'rbstems bridge: the bridge') {
    Reset-Stubs
    foreach ($l in @($dst, $set)) { if ((Test-Path -LiteralPath $l) -and [string](Get-Item -LiteralPath $l -Force).Attributes -match 'ReparsePoint') { [IO.Directory]::Delete($l) } }
    if ([IO.Directory]::Exists($rsRoot)) { [IO.Directory]::Delete($rsRoot, $true) }
    [void][IO.Directory]::CreateDirectory($set); [void][IO.Directory]::CreateDirectory($dst)
    [IO.File]::WriteAllText([IO.Path]::Combine($set, 'onnxruntime.dll'), $pioneerText)
    [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), "$pioneerSha  onnxruntime.dll`n")
    if ($liveText) { [IO.File]::WriteAllText($live, $liveText) }
}
function Restore-Says { try { return ((Invoke-RbspRestore $pf 6>&1 | ForEach-Object { [string]$_ }) -join ' | ') } catch { return "THROW $($_.Exception.Message)" } }
function Live { if ([IO.File]::Exists($live)) { return [IO.File]::ReadAllText($live) }; return '' }
function Leftovers { return @(Get-ChildItem -LiteralPath $dst -Force -File | Where-Object { $_.Name -like 'onnxruntime.dll?*' }).Count }

Reset-Restore
$r = Restore-Says
Check ($r -ceq "Pioneer's file is back in rekordbox 7.2.19." -and (Live) -ceq $pioneerText -and (Leftovers) -eq 0) "-Restore: the bridge replaced by Pioneer's file, no temporary copy left ($r)"
$r = Restore-Says
Check ($r -ceq "rekordbox 7.2.19 has Pioneer's file already." -and (Live) -ceq $pioneerText) "-Restore again: Pioneer's file already there, left ($r)"
Reset-Restore ''
$r = Restore-Says
Check ($r -ceq "Pioneer's file is back in rekordbox 7.2.19." -and (Live) -ceq $pioneerText) "-Restore with the live DLL missing (R4.I71): moved into its place ($r)"
Reset-Restore
[IO.File]::WriteAllText([IO.Path]::Combine($dst, 'onnxruntime.dll.rbsp-new'), 'a stopped run')
$r = Restore-Says
Check ((Live) -ceq $pioneerText -and (Leftovers) -eq 0) "-Restore with a stopped run's temporary file: removed, restored ($r)"
# the refusals: the live DLL left as it was
function Restore-Refused([scriptblock]$bend, [string]$want, [string]$what) {
    Reset-Restore
    & $bend
    $r = Restore-Says
    Check ($r -like "THROW $want" -and (Live) -ceq 'rbstems bridge: the bridge' -and (Leftovers) -eq 0) "-Restore refused: $what ($r)"
}
Restore-Refused { $script:high = $false } $Texts.RestoreElevated 'not elevated (no S-1-16-12288)'
Restore-Refused { $script:procs = @([pscustomobject]@{ ProcessName = 'rekordbox' }) } $Texts.RestoreOpen 'rekordbox open'
$busy = 'Finish updating or uninstalling rekordbox, or wait for RB Stems Plus to finish, then run the restore command again.'
$workerRoot = Join-Path $pf 'RB Stems Plus'
$tempCopy = Join-Path $dst 'onnxruntime.dll.rbsp-new'
foreach ($p in @(@{ ProcessName = 'Upmgr rekordbox' }, @{ ProcessName = 'Uninstall rekordbox' },
                 @{ ProcessName = 'RB Stems Plus'; Path = "$($workerRoot.ToUpperInvariant())\bin\RB Stems Plus.exe" })) {
    Reset-Restore
    $script:procs = @([pscustomobject]$p)
    [IO.File]::WriteAllText($tempCopy, 'a running process may still need this')
    $r = Restore-Says
    Check ($r -ceq "THROW $busy" -and (Live) -ceq 'rbstems bridge: the bridge' -and
           (Test-Path -LiteralPath $tempCopy) -and [IO.File]::ReadAllText($tempCopy) -ceq 'a running process may still need this') "-Restore refused: $($p.ProcessName) running (BS12) ($r)"
}
Reset-Restore
$script:procs = @([pscustomobject]@{ ProcessName = 'RB Stems Plus'; Path = "$workerRoot-other\RB Stems Plus.exe" })
$r = Restore-Says
Check ((Live) -ceq $pioneerText) "-Restore with RB Stems Plus running outside its Program Files folder (BS12) ($r)"
Restore-Refused { [IO.Directory]::Delete($set, $true) } $Texts.RestoreNone 'no saved set'
Restore-Refused { [IO.Directory]::Move($set, [IO.Path]::Combine($pf, 'RB Stems Plus', 'pioneer', 'x7.2.19')) } $Texts.RestoreNone 'a saved set whose name is not a version'
Restore-Refused { [IO.Directory]::Move($set, [IO.Path]::Combine($pf, 'RB Stems Plus', 'pioneer', '7.2.18')) } $Texts.RestoreNone 'a saved set of a version not installed'
Restore-Refused { [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), "$('0' * 64)  onnxruntime.dll`n") } '*isn''t complete' 'SHA256SUMS names another file'
Restore-Refused { [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), "$($pioneerSha.ToUpperInvariant())  onnxruntime.dll`n") } '*isn''t complete' 'SHA256SUMS in upper case'
Restore-Refused { [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), "$pioneerSha  onnxruntime.dll") } '*isn''t complete' 'SHA256SUMS without its line end'
Restore-Refused { [IO.File]::Delete([IO.Path]::Combine($set, 'SHA256SUMS')) } '*isn''t complete' 'no SHA256SUMS'
Restore-Refused { $t = 'not Pioneer''s'; [IO.File]::WriteAllText([IO.Path]::Combine($set, 'onnxruntime.dll'), $t)
          [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), "$(Sha ([Text.Encoding]::ASCII.GetBytes($t)))  onnxruntime.dll`n") } '*isn''t AlphaTheta''s' 'a saved copy that isn''t AlphaTheta''s'
Restore-Refused { $script:aclOf[$dst] = 'O:BAG:SYD:(A;;FA;;;BA)(A;;0x1301bf;;;AU)' } '*can be changed by other accounts' 'rekordbox''s folder writable by users'
Restore-Refused { $script:aclOf[$set] = 'O:S-1-5-21-1-2-3-1001D:(A;;FA;;;BA)' } '*can be changed by other accounts' 'the saved set''s folder owned by a user'
# a link in the path (I23): the version folder (a junction on Windows), then the saved set's folder
$linkType = if ($onWindows) { 'Junction' } else { 'SymbolicLink' }
Restore-Refused { [IO.Directory]::Move($dst, "$dst.real"); [void](New-Item -ItemType $linkType -Path $dst -Target "$dst.real")
          [IO.File]::WriteAllText($live, 'rbstems bridge: the bridge') } '*reached through a link' 'rekordbox''s version folder a link'
Check ([IO.File]::ReadAllText([IO.Path]::Combine("$dst.real", 'onnxruntime.dll')) -ceq 'rbstems bridge: the bridge') '-Restore through a link: the real folder untouched'
Restore-Refused { [IO.Directory]::Move($set, "$set.real"); [void](New-Item -ItemType $linkType -Path $set -Target "$set.real") } '*reached through a link' 'the saved set''s folder a link'
if (-not $onWindows) {   # a file link needs an administrator or developer mode on Windows
    Restore-Refused { $s = [IO.Path]::Combine($set, 'SHA256SUMS'); [IO.File]::Move($s, "$s.real"); [void](New-Item -ItemType SymbolicLink -Path $s -Target "$s.real") } '*reached through a link' 'SHA256SUMS a link (BS8)'
}
# BS1: the folders above the version folders, which the worker holds too, and a path the reparse
# check can't read (access denied: refused, not passed over)
$ours = [IO.Path]::Combine($pf, 'RB Stems Plus')
foreach ($d in @($ours, [IO.Path]::Combine($ours, 'pioneer'), [IO.Path]::Combine($pf, 'rekordbox'))) {
    Restore-Refused { $script:aclOf[$d] = 'O:BAG:SYD:(A;;FA;;;BA)(A;;0x1301bf;;;AU)' } ($Texts.RestoreOthers -f $d) "$d writable by users (BS1)"
}
Reset-Restore
$script:deniedPath = $dst
Check ((Test-RbspReparse $live) -and -not (Test-RbspReparse ([IO.Path]::Combine($set, 'none')))) 'Test-RbspReparse: access denied on a folder in the path counts as a link, a missing path doesn''t (BS1)'
Restore-Refused { $script:deniedPath = $dst } '*reached through a link' 'rekordbox''s version folder unreadable (BS1)'
Restore-Refused { $script:deniedPath = $ours } ($Texts.RestoreLinked -f $ours) 'our folder unreadable (BS1)'
# BS2: the live file and the temporary copy admin-only too (worker.c file_admin_only)
Restore-Refused { $script:aclOf[$live] = 'O:BAG:SYD:(A;;FA;;;BA)(A;;FA;;;BU)' } '*rekordbox 7.2.19''s ONNX Runtime can be changed by other accounts' 'the live file writable by users (BS2)'
Restore-Refused { $script:aclOf[[IO.Path]::Combine($dst, 'onnxruntime.dll.rbsp-new')] = 'O:BAG:SYD:(A;;FA;;;BA)(A;;FA;;;BU)' } '*the copy of Pioneer''s file in rekordbox 7.2.19 can be changed by other accounts' 'the copy writable by users (BS2)'
# BS8: SHA256SUMS exactly 82 bytes, so a BOM or UTF-16 is refused
$sumsLine = "$pioneerSha  onnxruntime.dll`n"
Restore-Refused { [IO.File]::WriteAllBytes([IO.Path]::Combine($set, 'SHA256SUMS'), [byte[]](@(0xEF, 0xBB, 0xBF) + [Text.Encoding]::ASCII.GetBytes($sumsLine))) } '*isn''t complete' 'SHA256SUMS with a UTF-8 BOM (BS8)'
Restore-Refused { [IO.File]::WriteAllText([IO.Path]::Combine($set, 'SHA256SUMS'), $sumsLine, [Text.Encoding]::Unicode) } '*isn''t complete' 'SHA256SUMS in UTF-16 (BS8)'
# BS6 with BS4: the live file classified before the saved set is read
Reset-Restore 'PIONEER onnxruntime 7.2.19 another build'
[IO.File]::Delete([IO.Path]::Combine($set, 'SHA256SUMS'))
$r = Restore-Says
Check ($r -ceq "rekordbox 7.2.19 has Pioneer's file already." -and (Live) -ceq 'PIONEER onnxruntime 7.2.19 another build' -and (Leftovers) -eq 0) "-Restore: another Pioneer file left, the saved copy not written, the saved set not needed (BS6) ($r)"
Reset-Restore ''
[void][IO.Directory]::CreateDirectory($live)
$r = Restore-Says
Check ($r -ceq "THROW rekordbox 7.2.19: $($Texts.RestoreNotFile -f '7.2.19')" -and @(Get-ChildItem -LiteralPath $live -Force).Count -eq 0 -and (Leftovers) -eq 0) "-Restore: a folder named onnxruntime.dll refused, nothing put in it (BS4) ($r)"
# BS7: a version that fails doesn't stop the others
Reset-Restore
$set10 = [IO.Path]::Combine($pf, 'RB Stems Plus', 'pioneer', '7.10.0'); $dst10 = [IO.Path]::Combine($pf, 'rekordbox', 'rekordbox 7.10.0')
[void][IO.Directory]::CreateDirectory($set10); [void][IO.Directory]::CreateDirectory($dst10)
[IO.File]::WriteAllText([IO.Path]::Combine($set10, 'onnxruntime.dll'), 'PIONEER onnxruntime 7.10.0')
[IO.File]::WriteAllText([IO.Path]::Combine($dst10, 'onnxruntime.dll'), 'rbstems bridge: the bridge')
$r = Restore-Says
Check ($r -ceq "THROW rekordbox 7.10.0: the saved copy of rekordbox 7.10.0's file isn't complete" -and (Live) -ceq $pioneerText -and
       [IO.File]::ReadAllText([IO.Path]::Combine($dst10, 'onnxruntime.dll')) -ceq 'rbstems bridge: the bridge') "-Restore: 7.2.19 restored past an incomplete 7.10.0, which is named (BS7) ($r)"
# BS5: the signer's subject read field by field
Reset-Restore
$savedDll = [IO.Path]::Combine($set, 'onnxruntime.dll')
Check (Test-RbspAlphaTheta $savedDll) 'AlphaTheta pin: the real subject'
foreach ($s in @('CN="x, O=AlphaTheta Corporation, SERIALNUMBER=0104-01-151827", O=Other, SERIALNUMBER=1',
                 'SERIALNUMBER=0104-01-151827, CN=AlphaTheta Corporation + O=AlphaTheta Corporation, C=JP',
                 'SERIALNUMBER=0104-01-151827, O=AlphaTheta Corporation, O=Other, C=JP',
                 'SERIALNUMBER=0104-01-151827, SERIALNUMBER=0104-01-151827, O=AlphaTheta Corporation',
                 'SERIALNUMBER=0104-01-151827, O=AlphaTheta Corporation Ltd, C=JP', 'O=AlphaTheta Corporation, C=JP',
                 'serialNumber=0104-01-151827, O=AlphaTheta Corporation')) {
    $script:subject = $s
    Check (-not (Test-RbspAlphaTheta $savedDll)) "AlphaTheta pin refused: $s (BS5)"
}
# BS16 and BS3: the failures after the copy, the message and the files on disk
function Restore-Fails([scriptblock]$bend, [string]$liveText, [string]$want, [string]$liveAfter, [string]$what) {
    Reset-Restore $liveText
    & $bend
    $r = Restore-Says
    Check ($r -like "THROW rekordbox 7.2.19: $want" -and (Live) -ceq $liveAfter -and (Leftovers) -eq 0) "-Restore: $what ($r)"
}
$bridge = 'rbstems bridge: the bridge'
Restore-Fails { $script:badSig = '*.rbsp-new' } $bridge '*the copy of Pioneer''s file in rekordbox 7.2.19 doesn''t verify' $bridge 'the copy doesn''t verify, the live file unchanged (BS16)'
Restore-Fails { $script:moveFails = 1 } $bridge 'couldn''t put Pioneer''s file into rekordbox 7.2.19 (the move failed (test))' $bridge 'the forced move throws, the live file there (BS16)'
Restore-Fails { $script:moveFails = 1 } '' '*where a file may have appeared meanwhile (the move failed (test))' '' 'the move into a missing file''s place throws (BS16)'
Restore-Fails { $script:badSig = $live } $bridge '*doesn''t verify as Pioneer''s after putting it back' $pioneerText 'the live signature rejected after the move (BS16)'
Restore-Fails { $script:moveFails = 2; $script:moveDeletes = $true } $bridge ($Texts.RestoreGone -f '7.2.19', 'the move failed (test); the move failed (test)') '' 'the forced move deletes the live file and fails, the retry too (BS3)'
Reset-Restore
$script:moveFails = 1; $script:moveDeletes = $true
$r = Restore-Says
Check ($r -ceq "Pioneer's file is back in rekordbox 7.2.19." -and (Live) -ceq $pioneerText -and (Leftovers) -eq 0) "-Restore: the forced move deletes the live file and fails, the retry without -Force puts it back (BS3) ($r)"
Reset-Restore
$ok = $true
try { [void](Get-RbspProgramFiles) } catch { $ok = $false }
if ($onWindows) { Check ($ok) 'Program Files read from the registry' } else { Check (-not $ok) 'Program Files: none off Windows, refused' }
# BS14: ProgramW6432Dir first (an x86 powershell.exe reads it too), ProgramFilesDir only in a
# process of the machine's own width
$script:hklm = @{}
function Get-ItemProperty([string]$LiteralPath, [string]$Name, $ErrorAction) { if ($script:hklm.ContainsKey($Name)) { [pscustomobject]@{ $Name = $script:hklm[$Name] } } }
function Program-Files([hashtable]$values, [string]$wow) {
    $script:hklm = $values
    $savedWow = $env:PROCESSOR_ARCHITEW6432
    $env:PROCESSOR_ARCHITEW6432 = $wow
    try { return Get-RbspProgramFiles } catch { return "THROW $($_.Exception.Message)" } finally { $env:PROCESSOR_ARCHITEW6432 = $savedWow }
}
Check ((Program-Files @{ ProgramW6432Dir = 'C:\Program Files'; ProgramFilesDir = 'C:\Program Files (x86)' } 'AMD64') -ceq 'C:\Program Files') 'Program Files: ProgramW6432Dir first, in an x86 process (BS14)'
Check ((Program-Files @{ ProgramFilesDir = 'C:\Program Files' } '') -ceq 'C:\Program Files') 'Program Files: ProgramFilesDir when there is no ProgramW6432Dir (BS14)'
Check ((Program-Files @{ ProgramFilesDir = 'C:\Program Files (x86)' } 'AMD64') -like 'THROW *') 'Program Files: ProgramFilesDir refused in a 32-bit process on 64-bit Windows (BS14)'
Check ((Program-Files @{ ProgramW6432Dir = 'Program Files' } '') -like 'THROW *') 'Program Files: a relative value refused (BS14)'
Remove-Item function:Get-ItemProperty, function:Program-Files

# the one-liner with RBSTEMSPLUS_RESTORE=1 in a ConstrainedLanguage child: unelevated (the Mac has
# no S-1-16-12288), its refusal alone; then the whole restore in a ConstrainedLanguage child, its
# functions defined under iex after the switch, with the four Windows answers stubbed first
function Invoke-Child([string]$cmd, [hashtable]$envs) {
    $exe = if ($onWindows) { 'powershell' } else { 'pwsh' }
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($cmd))
    $saved = @{}
    foreach ($k in $envs.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envs[$k]) }
    $savedEap = $ErrorActionPreference
    $o = ''
    try {
        $ErrorActionPreference = 'Continue'
        $o = (& $exe -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand $enc 2>&1 | Out-String)
    } finally { $ErrorActionPreference = $savedEap; foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
    return ($o -replace "$([char]27)\[[0-9;?]*[A-Za-z]", '').Trim()
}
if (-not $onWindows) {
    $out = Invoke-Child ('$ProgressPreference = ''SilentlyContinue''; $ExecutionContext.SessionState.LanguageMode = ''ConstrainedLanguage''; ' +
                         'Get-Content -Raw -LiteralPath $env:RBSP_BOOT_PATH | iex; "left: [$env:RBSTEMSPLUS_RESTORE]"') @{ RBSP_BOOT_PATH = $path; RBSTEMSPLUS_RESTORE = '1' }
    $lines = @($out -split '\r?\n')
    Check ($lines.Count -eq 2 -and $lines[0] -ceq $Texts.RestoreElevated) "-Restore under iex in ConstrainedLanguage, unelevated: its refusal alone (got: $out)"
    Check ($lines.Count -eq 2 -and $lines[1] -ceq 'left: []') "-Restore: RBSTEMSPLUS_RESTORE removed once read, so the next paste installs (BS15) (got: $out)"
}
Reset-Restore
$stubs = @'
function global:whoami { 'Mandatory Label\High Mandatory Level   Label   S-1-16-12288' }
function global:Get-Process([string[]]$Name, $ErrorAction) {
    if ($env:RBSP_WORKER_PATH -and $Name -contains 'RB Stems Plus') {
        [pscustomobject]@{ ProcessName = 'RB Stems Plus'; Path = $env:RBSP_WORKER_PATH }
    }
}
function global:Get-Acl([string]$LiteralPath) { [pscustomobject]@{ Sddl = 'O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)' } }
function global:Get-AuthenticodeSignature([string]$LiteralPath) {
    if ([IO.File]::ReadAllText($LiteralPath).StartsWith('PIONEER')) { return [pscustomobject]@{ Status = 'Valid'; SignerCertificate = [pscustomobject]@{ Subject = 'SERIALNUMBER=0104-01-151827, CN=AlphaTheta Corporation, O=AlphaTheta Corporation, C=JP' } } }
    [pscustomobject]@{ Status = 'NotSigned'; SignerCertificate = $null }
}
$src = Get-Content -Raw -LiteralPath $env:RBSP_BOOT_PATH
$at = $src.IndexOf("`n& {`n")
$src = $src.Substring(0, $at) + "`n. {`n" + $src.Substring($at + 5)
$global:RbspTestMode = $true
# Get-FileHash is a script function of this module on 5.1: loaded after the switch it would load in
# ConstrainedLanguage (no system lockdown here) and fail. Loaded first, in FullLanguage, as WDAC
# loads the Microsoft-signed module on a real SAC/WDAC PC (BS11).
Import-Module Microsoft.PowerShell.Utility
$ExecutionContext.SessionState.LanguageMode = 'ConstrainedLanguage'
iex $src
"mode: $($ExecutionContext.SessionState.LanguageMode)"
try { Invoke-RbspRestore $env:RBSP_PF } catch { "THROW $($_.Exception.Message)" }
'@
$out = Invoke-Child $stubs @{ RBSP_BOOT_PATH = $path; RBSP_PF = $pf; RBSP_WORKER_PATH = "$workerRoot\RB Stems Plus.exe" }
Check ($out -match 'mode: ConstrainedLanguage' -and $out -like "*THROW $busy" -and (Live) -ceq $bridge) "-Restore in a ConstrainedLanguage child: running worker refused (BS12) (got: $out)"
Reset-Restore
$out = Invoke-Child $stubs @{ RBSP_BOOT_PATH = $path; RBSP_PF = $pf }
Check ($out -match 'mode: ConstrainedLanguage' -and $out -match 'Pioneer''s file is back in rekordbox 7\.2\.19\.' -and (Live) -ceq $pioneerText -and (Leftovers) -eq 0) "-Restore in a ConstrainedLanguage child: restored (got: $out)"
Remove-Item function:whoami, function:Get-Process, function:Get-Acl, function:Get-AuthenticodeSignature, function:Get-Item, function:Move-Item
if ([IO.Directory]::Exists("$dst.real")) { [IO.Directory]::Delete("$dst.real", $true) }

# the shortcut's C# compiles (it runs only on Windows)
$compiled = $true
try { Add-Type -TypeDefinition $ShortcutCode -Language CSharp } catch { $compiled = $false; Write-Host $_.Exception.Message }
Check ($compiled -and ('RbspShortcut' -as [type])) 'the shortcut code compiles'

[IO.Directory]::Delete($tmpRoot, $true)
Write-Host "bootstrap.ps1: $script:checks checks, $script:failures failed"
if ($script:failures) { throw 'bootstrap.ps1 tests failed' }
