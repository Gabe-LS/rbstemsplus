# A carried release (release.md 1.3, section 7): its windows.app.version is the installed app's,
# so no update is offered and nothing is downloaded but payload.json; the Apps & features
# DisplayVersion and the installed-release record move to the new payload_version; no new
# version folder. The saved payload is aged two days first, so the start's check is due.
$l = Get-Layout
$p = Get-Content -Raw -LiteralPath (Share 'dist-test/carried/payload.json') | ConvertFrom-Json
$own = (Get-Content -Raw -LiteralPath $l.Current).Trim()
if ($p.windows.app.version -ne $own) { Skip "dist-test/carried isn't carried: its windows.app.version $($p.windows.app.version), the app $own" }
$folders = @(Get-ChildItem -LiteralPath $l.Apps -Directory | ForEach-Object Name) -join ','
$saved = Join-Path $l.Support 'payload\payload.json'
(Get-Item -LiteralPath $saved).LastWriteTime = (Get-Date).AddDays(-2)
$app = Start-Process -FilePath $l.Stub -PassThru
$dv = { (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus' -ErrorAction SilentlyContinue).DisplayVersion }
Check (Wait-Until { (& $dv) -eq $p.payload_version } 180) "DisplayVersion moved to the carried release $($p.payload_version) (now $(& $dv))"
$proc = $null
Check (Wait-Until { $script:proc = Get-AppWindow; $null -ne $script:proc } 60) 'the app opened (its window)'
if ($proc) {
    # the whole notice, {version} any version: never the welcome's or the title's "RB Stems Plus "
    $notice = [regex]::Escape((Get-CoreText 'T_UPDATE_NOTICE')).Replace('\{version}', '[0-9][0-9.]*')
    Check (-not (Find-UiText $proc.Id $notice 10)) 'no update offered'
}
$ir = Get-Content -Raw -LiteralPath (Join-Path $l.Support 'installed-release\payload.json') | ConvertFrom-Json
Check ($ir.payload_version -eq $p.payload_version) "the installed-release record is the carried payload ($($ir.payload_version))"
Check ((@(Get-ChildItem -LiteralPath $l.Apps -Directory | ForEach-Object Name) -join ',') -eq $folders) 'no new version folder (nothing downloaded but payload.json)'
Stop-App
Done
