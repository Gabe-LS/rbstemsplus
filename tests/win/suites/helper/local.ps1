# (included by helper.suite) Get-LocalExe NAME: helper/NAME from the share, copied into a fresh folder of the user's
# temporary folder (the system drive), and that folder
function Get-LocalExe([string]$name) {
    $src = Need "helper/$name" 'build/helper-win via helper/build-win.sh'
    $dir = Join-Path ([IO.Path]::GetTempPath()) ("rbsp-$($VmRun.Step)-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $dir | Out-Null
    Copy-Item -LiteralPath $src -Destination $dir
    return @{ Exe = (Join-Path $dir $name); Dir = $dir }
}
