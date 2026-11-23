$UpdateEntry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\RBStemsPlus'

function Expect([bool]$ok, [string]$what) {
    Check $ok $what
    if (-not $ok) { Done }
}

function Assert-UpdateDesktop {
    # Same stop rule as shell-ui: leave Windows prompts for the operator.
    $session = (Get-Process -Id $PID).SessionId
    $prompt = Get-Process -Name 'consent', 'smartscreen' -ErrorAction SilentlyContinue | Where-Object {
        $_.SessionId -eq $session -and ($_.Name -eq 'consent' -or $_.MainWindowHandle -ne [IntPtr]::Zero)
    }
    Expect (-not $prompt) 'the user session has no Windows security prompt'
}

function Use-UpdateUi([Parameter(ValueFromRemainingArguments = $true)][string[]]$argv) {
    Assert-UpdateDesktop
    $limit = [Array]::IndexOf($argv, '--timeout')
    $remaining = Get-WaitSecs ([int]$argv[$limit + 1])
    Expect ($remaining -gt 0) 'time remains for the UI action'
    $argv[$limit + 1] = [string]$remaining
    $rc = Invoke-Rbui @argv
    Assert-UpdateDesktop
    Expect ($rc -eq 0) "rbui $($argv -join ' ') (exit $rc)"
}

function Wait-UpdateApp {
    $opened = Wait-Until { $script:UpdateApp = Get-AppWindow; $null -ne $script:UpdateApp } 90
    Assert-UpdateDesktop
    Expect $opened 'the current app opened in the user session'
}

function Get-UpdateRelease([string]$name) {
    Get-Content -Raw -LiteralPath (Need "dist-test/$name/payload.json" "update fixture $name") | ConvertFrom-Json
}

function Get-NativeUpdateArch {
    $machine = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
    if ($machine -eq 'ARM64') { return 'arm64' }
    return 'x64'
}

function Test-UpdateAbout($app, [string]$version) {
    $title = Get-MenuText 'T_MENU_ABOUT'
    Use-UpdateUi menu --pid $app.Id (Get-MenuText 'T_MENU_HELP') $title --timeout 30
    Check (Find-UiText $app.Id ('^' + [regex]::Escape($version) + '$') 15) "About shows $version"
    Use-UpdateUi invoke --pid $app.Id --title $title (Get-CoreText 'T_OK') --type Button --timeout 15
}
