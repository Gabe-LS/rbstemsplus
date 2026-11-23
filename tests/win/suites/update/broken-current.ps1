# UP9: the shipped start stub refuses each broken pointer, without launching a versioned app.
$l = Get-Layout
$original = [IO.File]::ReadAllBytes($l.Current)
$message = "RB Stems Plus couldn't be opened because its installation is incomplete. Install it again."
$stub = $null
try {
    foreach ($value in 'missing', 'bad/version', '999999999.0.0') {
        if ($value -eq 'missing') { Remove-Item -LiteralPath $l.Current }
        else { Set-Content -LiteralPath $l.Current -Value $value -Encoding ASCII }
        $stub = Start-Process -FilePath $l.Stub -PassThru
        Expect (Find-UiText $stub.Id ([regex]::Escape($message)) 30) "current ($value): the stub shows the incomplete-install message"
        Check (@(Get-AppProcesses | Where-Object { $_.ProcessName -eq 'RB Stems Plus' -and $_.Id -ne $stub.Id }).Count -eq 0) 'no versioned app was launched'
        Use-UpdateUi invoke --pid $stub.Id --title (Get-CoreText 'T_APP_NAME') (Get-CoreText 'T_OK') --type Button --timeout 15
        Expect ($stub.WaitForExit((Get-WaitSecs 15) * 1000) -and $stub.ExitCode -eq 1) 'the stub exited with its refusal code'
    }
} finally {
    [IO.File]::WriteAllBytes($l.Current, $original)
    if ($stub -and -not $stub.HasExited) { Stop-Process -Id $stub.Id -Force }
}
[void](Start-Process -FilePath $l.Stub)
Wait-UpdateApp
Stop-App
Done
