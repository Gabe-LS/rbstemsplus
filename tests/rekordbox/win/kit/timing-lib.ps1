# Opt-in elapsed times. Keep diagnostics out of the PowerShell success stream.
function Start-KitTiming {
    if ($env:RBKIT_TIMING -eq '1') { [Diagnostics.Stopwatch]::StartNew() }
}

function Write-KitTiming([string]$phase, [Diagnostics.Stopwatch]$clock, [string]$row, [int]$attempt, [string]$detail) {
    if (-not $clock) { return }
    $scope = if ($row) { " row=$row" } else { '' }
    if ($attempt) { $scope += " attempt=$attempt" }
    if ($detail) { $scope += " $detail" }
    Write-Host "TIMING windows $phase elapsed_ms=$($clock.ElapsedMilliseconds)$scope"
}
