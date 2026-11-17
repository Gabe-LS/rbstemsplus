# What the shipped worker left, read as SYSTEM (the root's DACL is the worker's: the user may not
# read it all): <Program Files>\RB Stems Plus\ owned by Administrators, no write for the user's
# groups; one worker in bin\, named by the payload's admin_sha256's first 12 hex digits and with
# its bytes; Pioneer's real ORT in ort\ and its saved original under pioneer\ with SHA256SUMS;
# rekordbox's onnxruntime.dll the payload's bridge (app.md 5.5).
$f = Read-WorkerFacts
if (-not $f) { Skip 'no facts from the prep step (it failed: its log)' }
if (-not (Test-Path -LiteralPath $CacheRoot)) { Check $false "Stems Cache's root is there ($CacheRoot)"; Done }
$p = Read-TestPayload
$worker = ([string]$p.windows.app.admin_sha256).ToLowerInvariant()
$bridge = ([string]$p.windows.bridge.sha256).ToLowerInvariant()
$acl = Get-Acl -LiteralPath $CacheRoot
$owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
Check ($owner -eq 'S-1-5-32-544') "the root is owned by Administrators ($owner)"
# Users, Authenticated Users, Everyone, INTERACTIVE: none may write, delete or change the ACL
$writes = [Security.AccessControl.FileSystemRights]'WriteData, AppendData, WriteExtendedAttributes, WriteAttributes, Delete, DeleteSubdirectoriesAndFiles, ChangePermissions, TakeOwnership'
$open = @($acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | Where-Object {
        $_.AccessControlType -eq 'Allow' -and @('S-1-5-32-545', 'S-1-5-11', 'S-1-1-0', 'S-1-5-4') -contains $_.IdentityReference.Value -and
        ($_.FileSystemRights -band $writes) })
Check ($open.Count -eq 0) "no write for the user's groups on the root ($(@($open | ForEach-Object { "$($_.IdentityReference) $($_.FileSystemRights)" }) -join '; '))"
$bin = Join-Path $CacheRoot 'bin'
$dirs = @(Get-ChildItem -LiteralPath $bin -Directory -Force -ErrorAction SilentlyContinue | ForEach-Object Name)
Check ($dirs.Count -eq 1 -and $dirs[0] -eq $worker.Substring(0, 12)) "one worker folder in bin\, admin_sha256's first 12 hex digits ($($dirs -join ', '))"
$wexe = Join-Path $bin "$($worker.Substring(0, 12))\RB Stems Plus.exe"
Check ((Get-Sha $wexe) -eq $worker) "the bin\ worker is the payload's (admin_sha256)"
Check ((Get-Sha (Join-Path $CacheRoot 'ort\onnxruntime.dll')) -eq $f.PioneerSha) "ort\onnxruntime.dll is Pioneer's real ORT (prep's hash)"
$saved = @(Get-ChildItem -LiteralPath (Join-Path $CacheRoot 'pioneer') -Recurse -File -Force -Filter 'onnxruntime.dll' -ErrorAction SilentlyContinue |
        Where-Object { (Get-Sha $_.FullName) -eq $f.PioneerSha })
Check ($saved.Count -ge 1) "Pioneer's onnxruntime.dll saved under pioneer\ ($(@($saved | ForEach-Object { $_.FullName.Substring($CacheRoot.Length) }) -join ', '))"
Check (@($saved | Where-Object { Test-Path -LiteralPath (Join-Path $_.DirectoryName 'SHA256SUMS') }).Count -ge 1) 'with its SHA256SUMS beside it'
Check ((Get-Sha $RbOrt) -eq $bridge) "rekordbox 7.2.19's onnxruntime.dll is the payload's bridge"
Done
