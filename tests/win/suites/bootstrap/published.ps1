# Same HTTPS asset path as the published one-liner, on the run's TEST server.
function Get-PublishedBootstrapBase {
    return ($VmRun.Url -replace '^http:', 'https:') + 'releases/latest/download/'
}
