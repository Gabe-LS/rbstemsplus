# As SYSTEM: a batch that must ask no UAC (its name: this step's before "-uac"): the elevations
# the Security log saw between its start and end, 0
Test-MvUac ($VmRun.Step -replace '-uac$', '') 0
Done
