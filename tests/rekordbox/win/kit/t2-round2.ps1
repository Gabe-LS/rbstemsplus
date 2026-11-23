# T2 round 2. Only the named settings and probe evidence are reported.
function Read-T2Setting([string]$settings, [string]$name) {
    $match = [regex]::Matches($settings, ('\bname="' + [regex]::Escape($name) + '"\s+val="([01])"'))
    if ($match.Count -ne 1) { throw "rekordbox setting $name is missing or ambiguous" }
    return $match[0].Groups[1].Value
}
function Get-T2Facts {
    $settings = Get-Content -LiteralPath (Join-Path $env:APPDATA 'Pioneer\rekordbox6\rekordbox3.settings') -Raw
    $quality = Read-T2Setting $settings 'PartAnalysisQuality'
    $auto = Read-T2Setting $settings 'AutoAnalysis'
    Say "T2 item 8: PartAnalysisQuality=$quality AutoAnalysis=$auto"
    if ($auto -ne '0') { throw 'turn off Auto Analysis in rekordbox before loading the test corpus' }
    if ($quality -ne '0') { throw 'select Prioritize sound quality in rekordbox before T2' }
    $rb = Get-Rb
    $knownExe = @{
        '7.2.19' = 'c9ed23e974c51c75e2ec069fbd2679a3dbe606be9e79e9cbbd13d6418ab11c37'
        '7.2.20' = '37d2b6a57770a42d9604d8872b1d8dae4f1fffa15483189a92ba58f9dd4de9ef'
    }
    if (-not $rb -or -not $knownExe.ContainsKey($rb.Version)) { throw 'T2 inventory expects rekordbox 7.2.19 or 7.2.20' }
    $known = (Get-FileHash -LiteralPath $rb.Exe -Algorithm SHA256).Hash -eq $knownExe[$rb.Version]
    $dest = Join-Path $B 't2\rb-orig'
    [void][IO.Directory]::CreateDirectory($dest)
    Say "T2 item 2: rekordbox $($rb.Version) DLLs"
    Say "T2 item 2: known rekordbox executable hash: $known"
    foreach ($file in Get-ChildItem -LiteralPath $rb.Dir -Filter '*.dll' -File | Sort-Object Name) {
        Say ("T2 DLL: " + $file.Name)
    }
    foreach ($name in 'libmpg123.dll', 'libfaad2.dll') {
        $path = Join-Path $rb.Dir $name
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $version = (Get-Item -LiteralPath $path).VersionInfo.FileVersion
            Say "T2 item 2: $name file version $(if ($version) { $version } else { '(none)' })"
            Copy-Item -LiteralPath $path -Destination $dest -Force
        } else {
            Say "T2 item 2: $name absent"
            $previous = Join-Path $dest $name
            if (Test-Path -LiteralPath $previous) { Remove-Item -LiteralPath $previous }
        }
    }
    Say "T2 item 6: NUMBER_OF_PROCESSORS=$env:NUMBER_OF_PROCESSORS"
}
function Get-T2ProbeEvidence {
    $probeLog = 'C:\rbtest\probe.log'
    $lines = @(Get-Content -LiteralPath $probeLog)
    if (-not @($lines -match '\bMODEL\s+[0-9a-f]+\s+\S+\s+ours\b').Count) { throw 'the probe never recorded the Demucs v4 model as ours' }
    foreach ($name in 'AppendExecutionProvider_DML|SessionOptionsAppendExecutionProvider', 'SetIntraOpNumThreads|SetInterOpNumThreads') {
        $found = @($lines -match $name)
        if ($found.Count) { foreach ($line in $found) { Say "T2 probe: $line" } }
        else { Say "T2 probe: no line matching $name" }
    }
    $dst = Join-Path $B 't2'
    [void][IO.Directory]::CreateDirectory($dst)
    Copy-Item -LiteralPath $probeLog, 'C:\rbtest\rows.txt' -Destination $dst -Force
}
function Set-T2Model([bool]$enable) {
    # Demucs is the per-account model path. The Stems Cache worker has already restored
    # rekordbox's DLL. Only the app's own named Manage confirmation is answered here.
    . (Join-Path $Kit 'm12.ps1')
    Uia
    M12-Desktop
    $p = @{ App = (M12-Current) }
    Assert-M12 ($p.App -match '^\d{1,9}\.\d{1,9}\.\d{1,9}$') 'invalid installed app version'
    Assert-M12 (-not @(M12-Apps (M12-VersionExe $p.App)).Count) 'close RB Stems Plus before changing models'
    [void](Start-Process -FilePath $M12Stub)
    $app = M12-App $p
    $CT = [Windows.Automation.ControlType]
    M12-Desktop
    Assert-M12 (Select-El (Find-El $app 'Manage' $CT::TabItem 5)) 'Manage tab missing'
    $button = if ($enable) { 'Install Demucs v4' } else { 'Uninstall Demucs v4' }
    $reply = if ($enable) { 'Install' } else { 'Uninstall' }
    $result = if ($enable) { 'Uninstall Demucs v4' } else { 'Install Demucs v4' }
    # already as asked (a plan that turns it on before its own steps): nothing pressed
    if ((Find-El $app $result $CT::Button 5 -Enabled) -and -not (Find-El $app $button $CT::Button 0 -Enabled)) {
        M12-Close $app
        return "t2-model-$(if ($enable) { 'on' } else { 'off' }): OK already ($result available)"
    }
    $el = Find-El $app $button $CT::Button 5 -Enabled
    M12-Desktop
    Assert-M12 (Invoke-El $el) "expected '$button' in Manage"
    M12-Dialog $app "${button}?" $reply 15   # its confirmation's UI Automation name ends in "?" (the PC, 2026-11-19)
    Wait-M12 140 { [bool](Find-El $app $result $CT::Button 0 -Enabled) } "expected '$result' after changing the model"
    M12-Close $app
    "t2-model-$(if ($enable) { 'on' } else { 'off' }): OK $button completed"
}
