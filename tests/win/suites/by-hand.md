# The VM cases no suite drives (by hand)

The cases `tests/win/vm-run.sh` can't drive, for the VM lane by hand (W13a), each on x64 and arm64
unless said. They are never a pass of a suite: no step stands in for them (a step that can't run
its own case exits 77, SKIP, which `--record` writes as `.failed`), and `vm-run.sh check` reads only
`release.list`'s records. The results go in the round's notes.

Each case names its owner: T17 (a suite may drive it later), W13 (the VM lane's W13a round) or
the owner. A case with no label belongs to the W13a round.

## Moved to suites (no click but the app's own dialogs, no UAC prompt, nothing physical)

- `restore.suite` ("rekordbox 7.2.19 logged in", after Stems Cache through the shipped worker):
  the three `-Restore` cases (the bridge in place, the live DLL deleted, a junctioned version
  folder refused), plus refused unelevated and the run in ConstrainedLanguage. Elevated steps are
  `user-high` (no prompt). Each SKIPs while the served bootstrap.ps1 has no `RBSTEMSPLUS_RESTORE`.
- `uninstall-cases.suite` ("clean Windows", three install-and-uninstall rounds): the uninstall
  from the app's menu and from Settings (`--uninstall`) after a quiet start; a junction inside the
  apps folder (deleted as itself, its target kept); a planted `%TEMP%\ping.cmd` (not run);
  EnableExtensions=0 in HKCU with a `%TEMP%` holding `!`, `%` and `^`; cmd.exe blocked (IFEO, as
  SYSTEM): the RunOnce value written and read back; a `UsageLogs` entry that is a link (left); the
  stub and DLL copied elsewhere with `--after-exit` (a folder of another name, outside `%TEMP%`,
  a junction: refused, nothing deleted); a junction at `Microsoft\CLR_v4.0` (left).
- `volumes.suite` ("clean Windows", `need vm`): T3c case 3 on a VHDX as NTFS then exFAT, through
  location-win-test's `--make`/`--check`/`--gone` (diskpart as SYSTEM, the checks unelevated).

None is a release.list row. What each leaves by hand stays below, marked.

## Inputs the suites can't make themselves (the step SKIPs without them)

`tests/win/vm-run.sh stage --build` builds and stages the rest from the Mac (both architectures'
test exes, fixtures.txt, the TEST build's fault and bad-version releases). These stay:

- Owner: the network case of location-win-test when `net use` can't map `\\Mac\win` from the
  scheduled task's session: map it there by hand (or say how the VM should), then run again.
- Owner: whether the x64 runs under Prism on the ARM64 VM fill an x64 row of release.list, or x64
  is the real PC's round only (the x64 exes are staged and run either way).
- T17: version-refused needs a TEST build whose bootstrap.ps1 has Test-RbspAppVersion
  (w6/win-bootstrap-app), and the bootstrap suite a TEST build with a `windows` block
  (w6/release-windows's build.sh).
- T17: update.suite's three TEST releases (dist-test/a, carried, new), each its own
  `scripts/build.sh --test http://10.211.55.2:8766/current/` build.

## Bootstrap (release.md section 8)

- T17: the SAC On run (cl, elevated and install's SAC branch) from a snapshot with Smart App
  Control On, through `user-signed` (the user steps start through the unsigned hidden.exe, which
  SAC may block), with a `user-signed-high` mode for the elevated case, and `-Restore` with SAC On
  (restore.suite runs the rest of `-Restore`).
- T17: I82's start-up test from a release-shaped zip.
- Moved: the three `-Restore` cases (restore.suite).
- T17: the planted-DLL and worker ACL tests.
- T17: Windows on another drive than C:.
- T17: B5 (information): the size-mismatch zips through Expand-Archive, the bytes written recorded.
- W13: the NW6 memo's cases.

## Update (release.md section 7 and 8, app.md 5.4 and 5.5)

- T17: the shown version kept (sticky) while a newer app is fetched, and while one is uninstalled.
- T17: pin, update, launch and grouping from the pin; both stubs from a pin.
- T17: a release that changes the stub, from a fixture whose stub hash is known (update.ps1 checks
  only that the installed stub is the new release's).
- T17: Stems Cache's UAC cases (worker.suite elevates without a prompt) and J23; the audit's old
  `bin\` worker; rekordbox's cache hit with the shipped bridge (a track loaded twice, the second a
  hit in bridge.log: worker.suite's cache-hit SKIPs until a step can load one; whether this
  round, recorded, may stand in for worker.suite's record is the owner's open question); a
  worker-changing update; the worker chosen across accounts.
- W13: `--watcher` and its R1 cases; a broken `current`; the rollback of a failed swap (a broken
  XAML, a missing DLL) with its once-only message; remove_tree on a running exe; an elevated worker
  seen by its path.

## Uninstall (T11, app.md 5.9)

- Moved: the uninstall from the app's menu (uninstall-cases.suite).

- The complete uninstall with Stems Cache installed (UAC first, Pioneer's onnxruntime.dll back);
  cmd.exe blocked: the temp folder gone after the next logon (the RunOnce value itself is
  uninstall-cases.suite's); an app that never quits (nothing deleted); a second copy held open
  during the uninstall (waited 60 s, then the files and the Apps & features entry kept, the uninstall
  runs again from Settings > Apps once it is closed); the after-exit lines in DebugView;
  TMP=`\\host\share\t` (the folder gone after the logon); the copy's exe held open about 10 s and a
  `PING -n 10` from the copy's folder (a race with the after-exit's start).
- A second account B signed in (switch user) with its watcher and app running, then A uninstalls:
  the uninstall completes (AX_DONE, no 60 s wait; B's processes, refused with ERROR_ACCESS_DENIED,
  never count, review GH1); A's next start's update undo probe and sweep aren't held either.
- Moved (uninstall-cases.suite): a junction inside the apps folder; a UsageLogs entry that is a
  link; a junction at `Microsoft\CLR_v4.0`; a planted `%TEMP%\ping.cmd`; EnableExtensions=0 and a
  `%TEMP%` holding `!`, `%` or `^`; the stub and DLL copied elsewhere with `--after-exit`.
- Reboot-branch uninstall, a reinstall of the same release, a restart: the bin\ worker present
  (J7); the uninstall audit after the restart: the root gone (R4.I77).
- WC7-WC10 (app.md 5.9): a logoff vetoed while the worker runs (the app quits once the step ends);
  a veto then Cancel (the app stays, no later quit); a veto then "log off anyway" (WM_ENDSESSION's
  bounded wait); the message order; Close waiting on the worker; UPDATE_SWAP and UPDATE_RESULT; one
  copy (HANDED, BUSY, an abandoned mutex, a pin click during --swap).
- PF4: a Prepare run's helper command line (Get-CimInstance Win32_Process) carries
  `<PF>\RB Stems Plus\ort\onnxruntime.dll` and the current version folder; with the version folder
  renamed while the app is open, the run isn't started and the log says why.
- x64 under Prism on an ARM64 VM: the x64 stub and test exes as well as the native ones.

## Platform

- Moved (volumes.suite): T3c case 3, a VHDX as NTFS then exFAT with any=0: "ok" through its drive
  path and its GUID path (also upper case), with no drive letter, mounted at `C:\mnt\x-<fs>\`
  ("folder" there, "ok" through its GUID path), `--gone` on its mounted GUID root ("folder"), on
  NTFS a junction on it (`<Letter>:\jx` to `C:\rbtest\vol\jt`) on the way ("folder"; exFAT holds
  no junction, an INFO line), read-only ("read-only"), detached ("not-connected" from
  location_check and location_gone).
- T3c case 3's rest: a disk set offline and an empty card reader ("not-connected"); the mount
  folder left with nothing mounted (DRIVE_NO_ROOT_DIR): volumes.suite says what location_check
  makes of it, and plat_win.h:64's note is corrected from that by hand.
- MQ4 items 3-4 (models2-rr1): MR3 through the core DLL's --test-hooks build on NTFS (support a
  dangling junction: native_support_write ELOOP, nothing at its target; a file at support:
  ENOTDIR); REMOVE_LOCATION on NTFS and exFAT, Clear (kept, 0) and uninstall (the folder and its
  `._` companion gone) with a 258-byte CJK name inside; whether ESTALE fires for a folder deleted and
  recreated by name (information).
- app.md section 7: winc-app-wintest's external-drive case (`--external X:\`) on a real USB stick
  attached to the VM by hand, unelevated and elevated: the stick's letter in `C:\rbtest\usb.letter`,
  then the platform suite's wintests steps pass it (a VHDX isn't external to `vol_external()`).
- app.md 5.4: WinHTTP against GitHub (Range kept over the redirect, 206 resume, the automatic proxy,
  a redirect to http refused); remove_tree with a folder swapped for a junction mid-walk.
