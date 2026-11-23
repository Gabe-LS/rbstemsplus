# rekordbox test toolkit

Repeatable, unattended tests against a real rekordbox: load a corpus playlist track by track,
read back what rekordbox loaded, watch the bridge's log for the stems, and stop at anything
unexpected. One design, two drivers:

- **Windows** (`win/`, now): the owner's test PC, a standard test account, driven over SSH from the Mac.
- **Mac** (`mac/`, built; its first rekordbox session still to come): a tart VM, driven over SSH
  from the host. The same commands and the same rules, rebuilt for macOS (see the end).

Automation answers the app confirmations named by each test. The owner handles sign-in,
licence, activation, security and UAC prompts (see "What still needs the owner").

## The shared design

**Control: MIDI.** rekordbox is driven through a virtual MIDI port and a mapping learned in
rekordbox (MIDI LEARN), never by clicking its window. A press is a note on at velocity 127, then a
note on at velocity 0 as the release, as Pioneer hardware sends it. rekordbox reads a note off as
a held button and repeats it. The mapping (channel 1):

| Note | Hex  | rekordbox function          | Note | Hex  | rekordbox function           |
|------|------|-----------------------------|------|------|------------------------------|
| 64   | 9040 | Browse Down                 | 70   | 9046 | Play/Pause deck 1            |
| 65   | 9041 | Load deck 1                 | 71   | 9047 | Sort by track number         |
| 66   | 9042 | Browse Up                   | 72   | 9048 | Switch active window         |
| 67   | 9043 | Forward                     | 73   | 9049 | Sort by title (SortSONG)     |
| 68   | 9044 | Back                        | 74   | 904A | Area Left (learned by hand)  |
| 69   | 9045 | Area                        | 75   | 904B | Area Right (learned by hand) |

rekordbox listens only after it has sent its first MIDI (its lights). The driver waits for that,
then 15 s more while rekordbox builds its browser. On Windows, a rekordbox with both decks empty
may send no light at all (2026-11-19): after 30 s the driver presses Browse Down then Up (no net
move) every 10 s, and rekordbox's answer counts as its first MIDI.

**Play/Pause (70) is not learned on the PC** (its `Loopback B.midi.csv` has no PlayPause row,
2026-11-19): a Play press does nothing there, so `quit`'s pause and `dmlstall`'s playback check
have no effect until the owner learns it (MIDI LEARN, deck 1's Play/Pause, note 70).

**The walk to the playlist** (no mouse): Area Left ×5 reaches the icon column whatever has the
focus. Up ×10 and Down ×1 select Playlists. Area Right enters the tree. Up ×10 and Down ×2 select
the test playlist. Area Right enters its track list. The list's header is then read by OCR and
must be exactly the playlist's name and track count ("RBSP Tests (N Tracks)", N the corpus's rows
plus w16's long-path copy when `corpus\long-path.txt` exists); if it isn't, the run stops before
any Load.

**Read-back: every load is checked.** After each Load, a screenshot is taken and deck 1's title is
read by the OS's own OCR at four scales. Each reading is matched to a corpus row: either a unique
exact match (0 edits while every other row is at least 1 edit away — needed for 1-edit `fmt-*`
pairs like `1ch`/`2ch`), or the closest name within 3 edits and at least 2 edits closer than any
other row (`win/kit/match-lib.ps1`). A row needs at least 2 of the 4 readings, and more than any
other row. The runner counts
`w16-flac-longpath` and its long-path copy as separate rows and accepts them in either order.
The copy is named `w16-flac-longpath-copy` in the runner and shows its 8.3 short name on deck 1
(`W16-FL~1`, with a suffix that can vary).

- **Sort direction:** the runner loads the top row and reads it back. If OCR can't match a row,
  the runner reloads once in the same direction. A match to corpus row 0 confirms ascending
  order. A match to another row permits one SortSONG flip, followed by the same check. The run
  stops if it still can't confirm row 0.
- **Lost presses:** if deck 1 shows an earlier row than expected, a Down was lost. The runner
  steps again from the row it sees (up to 3 tries).
- **Rows rekordbox won't take** are stepped over: it refuses (deny icon) 64-bit float WAV and
  FLAC at 11.025, 352.8 and 384 kHz. It doesn't import raw AAC (ADTS) at all: that row isn't in
  the list.

**Completion:** a load counts as done when the bridge's log (`bridge.log`) has grown and then
stayed quiet for 10 s. Nothing new in 25 s is an `EXPECT FAIL`; still writing after 7 min is a
STOP (its lines would be counted against the next row). Each row's misses, hits, refused hits
(`hit  KEY refused:`, a damaged entry served as a miss) and near serves are counted from the new
lines.

**Expectations:** `any` (the first run: misses, stems computed) or `hits` (a later run, or after
Prepare). With `hits`, each load needs at least one `hit` and zero misses or refused hits. That
includes sort and recovery probes and repeat loads of the same track. Each load is evaluated
using its own new log lines. A probe that logs only a near serve or a diagnostic fails this
expectation. The runner records `EXPECT FAIL`, continues the run, and exits 3 if it finishes.

**The dialog watch** (`win/kit/watch-lib.ps1` and `mac/kit/watch-lib.sh`). At the start of a run,
both drivers list the watched windows by process, title and window ID. Both allow the main
windows of rekordbox and RB Stems Plus. The Mac also allows its calibrated rekordboxAgent news
window. Other watched windows stop the run before any MIDI, subject to the Windows update
exception below. During a run the watch lists
windows again immediately before each Load, after the navigation and settling time. This covers
sort probes, ordinary rows and retries. The Mac's configured walk is for navigation only.
The watch also checks after the last row and every 20 s during Windows Prepare.
A new window, even one with a known title, is a **STOP** unless the update exception applies.
On a STOP a screenshot is taken and the window is left open. The Mac sends one ntfy alert to the
owner's phone with the plan or command and the reason, up to 160 characters. Log and screenshot
paths stay in the local notification log line. `quit` checks the windows against the run's baseline (or
the start rule) before it presses anything, and stops untouched on a new one. The watch has its
own test. `run watchtest` opens a window titled "RBSP watch test", checks the watch stops on it,
sends the notification marked as a test, and closes it; it fails if the notification isn't
sent. `run T2 --inject-at N` opens the same window before row N of a real run (a row the run
loads, else it is refused), which must stop there with the STOP marked "(watch test)".

**Windows update exception (owner decision, 2026-11-19).** At startup, during the watch and at
quit (`run close`), the session task may invoke **Close** through UI Automation on the window
titled `rekordbox update manager`, owned by
`%ProgramFiles%\rekordbox\rekordbox <version>\Upmgr rekordbox.exe`. It requires the English text
“update packages” (read by OCR: the updater is a JUCE window, which does not expose its text) and
both in-dialog buttons, **Start** and **Close**. In UI Automation that window is named and
identified `Upmgr rekordbox` (framework JUCE), and the buttons are `Start.<empty>.Upmgr rekordbox`
and `Close.<empty>.Upmgr rekordbox`. The runner invokes only
**Close** and logs the displayed versions and the paths to screenshots taken before and after
the attempt. A failed check or a window still open after Close is a **STOP**.
`rekordbox | INFO` is rekordbox's own news window, not part of the offer. By the same owner
decision, and at the same points, the session task may close it (and only it): `rekordbox.exe`
under `%ProgramFiles%\rekordbox\rekordbox <version>\`, the title `INFO`, and in UI Automation a
JUCE window named and identified `INFO` with its `INFORMATION` and `NOTIFICATION` tabs
(`<tab>.<empty>.<empty>.INFO`). It is closed through its UI Automation window pattern, as its
close box does (no key, no click), with screenshots before and after; a failed check, another
`INFO` window or one still open after Close is a **STOP**.
`rekordbox | Initializing STEMS Engine` is, per the owner, rekordbox's normal progress bar while
it loads its STEMS engine; it usually takes 5-10 s (it was seen after the probe replaced
`onnxruntime.dll`). At the same points it is **waited out, never touched**: `rekordbox.exe` under
the rekordbox folder, that exact title, and in UI Automation no buttons of its own are checked
(read only, logged), then the window list is polled every 2 s for up to 60 s. One still there
after 60 s, a failed check, or a look-alike is a **STOP**.
rekordboxAgent's news window (`rekordboxAgent | rekordboxAgent`) may be closed at the same points,
quit included (owner decision, 2026-11-19), as start-up's `closeagent` already did: the agent
under `%ProgramFiles%\rekordbox\rekordbox <version>\rekordboxAgent-win32-x64\`, that title, no
owner window, and in UI Automation its Electron window (named `rekordboxAgent`, class
`Chrome_WidgetWin_1`; its page is not exposed) with no buttons. It is closed through its UI
Automation window pattern, with screenshots before and after; any other agent window, a second
one, a failed check or one still open is a **STOP**. The updater log includes the
native owner handle. This exception is Windows-only for now. The Mac records identify its
updater and confirm an update dialog, but do not document the title or dismissal control.

**rekordbox in front (Windows, 2026-11-19).** Before a run reads rekordbox's screen (the playlist
header, each deck 1 read-back), the session task minimizes our own app's window through its UI
Automation window pattern (never closes it), focuses rekordbox's verified main window
(`rekordbox.exe` under the rekordbox folder, title `rekordbox`) through UI Automation and confirms
it is the foreground window. If it can't, the run STOPs. Other apps' windows are left as they are.
Before the kit drives our app's own UI it restores that window from minimized (UI Automation again).

**Our app closed and reopened (Windows, owner decision 2026-11-19).** Before a rekordbox model change
(`run t2-model-on|off`) and before the Prepare round, the kit closes our app in the test account's
session by its own Exit (the menu RB Stems Plus > Exit through UI Automation; when that menu isn't in
UI Automation, its window's close, which the app treats as Exit), waits until its process is gone
(a STOP if it isn't within 30 s; nothing is ever killed) and starts it again afterwards. A fresh
start also clears the black window seen after many session reconnects.

**Auto Analysis off (Windows, owner decision 2026-11-19, test account only).** `run autoanalysis-off`
(session2's first step) does nothing when `rekordbox3.settings` already says `AutoAnalysis` 0. Else
it opens rekordbox's Preferences by its gear (the one unnamed button between the plan button and
`AudioCpuGraphButton`), shows the Analysis category, unticks the check box of the Auto Analysis
section only, with screenshots before and after, closes Preferences, quits rekordbox and reads the
setting back (a STOP unless it reads 0).

**Preconditions first (Windows, 2026-11-19).** Each auto plan runs `run preconditions --plan PLAN`
right after `preflight`: the session task, Pioneer's DLL, no research probe, Auto Analysis, the
stems model (Demucs v4 or rekordbox's own), our app (running, minimized, its tabs in UI Automation
or a black window) and whether rekordbox runs. What the plan puts right itself is a FIX line; every
failed one is listed in one BAD line and the plan stops there, before anything changes.

**After a STOP** (or an exit other than 0 or 3) nothing more is done: rekordbox is left open as it
is, and the owner is notified at once. An error in the runner is a STOP too.

**Stops of its own:** a `midi-stop` file in the results folder, rekordbox gone, deck 1 not the
expected track, the run's `--minutes` reached, or no MIDI from rekordbox in 120 s after its start.
After the start, rekordboxAgent's news window is closed (as its X does) only when it is the only
one; otherwise it stays, and the run's start rule stops on it.

**State:** `rbstate.ps1` snapshots the test account's rekordbox folders and registry key, puts
them back, takes inventories and diffs them. This brings rekordbox back to a known state between
sessions without touching machine-wide state (activation stays as it is). The roots:
`%APPDATA%\Pioneer`, `Music\PioneerDJ`, `Documents\rekordbox`, `%LOCALAPPDATA%\Pioneer` and
`%APPDATA%\PioneerLog`. PioneerLog was added after PC session 1, where a launch wrote logs there.

**The kit** is staged with a `SHA256SUMS` made on the Mac. It is checked on the Mac before the
copy and on the PC after it (`verify-kit.ps1`), and again at every preflight.

**Nothing personal is tracked.** Logins, addresses and disk names live in the Mac's local git
config (below). The corpus is generated tones (`helper/tests/perf.sh corpus`, `win/gen-corpus-win.sh`).
Logs, screenshots and collections go to `build/rekordbox/` (git-ignored); another results folder
inside the checkout is refused unless git ignores it. `win/pc-session-test.sh`
fails if an IPv4 address, the git identity or a private file's name appears under `tests/rekordbox/`.

**Exit codes** (both drivers): 0 done, 1 a STOP or a step not OK, 2 refused (bad arguments,
settings or state), 3 an expectation not met.

## The Windows driver

| File | Where it runs | What it does |
|---|---|---|
| `win/pc-session.sh` | Mac | `preflight`, `stage`, `run`, `collect`, `cleanup` over SSH (PowerShell sent as `-EncodedCommand`, every call time-bounded) |
| `win/pc-session-test.sh` | Mac | its tests without the PC (in `make check`) |
| `win/gen-corpus-win.sh` | Mac | the corpus's `win/` rows (ffmpeg) |
| `win/kit/midi-auto.ps1` | PC | the runner: `ensure`, `start`, `quit`, `run`, `watchtest`, `prepare`, `uninstallcache`, `bundle`, … (its header lists them all) |
| `win/kit/session-task.ps1` | PC, in the signed-in session | what SSH can't do: start and close rekordbox, screenshots, the window list, UI Automation of our app, the test window, sign-out |
| `win/kit/midi-lib.ps1` | PC | WinMM ports, the Loopback A/B pair (recreated when missing), presses |
| `win/kit/ocr-lib.ps1`, `match-lib.ps1` | PC | Windows OCR of screen regions; the title matching and the bridge's line counts |
| `win/kit/watch-lib.ps1` | PC | the dialog watch |
| `win/kit/kit-lib.ps1` | PC | shared paths, rekordbox's folder and Pioneer's `onnxruntime.dll` hash, the session-task call |
| `win/kit/rbstate.ps1` | PC | snapshot, verify, restore, inventory, diff |
| `win/kit/pre-lib.ps1` | PC (tested on the Mac) | an auto plan's preconditions judged: OK, FIX (the plan puts it right), NOTE, BAD |
| `win/kit/bench-lib.ps1`, `dml-run.ps1` | PC | the DirectML bench: the timing lines and deck 1's readouts parsed; one helper run in the desktop session |
| `win/kit/net-probe.ps1`, `fw.ps1`, `verify-kit.ps1` | PC | the TEST server reached over HTTP and HTTPS; rekordbox's connections (read-only `-Mode Check`); the kit's checksums |

Why a scheduled task: SSH runs in its own logon session, which can't show or close windows, take
screenshots or use UI Automation. The task "RBSP session" (the test account, interactive, run
through `conhost --headless`) runs one command at a time inside the signed-in session.
`midi-auto.ps1 -Do ensure` registers it. The account registers it for itself, so no elevation is
needed.

The OCR regions in `ocr-lib.ps1` are for the test PC's screen: 3840×2096 at 200 %, with rekordbox
maximised. On another screen, take a `shot`, measure deck 1's title and the list header, and
change `$RbspDeck1` and `$RbspHeader`.

### Settings (the Mac's local git config, never committed)

```
git config --local rbstemsplus.pcSsh USER@HOST
```

```
git config --local rbstemsplus.pcTestSsh TESTUSER@HOST
```

```
git config --local rbstemsplus.pcTestServer MAC-ADDRESS:PORT
```

```
git config --local rbstemsplus.pcT5Serial "T5-SERIAL"
git config --local --add rbstemsplus.pcProtectedDisks VEVTVFNFUklBTDAxfFJPRyBFU0QtUzFD
```

- `pcSsh` is the owner's admin login (also used by `tests/harness/netcheck.sh`). The kit uses it
  for `cleanup`'s admin steps and the disk tools, on the test account's PC (its
  `COMPUTERNAME`, read over the test login).
- `pcTestSsh` defaults to `rbtest@` the host of `pcSsh`.
- `pcTestServer` is the Mac's TEST server as the PC reaches it. The test CA is name-constrained
  to that address, so a new address needs a new CA.
- `pcT5Serial` is the dedicated Samsung Portable SSD T5's serial as reported by Windows
  `Get-Disk`, with leading and trailing whitespace removed. The T5 commands also require its
  model and USB bus to match. The real-hardware external-drive rows (J28 and the sleeping USB
  disk) wait until that drive is plugged in.
- `pcProtectedDisks` takes one value per disk via `git config --local --add`. Each value is
  Base64 of UTF-8 `SERIAL|FriendlyName` with the serial trimmed and no trailing newline.
  To compute your own value, substitute the disk's serial and friendly name in
  `printf '%s' 'SERIAL|FriendlyName' | base64`. `disk probe --drive LETTER` also prints the value.
  Replace existing UniqueId-based entries with fresh probe values. The configuration example above
  uses the fake serial `TESTSERIAL01`. Its value is `VEVTVFNFUklBTDAxfFJPRyBFU0QtUzFD` for
  `TESTSERIAL01|ROG ESD-S1C`. The kit also refuses the ROG ESD-S1C by model and any disk
  with an Acronis volume. The T5 folder commands require a valid protection list and limit
  changes on the T5 to `rbsp-test` and its contents.

`RBKIT_NOTIFY` overrides the notifier, `RBKIT_OUT` the results folder and `RBKIT_POLL` the seconds
between `cleanup`'s sign-out checks (10).

### Setting up once

1. **Owner:** create the standard test account and sign it in to rekordbox once (licence and
   sign-in are the owner's). Turn off Cloud Library Sync, cloud drives and Collection Auto Upload
   in its rekordbox. Add the Mac's SSH key to the account.
2. **Owner, admin:** install Windows MIDI Services Tools (Microsoft's GitHub release, MIT). Check
   its hash against the release's digest first.
3. Stage the kit and the corpus (below). `ensure` creates Loopback A/B and the task.
4. **Owner, in rekordbox (MIDI LEARN):** map Loopback B as in the table. Learn notes 64 and 65,
   then Area Left (74) and Area Right (75); `midi-auto.ps1 -Do learn -Note 74 -Repeat 30`
   presses a note every 2 s while rekordbox learns. The other rows can be added to
   `rekordbox6\MidiMappings\Loopback B.midi.csv` with rekordbox closed. Keep a copy.
5. Import the corpus into a playlist "RBSP Tests". The long-path copy of w16 is made by
   `verify-kit.ps1` (its path is in `corpus\long-path.txt`).
6. **Owner:** trust the test CA in the account's CurrentUser\Root store (one click).

### A session

Run each command on the Mac, unsandboxed. For long runs, start them in the background: every SSH
call has a time limit, and a STOP or a lost connection sends a notification. A step that hangs
without output is cut only at that limit: a no-output watchdog is for later (RK22).

```
tests/rekordbox/win/pc-session.sh stage --dist build/win/dist-test --corpus build/rekordbox/corpus
```

```
tests/rekordbox/win/pc-session.sh preflight
```

```
tests/rekordbox/win/pc-session.sh run watchtest
```

```
tests/rekordbox/win/pc-session.sh run T2
```

```
tests/rekordbox/win/pc-session.sh run prepare --playlist "RBSP Tests" --minutes 60
```

```
tests/rekordbox/win/pc-session.sh run hits
```

```
tests/rekordbox/win/pc-session.sh collect
```

```
tests/rekordbox/win/pc-session.sh cleanup
```

- `stage` copies only what it is given: the kit's scripts always, the corpus when named, and from
  `--dist` only `payload.json` and the test CA (`test-ca.pem`, converted to `test-ca.cer`). It runs
  `verify-kit` on the PC and runs `ensure` once the kit verifies. When a T5 serial and a valid
  protection list are configured, stage also verifies the T5's model, serial and USB bus
  on the PC using a read-only check.
- `preflight` checks: SSH to the test account, the kit's checksums, `ensure` (task, session,
  loopback, mapping, OCR), rekordbox's non-loopback connections, the test CA, the TEST server and
  the notifier.
- `run T2|hits` starts rekordbox, walks to the playlist, loads `--from`..`--to` (`--max` loads at
  most) and quits it. `--keep-open` leaves rekordbox running. `--minutes` stops new rows; the row
  in progress finishes (up to 7 min). A `midi-stop` is honoured during a row's wait too.
- `run prepare` drives our app's Prepare tab by UI Automation and waits until it is idle. On its
  time limit or `midi-stop` it presses the tab's Stop and checks it is idle within 60 s.
- `collect` zips the runner's log, the screenshots, the app's logs and the mapping on the PC,
  then unpacks them into `build/rekordbox/win/collect-<time>/`. The screenshots are deleted on
  the PC only after the unzip here.
- `cleanup` closes rekordbox, uninstalls Stems Cache (checking Pioneer's `onnxruntime.dll` is back),
  takes a snapshot and an "after" inventory, removes the account's admin rights (owner login,
  by the account's SID; without `pcSsh` the account only checks it is no longer an
  administrator) and signs the account out
  (`--no-logoff` keeps it signed in). Signed out means its session is gone (`quser`, up to 2 min).
  If rekordbox can't be closed (a STOP), cleanup stops there. If Stems Cache can't be uninstalled,
  our DLL stays under rekordbox, which the owner's own rekordbox on the PC loads too: cleanup
  notifies and stops before the snapshot, the admin rights and the sign-out, so that the
  account keeps the admin rights and the session needed to finish the uninstall. That is the
  default; the owner can choose otherwise (remove the rights anyway and leave the DLL).

To stop a run from the PC side, create `%TEMP%\rbst\midi-stop` in the test account.

### Unattended: one command, nobody at the PC

```
tests/rekordbox/win/pc-session.sh auto session2
```

`auto PLAN` starts with `session cleanup-runners`, then runs `session up`, `stage` (when
`--dist`, `--corpus` or `--probe` is given),
`preflight`, the plan's rounds, `collect` and `cleanup --lock`. The first step that exits 1 or 2
ends the plan: nothing more is done on the PC, and the owner is notified. Exit 3 (an expectation
not met) is logged and the plan goes on. The log is `build/rekordbox/win/<time>-auto-PLAN.log`.

Admin kit scripts use the same timeout on the PC and the Mac. The remote timeout ends the runner
and its Windows job's child processes. `session cleanup-runners` uses the admin login on the
verified PC to stop and report PowerShell runners older than 20 minutes (above the longest runner deadline) whose
encoded command decodes to the kit's runner command or to one of its earlier stdin bootstraps. The cutoff is above every runner's
own deadline (the admin worker's is 15 minutes), so an active runner is never stopped.

Set `RBKIT_TRACE=1` for UTC phase timestamps on stderr from admin kit scripts. `session up`
enables these timestamps by default and writes stderr to `build/rekordbox/win/<time>-session-up.log`.
Use `RBKIT_TRACE=0` to disable the timestamps. The console script flushes each pre-call
timestamp to stderr. The last timestamp shows which call the script reached. The intermittent
hangs on 2026-11-19 occurred before the script started, while the old bootstrap waited for
stdin that Windows OpenSSH sometimes failed to deliver.

Set `RBKIT_TIMING=1` for monotonic elapsed milliseconds. Each controller command writes a
`.log.timing` sidecar with its duration, exit code, console operations, and one record per
SSH or SCP call. Transport times include the remote work. The Windows runner writes
`TIMING windows` lines to the command log. Format batches time MIDI navigation (with
`nav-mode=relative` for a single Down from the previous row or `nav-mode=reset`), OCR,
each load and its activity wait, the settle window after a silent load, and research helper
keys by corpus ID. Load times include deck OCR and both waits when applicable. Controller
commands include their child operations.

| Plan | Rounds |
|---|---|
| `session2` | Auto Analysis off, T2 facts, watch test, Demucs v4 on, Stems Cache through its worker, `run T2`, `run hits`, console DirectML and worker uninstall. With `--probe`, Demucs v4 round B and three fresh AAC starts for round C. Demucs v4 off at the end. T11 remains a SKIP. |
| `dumps` | T2's mix dumps, collected by the probe on 01-05, 09, 17, 19 and w10. `--no-dumps` fetches `probe.log` and `rows.txt` only. `--helper` collects only row evidence for a fresh run and restores the runtime on exit. |
| `prepare` | Demucs v4 on (the Prepare tab needs it), Stems Cache, `run prepare` (our app freshly started), `run hits`, uninstall, Demucs v4 off |
| `record` | on the RC (`--commit SHA --dist DIR`): Stems Cache, `run T2`/`hits` (`--max`, 2), `run directml`, uninstall; then `build/gates/<commit>/pc-win.ok` |

**Format corpus staging and key comparison** (`corpus/formats`)

The owner's Windows run on 2026-11-21 measured all 16 priority rows on commit
`17d98f16`. Two fresh acceptance runs on `67f9abd9` reproduced all 16 baseline results.

1. Generate the files from the `corpus/formats` branch (`tests/corpus/gen-formats.sh` or
   `tests/corpus/pc-rows.tsv`). Stage the media, approved probe and research helper together.
   Choose `--format-tier published|discovery|all` or `--format-rows ROW|FROM:TO[,...]`.

   ```sh
   tests/rekordbox/win/pc-session.sh stage \
     --formats DIR --format-tier published \
     --probe build/win/probe-t1/onnxruntime.dll \
     --helper build/helper-win-timing/rbsp-prepare.exe
   ```

   The kit checks the destination volume and rejects reparse points. It verifies the upload
   in a sibling directory, checks any matching filename against the fixture already staged,
   and stops on a hash mismatch. It adds only missing files to `corpus\formats` and publishes
   the selected subset's SHA-256 manifest after all its files are in place. Earlier media
   stay at the paths rekordbox imported. An interrupted transfer preserves the previous
   selection. `stage --clear-formats` clears the selection and retains the media. An older
   stage without the kit ownership marker requires review before restaging.

   `pc-session.sh run library-check [--rekordbox VERSION]` uses the installed helper's
   read-only `--playlists` and `--tracks` in the test desktop session. The summary
   includes only names beginning with `RBSP `, their library IDs and one-based sibling
   positions, and track IDs matched to kit corpus paths. The kit copies the filtered
   JSON to `build/rekordbox/win`.

2. Run `pc-session.sh run format-import --rekordbox 7.2.20` to verify the playlist
   ahead of measurement. By default the kit reuses a root **RBSP Formats …** playlist
   marked for the staged manifest with exactly those files. When there is no match,
   or when you pass `--fresh-format-import`, the kit writes an M3U8 named
   **RBSP Formats <subset-id>.m3u8**, using the subset manifest and a per-import suffix.
   It records the expected name, opens rekordbox's **File > Import > Import
   Playlist**, verifies the native picker's owner chain, and reads back the kit path
   before submitting. Once progress clears and the kit closes rekordbox, the same
   library query used by `run library-check` must find exactly one playlist with that
   name. The kit reports `IMPORT_ADDED` or `IMPORT_REFUSED` for every staged corpus ID
   and writes an ordered snapshot to `%TEMP%\rbst\formats-library.m3u8`.

   After a submitted import stops, `run format-import --resume-import` closes
   rekordbox and checks the recorded name through the helper without submitting
   another file. Keep the same staged subset during verification.

3. Run the numbered batch. The batch also reuses exact imports by default and
   accepts `--fresh-format-import` to request a new one. Use `--reuse-format-import`
   to require the recorded playlist. If the recorded import has refused IDs, add
   `--confirm-import-refusals` to measure the rows that were imported.

   ```sh
   tests/rekordbox/win/pc-session.sh auto dumps --formats-only --format-batch 1 \
     --probe build/win/probe-t1/onnxruntime.dll
   ```

   Before installing the probe, the plan records the verified playlist ID, tree
   position, ordered corpus IDs, and subset manifest. Measurement checks this
   record and walks directly to the library's root position over MIDI under the
   window watch. Keep playlist folders collapsed so their children do not shift
   the visible root positions. The kit checks the **RBSP Formats** heading at that
   position before loading. Each Load must match the expected corpus title in deck
   1 OCR before the kit attributes measurement or cache evidence.

The owner may prune old **RBSP Formats …** playlists in rekordbox after their
batches are complete.

On launch, the kit checks InputDirector in the rbtest session and asks only the
rbtest-owned process to close normally. Format UI focus uses `AttachThreadInput`
and verifies the foreground window. The window checks tolerate recognized JUCE
tooltips and hidden helpers. Use `run close` to cancel a verified picker if one
is open, post Escape to the verified main window, and request a normal close.
Import STOPs and close timeouts capture redacted UI dumps for diagnosis.

Before starting rekordbox, the kit checks an RB Stems Plus watcher offer for the
install, **Remind Me Later**, and dismiss buttons. Their TaskDialog action IDs
are 100, 101, and 102. The native button IDs can all read as 0. In that case the
kit requires the buttons to appear in that order. It presses **Remind Me Later**
only on the verified “Stems Cache isn't on in rekordbox … yet.” offer. Any other
watcher text, including the Reinstall offer, refuses the start.

**UI diagnostics.** When a new rekordbox version changes the UI, run
`pc-session.sh run format-uidump --rekordbox VERSION` and `run format-step` first.
Start that version in the test session with `run start --rekordbox VERSION` if
needed. The plain dump reads the main window's UI Automation tree without focus or
input changes. It prints up to 3,000 redacted JSON lines through depth 12 and saves
them in `%TEMP%\rbst\format-uidump-<id>.jsonl`. Dumps and control STOPs redact
non-allowlisted names and identifiers that echo them.

Use `pc-session.sh run winlist --rekordbox VERSION` to inspect the test session's
rekordbox processes and all their top-level windows through Win32. It reports
screen rectangles, owners, classes, styles, visibility, enabled and cloaked state,
and uses the format diagnostics' caption allowlist. Standard Win32 dialogs also list
their controls' classes and IDs, with Static text and Button captions where readable.
DirectUI dialogs can also report their text, buttons, and links through UI Automation.
Winlist uses heuristic redaction for paths and track titles in dialog text. The command
leaves focus and input alone. STOP lines show only exact known messages verbatim and
report the character count and a SHA-256 prefix for other dialog text.
On-screen crops go to `build/rekordbox/win/shots/` by default. Each record says
whether capture succeeded, was unavailable, or was blocked by an overlapping app.

A visible rekordbox `#32770` message box causes a STOP during the window watch.
Use `run winlist --rekordbox VERSION` to inspect it and
`run dismiss-rb-error --rekordbox VERSION` for a confirmed “Unexpected application
error” notice. The desktop task checks the message and its single enabled OK button
again, then verifies that the dialog closed.

**Known limitations**

- Outside format runs, STOP lines show watched windows' captions verbatim.
  Winlist redaction is heuristic and can miss unquoted titles without an extension.
- `dismiss-rb-error` checks the OK button's style during dialog verification.
  A style change in the milliseconds before `BM_CLICK` can go undetected.
- The follow-up poll for another watcher offer stops after one second.
  A later offer stays open and untouched.

Run one step at a time with `--rekordbox VERSION`. Each step captures the relevant
windows and leaves rekordbox open. Use `focus`, `dump`, `click file`, `click import`,
`click import-playlist`, `cancel-dialog`, or `escape` to inspect the import sequence.
`click playlists` inspects the source switch. The arrow-key steps require a verified
menu, and `key alt+f` explicitly opens File. `run format-uidump --explore` walks the
import menus and cancels the picker, then captures the kit test playlist's context
and export menus. Compare these captures with the STOP before updating UI checks
or retrying an import. After menu steps, use `run format-step escape` for an open
menu or `run format-step cancel-dialog` for an open picker before `run close --rekordbox VERSION`.

Each batch covers up to four fixtures in corpus ID order and reports its batch count.
The 172-row published tier has 43 batches. Use `--format-batch N` for one batch or
`--format-batch FIRST:LAST` for an inclusive range in one rekordbox session. Between
batches the controller checks and forwards the kit's `FORMAT CONTINUATION` token. The kit
verifies rekordbox's process ID and start time and checks the activity log stamps before
the next load. Keep the same staged subset when resuming or retrying a batch. Each attempt
has its own comparison TSV and per-row content hashes in `chunk-hashes.tsv` under
`build/rekordbox/win/formats-batch-*`. The raw probe log stays on the PC.
The batch's `helper-stderr.log` records the helper's allowlisted AAC Media Foundation
type-change diagnostics.
On startup the kit polls for the browser heading and watches for startup windows until
15 seconds after the first MIDI message. Each batch verifies the format playlist heading.
Within a batch, the runner reaches the next row with a single Down after a confirmed load.
The batch's first row, each retry and each row after an unconfirmed load start from a full
reset: the walk to the playlist, the heading check and the count down from the top of the list.
Every load still needs the exact deck title match before its evidence counts. A load with
two unsuccessful deck readbacks is recorded as `LOAD_UNCONFIRMED`.
The comparison returns 3 for any refusal, missing evidence or mismatch. If rekordbox's
hashes match the helper's for another row in the batch, the verdict is `WRONG_TRACK`.

The format plan uses the approved installed-runtime probe in keys-only mode and restores
Pioneer's runtime before comparing. Its TSV measures deck loads and key agreement.
The `helper_mode` column records `proved`, `test-unproved-aac`, `test-unproved-mp3`,
or `test-unproved-aac+test-unproved-mp3` per row. With `auto dumps --formats-only`,
use `--test-unproved-aac`, `--test-unproved-mp3`, or both to opt the staged research
helper into those codec comparisons. With Demucs v4 and Stems Cache
installed, prepare the imported playlist with
`run prepare --formats --playlist "RBSP Formats <subset-id>"`, then use
`run hits --formats --format-batch N --rekordbox VERSION` for cache evidence. It reports
confirmed exact serves, misses, refusals, near serves, and seconds from Load to first and
last activity. It also reports hit latency when the bridge logs it.
The batch counts a cache row as met when it records at least one exact serve, at most
one miss, and zero refusals. When the helper identifies the expected chunks, the kit starts
a 2-second completion margin after all owned probe keys and their Run lines, including
`never_cached` chunks, or after all expected exact cache hits and one unplanned trailing
miss. Rows without the completion signal use the quiet wait.
The cache pass closes rekordbox after exit 0 or 3 when there is no STOP.
The keys-only plan attempts the same restore on interruption or failure.
If the kit reports that restoration is unconfirmed, close rekordbox and run `probe remove`
before the next batch. `run rowkeys --formats` runs just the helper on the active subset.
With rekordbox closed, add `--from ROW --test-unproved-aac` for one AAC fixture or
`--from ROW --test-unproved-mp3` for one MP3 fixture. The diagnostic prints the
helper's refusal reason if it skips the input.

**The DirectML bench** (`run dml-bench [--from A --to B] [--helper EXE]`, coordinator decision
2026-11-19), with Stems Cache and Demucs v4 installed: a `--test-timing` build of the helper
(`helper/build-win.sh --test-timing build/helper-win-timing`, staged with `stage --helper` or
`--helper`) runs rows A..B (default 19-mp3-cbr128..20-wav-16-30s) with rekordbox closed, first on
the CPU, then with `--dml`, each into a scratch cache. It logs DirectML's own lines (its version,
or why the CPU ran it), the execution provider and the mean per-chunk ONNX Runtime, model and
whole-chunk times of both, and their ratio. The `--dml` entries are copied into the account's
Stems Cache over the same keys, `run hits` on A..B must then show rekordbox serving them, and
(best effort) deck 1 plays while `--dml` prepares 09-flac-16: its elapsed time is read by OCR
every 2 s, and steps under half real time are counted as stalls. Exit 3 when DirectML fell back to
the CPU or the hits were not all there.

**M12 fixture run** on any of `session2`, `prepare` and `record`

```sh
tests/rekordbox/win/pc-session.sh auto session2 \
  --from-dist build/win/dist-test-old --dist build/win/dist-test-next \
  --uia build/uia/rbui --bootstrap-server MAC-ADDRESS:8765
```

Use two Windows TEST releases in sibling folders under the running Mac TEST server's
share. Set `pcTestServer` to its HTTPS address and build both apps for that endpoint.
The server reads `serve-current.txt` for each request and serves the selected folder at
`/<file>` over HTTP and `/latest/download/<file>` and `/download/vVERSION/<file>` over HTTPS.
Use the session card's TEST server with a share dedicated to this run. Build rbui with
`tests/win/uia/build.sh build/uia`. Start with a clean per-user RB Stems Plus install
state and the test account set up for unattended relogon.

- The driver selects N, verifies the served manifest, and installs through the TEST
  bootstrap on the interactive desktop. It selects N+1 and starts the app's own check.
  For a carried component, DisplayVersion and the installed manifest must advance,
  with the same version directories and no update offer. For a newer component, it
  confirms the app's own Update dialog and verifies the swap, watcher, and native stub.
- rbui invokes Explorer's **Pin to taskbar** verb. The persisted pin must target the
  stable stub, open a fresh app process, and expose one taskbar group. The kit removes
  its pin afterward. It stages the installed TEST manifest’s bridge before the existing admin worker runs.
- After the worker removes Stems Cache, rbui drives Settings > Apps > Installed apps.
  An image held under the app folder forces the kept dialog. The task records the
  dialog's position over Settings and the exact RunOnce command. The driver signs out,
  reopens the test session and checks the temp copy and its RunOnce entry are gone
  before retrying from Settings. `record` reinstalls N+1 before the RC hash check.
- Add `--installer NAME.exe` (or `.msi`) when an approved observation fixture exists.
  Put the same file in the candidate TEST folder and browser-download it into the
  test account's Downloads first. The kit checks the file hash and download zone,
  launches through the shell, and records the SmartScreen title and screenshot if
  observed. That observation stops and notifies, leaving the window on the desktop.
  With no new window and a successful shell launch it records a 25-second negative
  observation. The screenshot stays under `%TEMP%\rbst\shots` if the plan STOPs.
  The normal release uses the PowerShell one-liner. An executable installer fixture
  needs an owner decision.

The release-record command also accepts its existing worker-only inputs
(`--commit SHA --dist DIR`). For an M12 record, `--dist` is the RC TEST folder.

**Side-by-side Windows installs**

The kit recognizes the original Pioneer runtimes for rekordbox 7.2.19 and 7.2.20.
It selects the highest installed version by default. Pass `--rekordbox 7.2.19`
or `--rekordbox 7.2.20` to target a specific install for T2, related checks, probe
install or state, and close. A missing version is refused. If a different version
is already running in the test session, close it first with `run close` and its
version. The probe checks for running rekordbox processes across sessions before
changing a runtime.

```sh
tests/rekordbox/win/pc-session.sh preflight --rekordbox 7.2.19
tests/rekordbox/win/pc-session.sh run T2 --rekordbox 7.2.19
tests/rekordbox/win/pc-session.sh run close --rekordbox 7.2.19
tests/rekordbox/win/pc-session.sh auto dumps --rekordbox 7.2.19 --probe PATH/onnxruntime.dll
```

`auto dumps` pins the version for the plan and attempts probe restore on failure as
well as success. `probe remove` without a version follows the saved backup.
Stems Cache compatibility and resampler proof are governed by the TEST payload.

**T2 round 2** in `session2`

- Item 2 inventories the selected rekordbox 7.2.19 or 7.2.20 DLL names, records whether
  the executable matches the known build hash, and reports file versions for
  the two decoder candidates when present. Those files are copied into
  `t2/rb-orig/` in the collection. Decoder export inspection is a separate
  authorized analysis task.
- Pass `--probe PATH/onnxruntime.dll` for items 3, 4 and 6. After the Stems Cache
  worker returns Pioneer's runtime, the kit installs Demucs v4 through its Manage
  confirmation, uses the admin probe in keys-only mode, and runs B over the corpus.
  C makes three fresh rekordbox starts, waits 90 seconds before each first load,
  and selects the AAC rows including w10 and the HE rows when present. The MIDI
  runner still uses its row-1 sort check and OCR on every load. The collection
  includes `t2/probe.log` and its per-load `rows.txt`. The plan restores the
  runtime and the model after collection evidence is staged. Mix dumps use
  the separate `auto dumps` plan.
- Item 4 checks DirectML modules in the test account's console session and logs any
  provider/device lines from the probe. Item 6 logs processor count and the probe's
  thread calls for the current setting. The owner must change rekordbox's STEMS
  multithreading preference for the second measurement. Its UI control and saved
  setting have no verified unattended selector. Repeat the probe round from a clean
  state and retain both logs.
- Item 8 reports `PartAnalysisQuality` and `AutoAnalysis` before any MIDI load.
  The plan turns Auto Analysis off itself (above); Prioritize sound quality stays the owner's.
  Items 5 and 7 require a separately scoped pass.
- Item 9's generator adds a whole silent model chunk, the first chunk's end and
  one sample beyond it, and a 44.1 kHz 5.1 WAV. Set `RBKIT_HE_FFMPEG` to an
  ffmpeg build with `libfdk_aac` to generate the HE-AAC and HE-AAC v2 rows.
  Import the updated corpus into **RBSP Tests** once before running it.

- **The desktop session** (`session up|down|state`, `kit/console.ps1` on the owner's login):
  - The test account's existing session is put on the physical console, unlocked, by `tscon` run
    as SYSTEM through a one-shot task. The task action gives `tscon` 20 seconds in its own Windows
    job. The admin script removes the task after checking the session state. A watchdog timeout
    during a scheduler call can leave the on-demand task registered.
  - The step refuses when another account is active on the console (someone at the PC).
  - Account lookups, WTS calls and scheduler calls have per-step watchdogs within the 120-second
    runner limit. A move or disconnect is confirmed by polling WTS for up to 30 seconds.
    Disconnect requests are asynchronous. Windows can finish a session transition after a timeout.
  - When the account has no session, `session up` signs it in with FreeRDP (`sdl3-freerdp`,
    `sdl-freerdp` or `xfreerdp`, or `RBKIT_FREERDP`) over an SSH tunnel as the account. There is
    no window on the Mac (SDL's dummy driver). The password goes from the Mac's login Keychain
    straight into FreeRDP's stdin (`/args-from:stdin`): never an argument, a file or a log.
  - `cleanup --lock` disconnects the session (locked: only its password reconnects it), so the
    next `session up` needs no sign-in. Plain `cleanup` still signs the account out.
- **Elevation without UAC** (`cache install|uninstall|check`, `kit/admin-worker.ps1`): the
  owner's admin login runs the test account's own worker, with the app's command line
  (`VERB --proto 2 --targets T --manifest M --source DIR --force 0 --pipe NAME`). For install,
  the kit proposes a numerically sorted list of `X.Y.Z:<first 16 hex digits of rekordbox.exe SHA-256>`
  entries using the installed release's ORT bindings and the files on disk. The worker verifies
  that scope against its own inventory. `M` is the full SHA-256 of the staged `payload.json`.
  Uninstall sweeps with `--targets "*"`, `--manifest "-"` and an empty source. The script is the
  worker's parent and pipe server. It reports each `@target` result and treats exits 15 (scope
  or staged manifest changed) and 16 (partial completion) as failures. `check` stages the source
  and stops before launch. `cache uninstall` restores the test account's previous cache choice
  after `cache install --chosen`. With `pcSsh`, `cleanup`'s uninstall goes this way too.
  The UAC hand-off is covered by the VM suite `tests/win/suites/stems-cache-uac.suite`.
- **The probe** (`probe install|remove|state|fetch`, `kit/probe.ps1`): the research probe
  (not in the public repository; built into `build/win/probe-t1/`) is staged by `stage --probe`. Its hash
  is checked by the owner's login in an admin-only folder, and only then placed. Pioneer's DLL is
  kept in `C:\rbtest\real\` with the version it came from. `remove` puts Pioneer's DLL back in
  that install folder after checking its hash and signature, then deletes `C:\rbtest`.
  An explicit version that differs from the backup is refused. `fetch` copies the dumps and runs
  `tests/rekordbox/analysis/assemble-dumps.py`: `tracks/<row>.rb.f32` (float32 interleaved
  stereo, 44.1 kHz) with `<row>.json` for `dump-compare.py`.
- Nothing is sent on ssh's stdin: the PC's Windows OpenSSH could leave piped stdin undelivered
  (2026-11-19). The owner's login's scripts and their runner (`win/kitps-run.ps1`) are copied with
  scp to `%USERPROFILE%\rbsp-kit-admin` in the owner's profile (the test account can't write it) when
  missing or changed. A short encoded command (cmd.exe refuses lines over 8191 characters) checks
  both files' SHA-256 against the Mac's, runs nothing on a mismatch, and passes the PC check, the
  trace function and the script's settings to the runner. They are never run from the test account's
  `%TEMP%`, which that account can write. `console` (session up, down, state) retries a timeout once.

**Once, by the owner, for a run with nobody at the PC:**
- Either sign the test account in once and disconnect, without signing out (the session then
  lasts until the PC restarts), or install FreeRDP and store the password:

```
brew install freerdp
```

```
security add-generic-password -s rbstemsplus.pcTest -a rbtest -w
```

  (`-w` last: `security` prompts for the password, so it is never on a command line.)

### Kept on the PC between sessions

The corpus in the account's library, the kit in `%TEMP%\rbsp-kit`, Windows MIDI Services Tools, the
mapping, the task, the snapshots in `%TEMP%\rbst`, and the test CA. The CA comes out at the
release clean-up, or when the Mac's address changes. Loopback A/B is not kept by the MIDI service
across its restarts; `ensure` recreates it.

## What still needs the owner

- Sign-in, licence and activation in rekordbox, and anything they ask later. A restore can roll
  back the sign-in tokens, so the next launch may ask again.
- UAC and other security prompts. `uninstallcache` stops if one is waiting; so does `cleanup`
  (see above).
- Trusting the test CA (one click per new CA).
- MIDI LEARN of new rows, and of Area Left and Right after a mapping is lost.
- Decisions on the PC that are the owner's: admin rights for a session and the firewall.
- A look at rekordbox's sync settings once per session (preflight reminds).
- Every STOP: the notification names the window. Nothing on the PC closes it.
- On the Mac: approve Screen Recording for RBKit Screen after a rebuild or whenever macOS reports
  the permission is off. macOS prompts are the owner's.

## The Mac driver (tart VM)

The same commands and exit codes as on Windows, where they apply: `preflight`, `stage`,
`run T2|hits|watchtest`, `collect`, `cleanup`, and `shot` (a screenshot copied here, for
measuring the screen). `run prepare` is not on the Mac driver yet (below).

| File | Where it runs | What it does |
|---|---|---|
| `mac/vm-session.sh` | host | the commands, over SSH as admin with vm.sh's key, every call time-bounded |
| `mac/vm-session-test.sh` | host | its tests without the VM (in `make check`) |
| `mac/kit/midi-auto.sh` | VM, as root | the runner: `ensure`, `ready`, `start`, `quit`, `run`, `watchtest`, `learn`, `press`, `shot`, `windows`, `bundle`, … (its header lists them all) |
| `mac/kit/kit-lib.sh` | VM | paths and calls for rekordbox, MIDI, and screenshots in DJ's session (`launchctl asuser`). Vision OCR runs as root |
| `mac/kit/watch-lib.sh` | VM | the dialog watch |
| `mac/kit/rbkit.swift` | VM (built on the host) | the MIDI device, Vision OCR, the title matching (`match-lib.ps1`'s rules as they are) |
| `mac/kit/rbkit-screen.swift` | VM, as `RBKit Screen.app` | screenshots, the window list, the watch's test window |
| `mac/kit/verify-kit.sh` | VM | the kit's checksums |
| `mac/kit/screen.conf` | VM | the OCR regions, playlist walk and window titles for the VM |

What changes from Windows:

- **The VM** is the one `tests/harness/vm.sh up CLONE` started: headless, with rb26-template's MAC
  address (one rekordbox seat; never beside the template or another clone of it). The kit uses
  vm.sh's clone and SSH key, so it needs no settings and keeps nothing personal. Before opening
  rekordbox, the host checks the VM's MAC against the template. The guest's `midi-auto.sh start`
  checks en0 against the staged template address and the host's fresh reading when supplied.
  The staged address is covered by `SHA256SUMS`.
- **MIDI:** `rbkit midi serve` makes the virtual device "RBSP Test": a source rekordbox reads and a
  destination it writes its lights to, with fixed unique IDs, so rekordbox sees the same device
  after a restart. It runs as a launchd job in DJ's session (`io.github.rbstemsplus.rbkit.midi`).
  Presses go through its FIFO. rekordbox's lights go to `midi-in.log`, which gives the first MIDI
  and deck 1's PLAY light.
- **Screenshots and the window list** come from `RBKit Screen.app`, started as DJ with `open -W`.
  Screenshots need macOS's Screen Recording permission, and so do other apps' window titles.
  The run and the start rule of the watch require permission to read window titles. The watch's
  own test can see its test window by process and window number. The permission is tied to the
  app's code hash. `stage` rebuilds the app
  only when `rbkit-screen.swift` changed, and prints the hash; after a rebuild the owner allows it
  again.
- **OCR:** Vision (accurate, en-US, no language correction). The region is inverted and scaled
  ×1-4, as on Windows. The Mac corpus is `v1/` (and `mac/` when there is one), so there is no
  long-path copy, and no rows are known to be refused yet.
- **Start and quit:** `open -a` as DJ, then the first MIDI within 120 s, then 15 s. `caffeinate`
  keeps the display awake while rekordbox runs. Quit is an Apple event (as a user quits), after
  the watch and the PLAY check, never a kill.
- **The watch:** CGWindowList, layers 0-24, windows from 40×20. It watches rekordbox,
  rekordboxAgent, RB Stems Plus, UserNotificationCenter, universalAccessAuthWarn (the permission
  prompt), CoreServicesUIAgent, SecurityAgent and Problem Reporter. Add a process to the watch
  after it has been seen in the VM. The test window is RBKit Screen's. A run starts with
  the calibrated rekordbox main window (at least 800×500), optionally the RB Stems Plus main
  window, and optionally one rekordboxAgent window with the calibrated news title. The kit leaves
  the news window open. A quit without a run baseline uses the same window checks.
- **Not on the Mac driver yet:** `run prepare` and the Stems Cache uninstall. Windows uses UI
  Automation for them. The Mac app's probe selects a playlist only by its rekordbox ID, which the
  kit can't get from a name, so this needs a probe verb by name, or Accessibility. Instead,
  `cleanup` quits rekordbox, removes the MIDI device and stops the VM. The clone keeps its state.
  A fresh clone of the template has Pioneer's files.

### Setting up once

1. Start a clone kept for the kit: `tests/harness/vm.sh up rbkit-mac`.
2. Make the corpus with `helper/tests/perf.sh corpus build/rekordbox/corpus/v1`, then stage it.
3. **Owner,** in the VM (`vm.sh up rbkit-mac --vnc`): allow "RBKit Screen" in System Settings >
   Privacy & Security > Screen & System Audio Recording. The first screenshot without it brings
   up macOS's own prompt for it (universalAccessAuthWarn). The kit leaves it as it is.
4. **Owner,** in rekordbox (MIDI LEARN): map "RBSP Test" as in the table. `midi-auto.sh learn
   --note N --repeat 30` (in the VM, as root) presses a note every 2 s while rekordbox learns.
   `ensure` looks for the mapping at `~/Library/Application Support/Pioneer/rekordbox6/MidiMappings/RBSP Test.midi.csv`.
   If rekordbox saves it elsewhere on the Mac, change `MAPPING` in `kit-lib.sh`.
5. **Owner:** import `/Users/Shared/rbsp-kit/corpus/v1` into a playlist "RBSP Tests". Turn Auto
   Analysis off (`ensure` checks it: when it's on, new tracks bring a dialog), and keep STEMS on
   "Prioritize sound quality".
6. **Owner:** install Stems Cache in the clone's rekordbox using the RB Stems Plus app. Both
   `run T2` and `run hits` require it. `ensure` reports the installed state.
7. **The first session,** with rekordbox open:
   - `vm-session.sh shot`, then measure deck 1's title and the list header for `DECK1` and
     `HEADER` in `screen.conf`.
   - Learn the walk with `midi-auto.sh press --keys …` and write it as `WALK`.
   - Run `midi-auto.sh windows` in the VM. Record the titles of the rekordbox main window and
     the agent's news window as `MAIN_TITLE` and `NEWS_TITLE` in `screen.conf`.
   - Stage again, and write the walk here.

   `midi-auto.sh ready --expect any|hits` reports missing fields. The host checks readiness before
   starting rekordbox.

### A session

Run each command on the host, unsandboxed:

```
tests/harness/vm.sh up rbkit-mac
```

```
tests/rekordbox/mac/vm-session.sh stage --corpus build/rekordbox/corpus
```

```
tests/rekordbox/mac/vm-session.sh preflight
```

```
tests/rekordbox/mac/vm-session.sh run watchtest
```

```
tests/rekordbox/mac/vm-session.sh run T2
```

```
tests/rekordbox/mac/vm-session.sh run hits
```

```
tests/rekordbox/mac/vm-session.sh collect
```

```
tests/rekordbox/mac/vm-session.sh cleanup
```

- `stage` builds and copies the kit. `--corpus` replaces the VM's corpus, and `--dist` replaces
  the dist contents with `payload.json`. Omitted inputs keep their files and checksums in the VM.
  Stage then runs `verify-kit` and `ensure` in the VM.
- `preflight` checks: the VM running, SSH, the seat (MAC address), the kit's checksums, `ensure`
  and the notifier. `ensure` covers the tools, Screen Recording, DJ's session, the MIDI device as
  DJ's session sees it, the mapping, rekordbox's version and settings, the corpus, Vision,
  `screen.conf`, and the screen saver. It never starts rekordbox.
- `run T2|hits` takes `--from`, `--to`, `--max`, `--inject-at`, `--minutes` (1–1440 on the Mac)
  and `--keep-open`.
- `cleanup` confirms the clone stopped with Tart. Use `--keep-running` to leave the VM running.

To stop a run from the VM side, create `/Users/Shared/rbst/midi-stop`. The VM keeps
`/Users/Shared/rbsp-kit` (the kit and the corpus), `/Users/Shared/rbst` (logs, screenshots, the
MIDI daemon's files), the mapping and the permission, until the clone is deleted.

**Dry run, 2026-11-19** (clone of rb26-template, rekordbox not started): `stage` and `preflight`
ran. These were OK:
- SSH, the seat, and the kit's checksums
- DJ's session
- the MIDI device, seen in DJ's session
- rekordbox 7.2.19 with Pioneer's onnxruntime, and STEMS on sound quality
- the 20 rows
- Vision, in the VM
- the screen saver

`run watchtest` passed: the watch saw the test window by its process. `collect` and `cleanup`
ran too (the VM stopped). These still need the owner's set-up: Screen Recording, the mapping,
Auto Analysis (on), `screen.conf` and the walk.
