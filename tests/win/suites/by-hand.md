# The VM cases no suite drives (by hand)

The cases `tests/win/vm-run.sh` can't drive, for the VM lane by hand (W13a), each on x64 and arm64
unless said. They are never a pass of a suite: no step stands in for them (a step that can't run
its own case exits 77, SKIP, which `--record` writes as `.failed`). The results go in the round's notes.

Each case names its owner: T17 (a suite may drive it later), W13 (the VM lane's W13a round) or
the owner. A case with no label belongs to the W13a round.

## VR2 release record

After the applicable by-hand cases in the round pass on the RC's TEST build, the owner or
automation runs the following from the checkout. Use the full RC commit ID.

```sh
python3 -I tests/win/gate-record.py write-by-hand COMMIT dist-test --pass
```

The writer creates `build/gates/COMMIT/win-by-hand.ok` with the `commit`, `payload_sha256`,
`round VR2`, UTC `date` and `result pass` lines. The gate checks the record and both bindings.
Keep the case results in the round's notes.

## Moved to suites

- `shell-ui.suite` ("clean Windows"), W2 plan rows 29–32 and 35–36. K1/K3 opens Settings >
  Apps > Installed apps and checks the single kept dialog's text, foreground and z-order. K4
  restarts with the dialog open and checks the K4, UN2 and UN6 temp folders at the same logon.
  K1 retries through Settings and completes. UN4 checks a held image and the watcher before
  uninstall, then checks the files and Installed apps entry are retained. UP2 pins through
  Explorer, closes and relaunches the app, and checks grouping. WC6/WC7 repeats the update from
  the pin at 0, 500, 1000 and 2000 ms after close, using the native and x64 stubs on ARM64.

- `restore.suite` ("rekordbox 7.2.19 installed", after Stems Cache through the shipped worker):
  the three `-Restore` cases (the bridge in place, the live DLL deleted, a junctioned version
  folder refused), plus refused unelevated and the run in ConstrainedLanguage. Elevated steps are
  `user-high` (no prompt). Each SKIPs while the served bootstrap.ps1 has no `RBSTEMSPLUS_RESTORE`.
- `uninstall-cases.suite` ("clean Windows", three install-and-uninstall rounds): the uninstall
  from the app's menu and from Settings (`--uninstall`) after a quiet start; a junction inside the
  apps folder (deleted as itself, its target kept); a planted `%TEMP%\ping.cmd` (not run);
  EnableExtensions=0 in HKCU with a `%TEMP%` holding `!`, `%` and `^`; cmd.exe blocked (IFEO, as
  SYSTEM): the RunOnce value written and read back; a `UsageLogs` entry that is a link (left); the
  stub and DLL copied elsewhere with `--after-exit` (a folder of another name, outside `%TEMP%`,
  a junction: refused, nothing deleted); a junction at `Microsoft\CLR_v4.0` (left); round 4,
  K2: a watcher of the same account in another session (session 0, through a scheduled task with
  a password logon, the account given a random one), then `--uninstall`: AX_LEFT, the files, the Apps & features entry and the shortcut
  kept, the after-exit's log (the debug output, read by the step) naming the apps folder it runs
  from, the RunOnce value written before the kept dialog, the dialog shown; that watcher ended,
  the uninstall run again completes.
- `sessions.suite` ("clean Windows"), UN8: a second local account B made in the run (its password
  generated in the VM, never printed or on a command line) installs with the one-liner and runs
  its app and watcher in its own logon session (a scheduled task, LogonType Password, session 0);
  A's start's sweep isn't held by B's processes (refused, GH1) and A's uninstall completes:
  AX_DONE, no 60 s wait, everything of A's gone, B's copy untouched.
- `volumes.suite` ("clean Windows", `need vm`): T3c case 3 on a VHDX as NTFS then exFAT, through
  location-win-test's `--make`/`--check`/`--gone` (diskpart as SYSTEM, the checks unelevated).
- `stems-cache-uac.suite` ("rekordbox 7.2.19 installed + stems model", UAC prompting as Windows
  ships it; the by-hand card's G3, the automation plan's W1): UP4 refused (one prompt, nothing
  changed) and accepted (the prompt names `RB Stems Plus.exe` and an unknown publisher); W5 A7.1, the chain
  held during runas of the `bin\` worker; WC1+WC9 (a sign-out vetoed while the worker runs,
  nothing pressed, it goes on by itself); WC5 (Close refused while the worker runs; no later quit); WC2 (the veto, then
  Cancel: the app stays, no later quit); WC3/WC4 ("Sign out anyway": rekordbox's DLL whole);
  WC10 (Uninstall All, the veto, "Sign out anyway": rekordbox's model intact or recovered, no
  half-written one). UAC's prompt and the block screen are answered by rbui (FlaUI) on the secure
  desktop, run there as SYSTEM by sysdesk (`tests/win/uia`); the sign-out comes from the consent
  responder as soon as the worker runs, and autologon signs back in (vmlib's Set-AutoLogon: a new random password on the local account,
  set in the reverted VM only; a Microsoft account SKIPs). A case
  whose worker ended before the sign-out reached the app says MISSING (a SKIP), never a pass.
- `uninstall-uac.suite` (the same snapshot; the card's G4): UN1, the complete uninstall with
  Stems Cache (UAC first, Pioneer's onnxruntime.dll back, nothing left, the trace audit); UN9's
  reboot branch with the `bin\` worker held (its image renamed to `*.del` and pending), a
  reinstall of the same release, Stems Cache again, a restart: the `bin\` worker there (J7); the
  last uninstall elevates the `bin\` worker (the prompt's details), a restart, the root gone
  (R4.I77), the trace audit as before the first install.

- `volumes.suite`, its rest (the automation plan's W3): the VHDX's disk set offline while mounted
  in its folder ("not-connected" by its GUID path and from location_gone), the mount folder left
  with nothing mounted (DRIVE_NO_ROOT_DIR: "not-connected", checked), and the VM's CD-ROM drive
  with no disc ("not-connected"), the stand-in for an empty card reader.
- `platform.suite` (a release.list row), W3: the location step's network case maps `\\Mac\win`
  from its own session, else the VM's own SMB share `\\127.0.0.1\rbnet` (and checks that one
  maps); net-remote: a mapped share removed under its mapping stays DRIVE_REMOTE and "not-local"
  (plat_win.h's note says so now); winhttp: update_win.c's WinHTTP download against the run's own
  HTTPS server (vm-run.sh's `/w3/`): a 302 to https followed, a resume from the middle straight
  and over the 302 (206, Range kept), a 302 to http refused (its own 302, nothing written), the
  user's PAC proxy used (a CONNECT in the server's log).
- `platform-owner.suite` (optional owner setup; each step SKIPs without its input), PL1/PL3/PL4:
  winc-app-wintest's external-drive case (`--external`) on a real USB stick, unelevated and
  elevated (the stick left in a Mac port, its id from `prlsrvctl usb list` in
  `.orchestration/vm-run.conf` as `usb VM DEVICE-ID`: "need usb" assigns it before the VM starts and
  takes it back at the end); the real empty card reader (`usb-reader VM DEVICE-ID`), whose empty
  removable drive letter is checked by location-win-test; WinHTTP against GitHub itself (the latest release's payload.json over
  GitHub's 302, whole then resumed: 206), once `.orchestration/vm-run.conf` says `github allowed`
  (after Little Snitch's rules for Parallels to github.com, release-assets.githubusercontent.com
  and objects.githubusercontent.com).
- `sac.suite` (from "clean Windows", run with `--keep`, then `vm-run.sh snapshot "SAC On"`):
  Smart App Control switched On (VerifiedAndReputablePolicyState 1, CiTool --refresh, a restart),
  read back (the registry, Defender's SmartAppControlState) and acting (our unsigned hidden.exe
  blocked). `bootstrap-sac.suite` (from "SAC On", every user step through user-signed): the
  one-liner in ConstrainedLanguage and the plain install both give the SAC line and install
  nothing. Both suites' steps are ConstrainedLanguage-safe (sac/lib.ps1, not vmlib).

- `start-lock.suite` ("clean Windows", N12): the stub started with the full token (user-high,
  as Run as administrator) refused at start-up (T_CANT_OPEN, T_START_ROOT, exit 1, nothing
  written), then a normal start; the complete uninstall from the menu followed at once by the
  one-liner while the after-exit holds the install lock (its one notice, then the install).
- `report.suite` ("clean Windows", N4): Create Report end to end (the ZIP on the Desktop with the
  event log's and WER's parts, the issue link caught from msedge.exe's command line, the account,
  its full name, its profile folder and its 8.3 alias masked in both), its Logs\reports fallback,
  and the Settings dialog's fields.
- `watcher.suite` ("rekordbox 7.2.19 installed + stems model", N4 and N10): the watcher stopped
  by Uninstall Demucs v4 and back with its reinstall, kept off when Startup apps says off; R1's
  offer opened by the watcher, Don't Ask Again for This Version under the answer lock held by the
  step (the write waits for it), and the watcher's next round not opening the app.
- `admin-paths.suite` ("rekordbox 7.2.19 installed + stems model", N12 and AR4): UAC No on
  Uninstall Stems Cache (the choice kept); Cancel at "rekordbox updating" (a PING.EXE copy named as
  rekordbox's updater, rekordbox's folder moved aside: the two-prompt case) keeping the choice; the
  app folder's admin\ worker removed: Install stops before elevation, Restore and Uninstall
  elevate the bin\ worker.
- VM fixture consent and logoff rows moved to `stems-cache-uac.suite`, `uninstall-uac.suite`
  and `admin-paths.suite` as detailed above. Keep the first live results with the reverted
  fixture round.
- RB-KIT M12 rounds moved to `tests/rekordbox/win/pc-session.sh auto prepare` and
  `auto record` with the M12 fixtures. The plans cover install, update, pin, worker source,
  Settings kept, confirmed sign-out and the retry. The record plan restores the candidate;
  the SmartScreen check needs the owner-supplied downloaded installer fixture and owner
  handling of any OS prompt.

None is a release.list row but platform.suite. What each leaves by hand stays below, marked. The two UAC suites
await their first VM run (the secure-desktop spike: rbui reading the consent prompt and the block
screen as SYSTEM); a part that fails there comes back here.

## Inputs the suites can't make themselves (the step SKIPs without them)

`tests/win/vm-run.sh stage --build` builds and stages the rest from the Mac (both architectures'
test exes, fixtures.txt, the TEST build's fault and bad-version releases). These stay:

- Moved (platform.suite): the network case of location-win-test when `net use` can't map
  `\\Mac\win` from the scheduled task's session (the VM's own `\\127.0.0.1\rbnet` instead).
- Moved (platform.suite, startup.suite): the release matrix records ARM64 and forced x64
  (Prism on ARM64) separately. The owner's native x64 PC checklist in app.md section 7 still
  needs its own round.
- Moved (IN3/IN5): use the commands in `build-update-fixtures.sh` to build the planned TEST
  candidate and capture its inputs. `build-update-fixtures.sh OLD_VERSION` copies that candidate
  and makes the old app, carried, bad-version and fault releases for bootstrap.suite and
  update.suite. Put the generated `build/update-fixtures/dist-test` tree in the run share after
  staging. Add `--local` for standalone experiments.
  First live bootstrap version refusal and old/carried/new update runs are pending.

## Bootstrap (release.md section 8)

- Moved (bootstrap.suite, `published`): the PowerShell 5.1 one-liner through the published
  HTTPS asset path on the Mac's TEST server. The post-publish check against the actual release
  in release.md section 8 still needs its live round.
- Moved (sac.suite, bootstrap-sac.suite): the SAC On snapshot and the SAC On run of cl and the
  install's SAC branch through `user-signed`.
- Moved (bootstrap-sac.suite, `install-high`, `restore-fixture` and `restore`): BS1 checks the
  elevated SAC refusal and restores Pioneer's DLL with SAC On through `user-signed-high`.
  The Restore steps need the SAC On snapshot with rekordbox 7.2.19 installed and closed.
- Moved (startup.suite): I82's start-up test from the installed TEST zip, with the native
  ARM64 shell and the staged forced-x64 shell under Prism.
- Moved (bootstrap.suite, `zip-start`): I82 starts the extracted zip natively and, on ARM64,
  under Prism x64, and checks the core DLL binding.
- Moved: the three `-Restore` cases (restore.suite).
- Moved (worker-guards.suite, `plant-dlls` / `first-worker` / `check-dlls`): BS3's shipped
  worker is traced during its first elevation with the four DLL fixtures beside it.
- Moved (worker-guards.suite, `refuse-writable`, `refuse-null-dacl`, `refuse-empty-dacl`,
  `refuse-writer`, `refuse-delete-handle` and their trace checks): each ACL or held handle
  gives exit 4 with no worker launch. `retry` / `check-retry` check a bin-worker uninstall
  after fixture release. A7.1's chain across consent is already in stems-cache-uac.suite
  (`wc1`).
- T17: Windows on another drive than C:.
- Dropped: B5, the size-mismatch zips through Expand-Archive. bootstrap.ps1 unpacks with its own
  capped Expand-RbspZip (BS2); unit-51 runs the size-mismatch zips through it on 5.1.
- Moved (platform.suite, `memo-stamp`): NW6 checks WOF hashing and ChangeTime invalidation
  after restoring LastWriteTime. It records hit or miss when LastWriteTime updates are suspended.
  First live NTFS runs pending.

## Update (release.md section 7 and 8, app.md 5.4 and 5.5)

- Moved (update.suite, `sticky` and `update`): UP1 checks About and Installed apps keep the
  carried version after fetching a newer app and through the old app's uninstall confirmation.
  A fresh carried install then updates and checks the replacement's shown version.
- Moved (update.suite, `stub-change`): UP3 checks the native stub's old and new fixture hashes,
  the retained .old backup and its removal at the next good start. The two fixture hashes must differ.
- Moved: Stems Cache's UAC cases (stems-cache-uac.suite: refused, accepted; worker.suite still
  elevates without a prompt) and the audit's old `bin\` worker (uninstall-uac.suite).
- Moved (worker-update.suite, `old-app-new-payload` / `check-old-choice`): J23's old
  app fetches a newer payload with a different worker hash, then reinstalls Stems Cache
  through the old app's own worker. The bridge asset is carried to isolate worker selection.
- Moved (worker-update.suite, `refuse-replaced-worker` / `check-replacement`, then
  `install-successor` / `check-successor`): UP5's replacement is rejected by the old bin
  worker. The retry verifies both worker paths and verbs, and the new worker in bin.
- UP7 runs in worker-update-uninstall.suite (`uninstall-first` / `check-uninstall-first`).
  The two-elevation result is checked. The same-protocol one-elevation outcome is SKIP
  pending a ruling on release.md §7 versus admin.c's existing matching-worker choice.
- Moved (worker-choice.suite, `account-b-worker-choice`, `b-install-worker`, `worker-choice`
  / `check-worker-choice`): UP8/J23's older app uses the newer matching shared bin worker,
  with B's independent new app and watcher present. SYSTEM applies B's installed release
  through the shipped workers because the B task has no interactive desktop.
- T17: rekordbox's cache hit with the shipped bridge (a track loaded twice, the second a
  hit in bridge.log: worker.suite's cache-hit SKIPs until a step can load one; whether this
  round, recorded, may stand in for worker.suite's record is the owner's open question).
- Moved (update-rollback.suite, `broken-current`): UP9 checks the stub refuses missing, malformed
  and dangling `current` records and starts normally after the record is restored.
- Moved (update-rollback.suite, `rollback-xaml` and `rollback-missing-dll`): UP10 exercises failed
  swaps with an off-screen XAML load error and a missing native core DLL.
- Moved (update-rollback.suite, `rollback-offscreen-hang`): UP11 blocks the candidate's WPF thread
  off screen and checks the watchdog restarts the old app.
- Moved (update-rollback.suite, `rollback-once-only`): UP12 checks a saved failure appears on the
  first start and is gone on the next start.
- Moved (watcher.suite, `r1-missing-live` / `r1-offer-live`, `r1-missing-ort` / `r1-offer-ort`, and
  their restore steps): each missing DLL patch opens the app with its own R1 offer. The existing
  `r1-foreign-2` / `r1-quiet` pair checks the dismissed missing-live offer.
- Moved (watcher.suite, `sharing`): the update and removal directory operations while the watcher
  remains resident. Running Pioneer's actual updater and uninstaller still requires operator-owned
  fixtures and handling of any prompts.
- W13: an elevated worker seen by its path.

## Status

| Case | Check |
| --- | --- |
| Status order and counts | Display name vs kept-set ID for ordering. Manifest-only vs library for counts. Owner/coordinator ruling pending. |

## Uninstall (T11, app.md 5.9)

- Moved: the uninstall from the app's menu (uninstall-cases.suite).

- Moved: the complete uninstall with Stems Cache installed (UAC first, Pioneer's onnxruntime.dll
  back: uninstall-uac.suite).
- Moved (uninstall-cases.suite, `never-quits`): UN3 keeps the install when the app outlasts
  the after-exit parent wait.
- Moved (uninstall-cases.suite, `never-quits` and `held-copy`): UN5 captures the after-exit
  debug trace with DBWIN.
- Moved (uninstall-cases.suite, `held-copy`): UN7 holds the after-exit exe about 10 s with
  `PING -n 10` running from its folder and checks that cleanup still finishes.
- Moved (sessions.suite): a second account B with its watcher and app running, then A uninstalls:
  AX_DONE, no 60 s wait, B's processes (refused with ERROR_ACCESS_DENIED) never count (GH1); A's
  start's sweep isn't held.
- Moved (sessions.suite and sessions-interactive.suite, `a-undo`): A's pending update is undone
  while B's app and watcher run. The failure alert is shown once. The desktop variant
  (`account-b-desktop`, `b-after-desktop`) needs a snapshot with B signed in through Switch user
  and A back at the console. Preparing that snapshot still requires the operator.
- Moved (uninstall-cases.suite, round 4): K2, a watcher in another session of the same account.
- Moved (uninstall-cases.suite): a junction inside the apps folder; a UsageLogs entry that is a
  link; a junction at `Microsoft\CLR_v4.0`; a planted `%TEMP%\ping.cmd`; EnableExtensions=0 and a
  `%TEMP%` holding `!`, `%` or `^`; the stub and DLL copied elsewhere with `--after-exit`.
- Moved (uninstall-uac.suite): the reboot-branch uninstall, a reinstall of the same release, a
  restart: the bin\ worker present (J7); the uninstall audit after the restart: the root gone
  (R4.I77).
- Moved (stems-cache-uac.suite): WC1-WC4, WC5, WC9, WC10 (app.md 5.9): a logoff vetoed while the
  worker runs, then Cancel, then "log off anyway"; Close refused while the worker runs; Uninstall All
  during the worker, then "log off anyway".
- Moved (stems-cache-uac.suite, `wc1-trace`, `wc2-trace`, `wc3-trace`, `wc10-trace`): the
  TEST shell and existing block-screen responder record the veto, visible block screen and end
  messages. Cancel and Sign out anyway each have a separate ordering assertion.
- Moved (shell-copy.suite, `copy-close`): WC8 keeps the real model copy staged until Close is
  refused, then checks that the copy finishes, with no later quit.
- Moved (shell-ui.suite, `one-copy`): standalone HANDED, BUSY (10 s), and acquisition of an
  abandoned mutex. The timed pin cases already run in `update-native-*` and `update-x64-*`.
- Moved (sessions.suite and sessions-interactive.suite, `a-undo`): an undone update's
  UPDATE_RESULT alert appears once. The host update tests also cover retaining the result
  across a start that exits before the alert.
- Moved (prepare-app.suite, `helper-path` and `helper-path-refused`): PF4 checks the Prepare
  helper's ORT and verified rekordbox paths, then checks the process trace and app log after the
  rekordbox version folder is moved while the app is open.
- Moved (startup.suite, platform.suite): the x64 stub and platform test exes under Prism, with
  separate native ARM64 records in release.list.

## Prepare

- Moved (prepare-w13b.suite, `w13b-spike` and `w13b-pt`): the 5,000-row spike, picker
  rollover, PT8 input counts, folder selection parity against Mac A and ST3 high-contrast
  pixels. First live runs are pending on native x64, ARM64 and x64 under Prism. The
  app.md section 4.2 N1 rows still require their real-facts setup.

## Platform

- Moved (volumes.suite): T3c case 3, a VHDX as NTFS then exFAT with any=0: "ok" through its drive
  path and its GUID path (also upper case), with no drive letter, mounted at `C:\mnt\x-<fs>\`
  ("folder" there, "ok" through its GUID path), `--gone` on its mounted GUID root ("folder"), on
  NTFS a junction on it (`<Letter>:\jx` to `C:\rbtest\vol\jt`) on the way ("folder"; exFAT holds
  no junction, an INFO line), read-only ("read-only"), detached ("not-connected" from
  location_check and location_gone).
- Moved (volumes.suite): T3c case 3's disk set offline and the mount folder left with nothing
  mounted ("not-connected"), and an empty CD-ROM drive standing in for an empty card reader.
- Moved (platform-owner.suite), owner hardware: T3c case 3's real empty card reader
  (DRIVE_REMOVABLE, no medium). Assign the reader with `usb-reader VM DEVICE-ID` in
  `.orchestration/vm-run.conf`. The reader needs one visible empty removable drive letter.
- Owner PC hardware facts: `pc-session.sh disk probe --drive LETTER` records the selected USB
  drive's type, bus, readiness and file system for the platform round. Cases requiring the
  product to run against that hardware still need live verification.
- Moved (platform.suite, `support-errors`): on NTFS the native support writer returns ELOOP for
  a dangling junction without creating its target and ENOTDIR for an ordinary file.
- Moved (volumes.suite, `ntfs-remove-location` and `exfat-remove-location`): Clear keeps the
  258-byte CJK name, and uninstall removes the folder and its `._` companion.
- Moved (platform.suite, `plat-win-test`): reports whether deleting and recreating a directory at
  the same name changes its ID, and checks ESTALE when it does. The NTFS run also covers EBUSY
  with a live executable and a junction substituted after directory enumeration.
- Moved (platform-owner.suite), unproven until its first run with the stick: app.md section 7's
  external-drive case on a real USB stick, unelevated and elevated. Owner, once: a stick left in a Mac port and its id in
  `.orchestration/vm-run.conf` (until then the steps SKIP).
- Moved (platform.suite's winhttp against the run's server; platform-owner.suite's winhttp-github
  against GitHub): app.md 5.4's WinHTTP cases (Range kept over the redirect, 206 resume, the
  automatic proxy, a redirect to http refused). Owner, once: Little Snitch's allow rules for
  Parallels to github.com, release-assets.githubusercontent.com (where GitHub's 302 points
  today) and objects.githubusercontent.com, then `github allowed` in
  `.orchestration/vm-run.conf` (until then winhttp-github SKIPs).

## Narrator and notification cleanup (NAR / W6a Q13)

Moved to `shell-ui.suite` (`a11y`). The step records the Manage tree and an app dialog's
Name and HelpText, the focus and live-region events during Create Report, and the banner and
tray lifetime of a uniquely titled NIF_INFO. It diffs notification settings and exercises
`life_records_remove` only on the exact new app AUMID, with a control record present. First
live run must confirm UIA exposes the light and dialog peers, the report status event, the
banner and icon on that Windows build, and that Windows creates the expected record.
Speech, keyboard traversal and the wording reported by Narrator still need a short owner pass.

## Create Report (M4 VM) and N4's other steps

NS2's display-name row (`net user <u> /fullname:"Dana Jones"`, the name masked in the ZIP and the
issue link) moved to `report.suite`, with the rest of Create Report end to end.

| Case | Check |
| --- | --- |
| N4, the date text (CORE_STEP_DATE_TEXT) | Moved to `prepare-app.suite` / `helper-path`. With a real Prepare helper on a closed rekordbox fixture, compare its selected day with the new app.log entry using the user's long date format. Live: confirm the date formatter and log flush on x64 and ARM64, ideally with two user locales. |
| N4, the notification (CORE_STEP_NOTIFY) | Moved to `prepare-app.suite` / `notifications`. Complete two separate playlists with the app minimised while the helper is live. Each expected banner must appear once and the app's native tray icon must disappear. Live: confirm title exposure and cleanup on the snapshot's Explorer build with notifications enabled. |

## Prepare and Status (W13b, N1)

The VM fixtures use real facts, the installed TEST payload and helper, and a closed rekordbox.
Run the ready case on native x64 and ARM64 with the x64 helper under emulation. The PC plan
`tests/rekordbox/win/pc-session.sh auto prepare` lists owner-only checks as pending steps.

| N1 row | Moved to | First live verification |
| --- | --- | --- |
| 1. Ready installation | `prepare-app` / `helper-path` | With listed rekordbox, Stems Cache and sound quality, the Prepare summary has no problem lines and the playlist can start. |
| 2. Speed and actual settings location | `prepare-facts` / `negative-fact` (`facts-speed`), RB-KIT `W13b settings-location` | Locate the real settings file by changing the option in rekordbox. Confirm the resulting speed line. An alternate path needs a separate `hf_env` decision. |
| 3. Missing helper | `prepare-app` / `facts-missing` | The exact warning appears, Prepare is disabled, and the process trace delivers its control event without an app helper start. |
| 4. Keep arguments and Status | `prepare-app` / `helper-path`, `kept-lifecycle` | Confirm the command line, the two real files, and the Status date and count. Code orders by kept-set ID and can replace manifest count with the library track count. The fixture uses a playlist with all tracks kept. The plan's displayed-name ordering needs a ruling if that is the intended sort. |
| 5. Stop Keeping | `prepare-app` / `kept-lifecycle`, RB-KIT `W13b bridge-release` | Confirm both files and the row disappear. With running rekordbox, confirm the bridge releases the set. |
| 6. Expired and malformed | `prepare-app` / `kept-lifecycle` | A planted expired set stays out of the table and a malformed set appears with a blank date cell displayed as a dash. |
| 7. Unlisted version | `prepare-facts` / `negative-fact` (`facts-unlisted`) | With a signed test payload omitting the installed version and the Stems Cache ORT copy present, confirm one unsupported-version line and no reinstall line. |
| 8. Entry-only bridge | `prepare-facts` / `negative-fact` (`facts-entry`) | Confirm the entry-only fixture lacks kept support and produces the reinstall line with a listed version. |
| 9. Background notification and capacity | `prepare-app` / `notifications`, `capacity` | Verify both notification lifetimes. Verify a limit raise is offered only when the required increase fits on the cache drive after the 1 GiB reserve. The dialog exposes the offered limit, so the step checks both decisions against independent available bytes. |
| 10. Pause, Resume, Stop, Exit, Update | `prepare-app` / `pause-stop`, `paused-exit`; `prepare-update` / `paused-update` | Confirm the fixture runs are long enough, the same helper really suspends and resumes, Stop writes both files, and Exit and Update drain the paused helper. In the update lap, confirm the candidate owns the new window. |
| 11. Watcher prepared playlists | RB-KIT `W13b watcher-reminder`, `watcher-dedupe`, `watcher-active-set`, `watcher-set-filter` | Verify the notification and click-through, session restart and reopen deduplication, running and paused skips, and the missing, empty, expired and unreadable-date cases with fresh rekordbox sessions. |
