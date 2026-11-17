# core.h API v1: notes for the freeze

**Frozen: API v1, 2026-11-09, at commit f3c8759.** The review (3 Claude and 3 GPT reviewers, 6
skeptics, a judge) ruled on 40 issues (C1-C40, 37 real) and on the 15 open points below; its
designs D1-D11 are applied (§3). Re-review 1 (2 Claude and 2 GPT reviewers, 4 skeptics, a judge)
ruled on 14 low issues (R1-R14) with designs E1-E4, all applied (§6); the judge froze core.h once
they landed, with no further review round.
Plan references are to mac-core.md (§3 the API, §5 build, §7 tests, §9 G1), app.md (§4 the WPF
shell) and the main plan's §2.

Two things are frozen at different times:

- **core.h's shape** (entry points, handle types, callback table, ownership, threading and
  re-entry rules): what the freeze covers.
- **The .def tables** (fields, columns, texts, slots, menus, facts, events) keep growing until G4
  (1.4.0). Their rows here are starters. Ids are positions per build; core_ids_hash() lets each
  shell refuse ids from another build.

New entry points may still be added after the freeze (additive).

## 1. Design decisions (as they stand after the ruling)

### ABI, for Swift and P/Invoke with one set of declarations

1. **64-bit only, cdecl.** `CORE_API` is `dllexport` under `CORE_BUILD_DLL`, empty on the Mac,
   built `-fvisibility=hidden`; `core/build.sh lib` refuses an archive with a non-private external
   symbol, a `core_test_` symbol or the debug marker (C38).
2. **No C `bool`, no struct by value, no varargs.** Truth values are `int32_t`. Three structs cross
   by pointer, blittable, sizes checked by the unit tests: `core_menu_item` (40 bytes, out),
   `core_slot` (24, in), `core_mask_ctx` (40, in). `core_host` is 136 bytes (W0 asserts the C#
   layouts). Their reserved fields must be 0: core_new, core_text and core_mask refuse anything
   else, so a later version can give them a meaning (R14).
3. **Enums are C ints.** Parameters that take one enum are typed with it. Bit sets and return
   codes stay anonymous enums (OP15); Swift casts them (`UInt32(CORE_MENU_ENABLED)`), and gen_ids
   emits every one of them for C#.
4. **`size_t` is UIntPtr/nuint** in C#, everywhere; returned strings are `IntPtr`/`byte*`, never a
   MarshalAs return (C27; written into core.h).
5. **Strings in**: pointer + length, `CORE_NTS` = NUL-terminated. **Out**: NUL-terminated pointer +
   length; each says how long it lives.
6. **Buffer-out** functions NUL-terminate on errors too: the buffer holds "" (C15).
7. **Error codes**: `CORE_E_*`; `CORE_E_ARG` now also means "an event member of the wrong type or
   out of range"; `CORE_E_STATE` "inside a callback".
8. **No exception leaves a callback**: each C# delegate is a try/catch wrapper returning its
   entry's documented safe value ("on failure" in core.h) (C26).

### Threads, jobs, callbacks, lifetime

9. **Opaque jobs** (`post_main(job)` / `post_work(kind, job)`, run with `core_run`, dropped with
   `core_job_drop`), each run or dropped once (OP1). While the core lives a host never drops a job:
   one it can't queue (out of memory) makes it fail fast (C#: log, then `Environment.FailFast`);
   only once its loop has stopped does it drop jobs, synchronously (E1). A job that ends a state the
   core waits on (a flow's end, the quit decision, a helper run's end) is reserved when the state
   begins, so it can't be lost to an allocation failure (E2).
10. **One render per drain (D1).** `core_commit(c)` renders now if anything changed since the last
    render; the core's own commit job still ends every turn. On the Mac it renders (a no-op after
    a `core_commit`); with `"os":"win"` it only wakes the drain, and the WPF drain's `core_commit`
    at its 4-8 ms boundary is the one render (E4). A commit
    with no change renders nothing (C34). Start-up: `core_new` → `{"ev":"start"}` →
    `core_commit` → show the window → `{"ev":"shown"}`.
11. **The callback rule (D2)**, in core.h: what may be called inside a callback on main and on a
    worker. Enforced with a callback depth: CORE_DEBUG aborts; release builds refuse a nested
    `core_event` (`CORE_E_STATE`, logged once), ignore nested `core_commit`/`core_snap_current`,
    queue a nested `core_run` of a main job and run it in order when the outermost core call
    returns, and defer a nested `core_free` to that point. Scratch is reset only then.
12. **Close and quit (D2)**: `close_reply` from its own main job; a re-ask after an approval answers
    NOW (allowed inside a callback); repeats while pending get LATER; QUIT supersedes a pending
    WINDOW and one decision answers both; the quit flow waits while `modal_open()` is 1; the shell
    posts its close/terminate action out of `close_reply`.
13. **Lifetime (D3)**: `core *` stays the handle (no tokens). The core header lives inside a
    counted `life`; `core_free` (optional at exit) tears the model down, clears an atomic `alive`
    flag and releases the core's reference. Flows (any kind) and helper runs hold the life until
    they end, so `core_helper_line/_ended` and `core_flow_progress` stay legal after
    `core_free` and do nothing. Queued main jobs and not-yet-started worker jobs are dropped (their
    cleanup runs). A flow or worker job already past its liveness check may make that one host
    call after core_free. `ctx` and the callbacks must outlive all of that; shells keep them for
    the process lifetime.
14. **Flows**: completion carries the flow's kind; only the normal flow's end clears the normal
    busy state; an atomic count covers every kind; a flow whose job can't be made rolls its flags
    back (C9, C10).
15. **Helper runs (D3)**: `helper_spawn`/`helper_signal` may be called from any thread; a 0 return
    means exactly one `core_helper_ended`; nonzero means not started; runs overlap; "current" is
    per purpose (status set, estimate, run), never core-wide (C23). A run id carries its slot in
    its low 5 bits (opaque to the host) so `core_helper_ended` reaches the run's reserved end job
    (E2). Inside `helper_spawn`/`helper_signal` the host may call only the pure functions (E3).
16. **The main thread** is the one `core_new` ran on; release builds check it on the main-only
    entries (`CORE_E_THREAD`, `REFUSE`, NULL, 0, or log-once), and a main job run off main is
    re-posted (C17).
17. **Gens are per domain** (status, prepare, list, size); a main job carries (domain, gen) (C39).

### Snapshots, rows, deltas

18. **`render` hands over one reference**; the delta lives as long as its snapshot; shells apply
    every render in order, and one that fails or skips a render reloads everything from the next
    (OP3).
19. **Delta**: relative to the previous render; per table reload, old_rows, changed rows
    ascending. A table change moves its section's rev (kept, C28).
20. **Copy on write**: flat row-pointer arrays, copied once per commit when shared (OP8, C7).
21. **Ids are positions** (OP13), names are the stable identity, `core_ids_hash()` is checked at
    start by both shells (the Swift check reads the generated `CoreIds.swift`). The hash covers
    every .def row whose id crosses the boundary (sections, tables, fields, columns, slots, texts,
    menus, facts); events cross by name and are left out (R9).

### Host table and facts

22. **Required entries**: app: render, front_facts, modal_open, facts, post_main, post_work,
    dialog, close_reply, log. Watcher: post_main, post_work, dialog, close_reply, facts, step, log.
    NULL never means native (D6). Under CORE_WIN_POLICY only, `"native": [...]` in the start config
    hands post_work, slow_fact, step, admin and helper to the DLL's Windows code (the dispatch:
    §11; a listed entry the DLL's code doesn't provide fails cleanly, as a NULL entry does).
23. **`front_facts`** (renamed from `is_front`, OP5) returns raw bits; the core decides.
24. **`step` and `slow_fact` run on any core worker**, possibly concurrently; NOTIFY and REVEAL are
    steps; open URL, notify, reveal and the manifest download are short worker jobs outside the
    one-flow lock (D4).
25. **View commands are snapshot state**: the tab (`F_TAB`), selections, the row being prepared, and
    one-shot pairs (`F_PREP_SCROLL_ROW` + `F_PREP_SCROLL_SEQ`, `F_PREP_FOCUS_SEQ`,
    `F_LOG_SHOW_SEQ`). No `effect()` entry.
26. **Dialogs (D5)**: `core_dlg_int(d, what, i, j)`; `CORE_DLG_BUTTON_ENABLED`,
    `CORE_DLG_ROW_BUTTON_ENABLED`, `CORE_DLG_BUTTON_LEFT`; disabled masks inside, so a zeroed dialog
    is fully enabled. Settings' rules are `dlg_settings_enable` (today's, as the C4 verdict
    corrected them), tested.
27. **`pick_folder`** gets its texts from `T_PICK_FOLDER_MESSAGE` and `T_PICK_FOLDER_CHOOSE` (C24);
    the zip is `CORE_STEP_REPORT_ZIP` (OP4).
28. **The log (D10)**: no `S_LOG`/`T_LOG`. `host->log` is the one path for every core line; the
    shell writes it, with its own lines, to the file and the view.
29. **`core_mask(ctx, ...)`** with an explicit `core_mask_ctx` (home, names, allowed checksums) (D8).

### Events, config

30. **Events** carry members listed in `events.def`; each handler checks types and ranges
    (`CORE_E_ARG`). Row events carry `"gen"`; a stale one is ignored quietly (D9). New: `start`,
    `shown`, `rename`, `until`, `watch`. Today's mapping: `reopened` → checkRequest,
    `deminiaturized` → a quiet re-check (both M4's), no full refresh (C39).
31. **The start config is required, with `"os"`** (D7, C35); unknown keys refused (`"native"` is
    unknown in the Mac build).
32. **The JSON reader**: numbers in bounded time (C14). Paths with backslashes need the shell's
    escaper (W8a, C37); the core's tests carry a Windows path.

### Builds and tests

33. **`CORE_WIN_POLICY`**: Windows rows and code in addition to the Mac ones, the OS chosen at run
    time; the DLL keeps both columns (OP2: no CORE_DROP_MAC); slots are OS-tagged too (C16).
34. **Test matrix** (`make core-test`, 10 unit builds): unit tests for mac and win, each plain,
    ASan+UBSan and TSan *as released* (no CORE_DEBUG: the release behaviour of violations; the TSan
    one, `tsanrel`, added by R8), ASan+UBSan and TSan with CORE_DEBUG (violations abort, checked in forked children; the leak check after every test);
    `os-rows.sh`; the Swift smoke test; the fuzz drivers for the Mac config and the Windows one.
35. **Test hooks** live in their own archive, `libcore-testhooks.a` (C38).
36. **plat.h stand-in**: `core/plat_shim.h` (thread identity, mutex, condition, clocks, sleep)
    until common/plat.h exists; its Windows branch (SRWLOCK, CONDITION_VARIABLE) is written but
    unverified (OP10).

## 2. The open points, with the ruling's binding answers

1. **Opaque jobs** instead of `post_main(fn, arg)`. *Ruling: keep them; run or drop each once; a
   nested `core_run` from a pump inside a callback is deferred; after its loop stops the host drops
   jobs synchronously.* Done (decisions 9, 11).
2. **CORE_DROP_MAC** for the DLL. *Ruling: no; the DLL keeps both columns; "os" required; slots
   OS-tagged.* Done.
3. **Delta lifetime.** *Ruling: decision 13 (now 18) stands; apply every render in order; a skipped
   or failed one means reload everything next time.* Done (render's doc).
4. **pick_folder and zip.** *Ruling: pick_folder is an entry with texts from text ids; the zip is
   CORE_STEP_REPORT_ZIP; reveal and notify are steps; M6 documents each step's arguments and
   results before the shells implement them.* Done, except M6's per-step documentation (M6).
5. **`is_front`.** *Ruling: rename to `front_facts` now, and the spec mentions.* Done (core.h,
   mac-core §3.5 and §7, app.md §4.1).
6. **Typed close/quit.** *Ruling: keep, with D2's rules.* Done (decision 12), tested.
7. **Globals.** *Ruling: "no mutable globals"; static const tables are fine.* One exception
   added: the CORE_DEBUG-only live-object counter for the leak check (not in release objects).
8. **Row-pointer array copy.** *Ruling: accepted; the log table goes; the spike measures 5k and 20k
   rows with the previous snapshot held, one change per commit, full speed and 1 line/ms; chunk
   only if P6/P7/P10 fail; fix the §3.4 sentence.* Done (the sentence fixed; the spike is M7/M1's).
9. **Windows workers.** *Ruling: NULL never means native; an explicit `"native"` list under
   CORE_WIN_POLICY (D6); W-core implements it.* The config and its checks are done; the Windows
   code is W-core's.
10. **pthreads.** *Ruling: behind common/plat.h before the first llvm-mingw build (SRWLOCK,
    CONDITION_VARIABLE, GetCurrentThreadId); the DLL's import gate refuses libwinpthread.* Done as
    a stand-in (`plat_shim.h`) until plat.h exists; the import gate is release.md's.
11. **Watcher.** *Ruling: freeze its contract now: required entries, `start`, rounds by the core's
    timer and `{"ev":"watch"}`, the final `close_reply(WATCHER_DONE, 1)`, a host main loop, no
    `core_free` needed; round internals M5's.* Done in core.h and core_new; the `watch` event exists
    and is accepted; the rounds and the timer are M5's (the timer needs plat.h).
12. **Spike coverage.** *Ruling: a real reader thread calling `core_helper_line` with a stub parse
    that sets a cell, one main job per line, the real commit scheduler, `core_commit` at bounded
    drains on WPF and GCD on the Mac; full speed and 1 line/ms, 5k and 20k rows, the previous
    snapshot held; read AppKit cells and release snapshots; time the parser separately once M5
    has it.* For M7/M1. The helper-line path exists (per-run, one main job per line); the stub
    parse that sets a cell is the spike's.
13. **Positional ids.** *Ruling: accepted; names in traces; `core_ids_hash()` enforced at start.*
    Done.
14. **Event member types.** *Ruling: handler validation; missing, wrong-typed or out-of-range
    members rejected before acting; row events carry "gen"; C# name constants optional.* Done for
    every scaffold handler.
15. **Anonymous enums.** *Ruling: keep them; gen_ids emits them for C#.* Done.

## 3. The review's issues: resolution per id

Commits on `p0/core-scaffold`: **31a0e8c** (JSON numbers), **e77b0af** (core.h and the scaffold),
**99e79f2** (tests and build), **8003278** (spec mentions), and this notes commit.

| Id | Ruling | Resolution | Commit |
|---|---|---|---|
| C1 | D1 | `core_commit`; the commit job a no-op after it; the drain rule in core.h, mac-core §3.4, app.md §4.1 | e77b0af, 8003278; test 99e79f2 |
| C2 | D2 | callback depth; nested event refused, nested runs and frees deferred, scratch at depth 0; close latch | e77b0af; re-entry tests 99e79f2 |
| C3 | D3 | `alive` flag; queued workers dropped after free; the ctx sentence; header wording | e77b0af; test 99e79f2 |
| C4 | D5 | `core_dlg_int(d, what, i, j)`, BUTTON_ENABLED, ROW_BUTTON_ENABLED; Settings' rules | e77b0af; test 99e79f2 |
| C5 | D4 | step/slow_fact on any worker; NOTIFY, REVEAL; view-command fields and `*_SEQ` | e77b0af |
| C6 | D4 | partly: clocks, mutex, thread id in `plat_shim.h`; **timers and the fake clock wait for plat.h** (see §4) | e77b0af |
| C7 | keep flat arrays | log table removed (D10); §3.4 sentence fixed; the spike's sizes are M7/M1's | e77b0af, 8003278 |
| C8 | D9 | `"gen"` on select and folder; stale ignored quietly | e77b0af; test 99e79f2 |
| C9 | D3 internals | post functions return int; flow flags rolled back; commit flag cleared on a failed post | e77b0af; allocation-failure test 99e79f2 |
| C10 | D3 | completion carries the kind; outstanding-flow count | e77b0af; quit-beside-flow test 99e79f2 |
| C11 | D3 | no tokens; flows and runs hold the life; late off-main calls no-ops; free optional at exit | e77b0af; tests 99e79f2 |
| C12 | D2 host queues | wording in core.h (synchronous drop after the loop stops, background threads, dialog nonzero at shutdown) | e77b0af; closed-host test 99e79f2 |
| C13 | clone, copy, then release | `sec_w` hands the old section back | e77b0af; self-aliasing ASan test 99e79f2 |
| C14 | bounded conversion | as ruled; deterministic cases; a performance check in the unit tests | 31a0e8c; tests 99e79f2 |
| C15 | one fail path | `buf[0] = 0` on every error; `_Static_assert(CORE_SLOT_COUNT <= 64)` | e77b0af; tests 99e79f2 |
| C16 | D7 | `SLOT(name, os)` | e77b0af; os-rows.sh case 99e79f2 |
| C17 | D11 | release thread checks with the documented results | e77b0af; test 99e79f2 |
| C18 | D7 | `core_ids_hash()`; gen_ids writes it into CoreIds.cs and CoreIds.swift | e77b0af; Swift check 99e79f2 |
| C19 | D6 | `"native"` list under CORE_WIN_POLICY, checked by core_new; the Windows code is W-core's | e77b0af; tests 99e79f2 |
| C20 | D8 | `core_mask_ctx` | e77b0af |
| C21 | nested menus | any depth, parent an item covering the child's OS; Manage › Demucs v4 › Install row | e77b0af; test 99e79f2 |
| C22 | D11 watcher | required entries, lifecycle in core.h, `watch` event; rounds M5's | e77b0af |
| C23 | D3 | any-thread spawn (host side), per-run staleness by purpose, the start-failure rule | e77b0af; test 99e79f2 |
| C24 | not real (texts) | two text ids | e77b0af |
| C25 | D10 | log table dropped; app.md:591 now says `host->log` | e77b0af, 8003278 |
| C26 | D2 | "no exception leaves a callback" and each entry's safe value in core.h | e77b0af |
| C27 | D11 | ABI lines; gen_ids emits the anonymous constants; DllImports and layout asserts are W0's | e77b0af |
| C28 | not real | decision kept | — |
| C29 | tests | dfuzz (unit + fuzz), TSan release off main, core_free during a flow, re-entry, allocation failure, Mac-config fuzz, a per-input time budget, the leak check, a 60 KB fuzz seed | 99e79f2 |
| C30 | D2 | close/quit rules in core.h; logoff left to app.md §5.9 | e77b0af |
| C31 | D5 | `CORE_DLG_BUTTON_LEFT` | e77b0af |
| C32 | not real now | the plat date formatter comes with plat.h; `C_SET_UNTIL` stays a string | — |
| C33 | D1 | `start` and `shown` events; the order in core.h | e77b0af |
| C34 | D1 | no render without a change; test fixed | e77b0af, 99e79f2 |
| C35 | D7 | `"os"` required, NULL config refused | e77b0af; tests 99e79f2 |
| C36 | split mods | `mods_mac`/`mods_win`; today's app-menu order | e77b0af; test 99e79f2 |
| C37 | shell escaper | the core's tests carry a Windows path; the escaper is W8a's | 99e79f2 (+ selftest vector, e77b0af) |
| C38 | release gate | `libcore-testhooks.a` in its own object folder; gate on every release archive; a real debug marker | e77b0af, 99e79f2 |
| C39 | per-domain gens | (domain, gen) on main jobs; today's event mapping | e77b0af; test 99e79f2 |
| C40 | events | `rename`, `until`, `watch` (and `start`, `shown`) | e77b0af; test 99e79f2 |

## 4. What isn't done, and why

- **C6's timers and fake clock.** The ruling puts clocks and timers in common/plat.h (bridge
  §0a), which doesn't exist yet. `plat_shim.h` has thread identity, mutex, condition, a monotonic
  clock and a sleep; a delayed post to main, the fake clock and the helper deadlines (STOP from a
  timer) need plat.h's timer and land with it. Meanwhile the quit flow polls `modal_open()` every
  20 ms while a dialog is up (a stand-in for the timer).
- **D6's Windows code** (WinC's post_work pool, helper reader, admin and step) is W-core's; the
  config list and its checks are here.
- **W-core note (E1):** under `native`, a job the DLL can't queue (thread start or thread-pool
  submission) fails fast, as §11 and core.h's post_work ask; the host never drops a job.
- **M4-M6 behaviour behind the plumbing**: the refresh's decisions, the quit flow's playlist name
  and its stopping of the run, the watcher's rounds, the step arguments.
- **Not verified here**: P/Invoke and the C# layouts (W0, in Parallels), an llvm-mingw build of the
  core (including plat_shim.h's Windows branch), and anything timed beyond the unit tests'
  generous performance checks (the G1 spike runs at night).
- **One spec mention not touched**: the main plan's §2 "Threads" row still says `is_front`; the
  ruling named only mac-core and app.md.

## 5. What was verified (host only)

- `make check`: green.
- `make core-test` (outside the sandbox: swiftc's module cache): every unit-test variant passes
  (plain, asanrel and tsanrel with the release checks, asan and tsan with CORE_DEBUG; mac and
  win: 10 builds),
  `os-rows.sh` (24 Mac objects and both archives byte-identical with a Windows-only row in every
  table, a Windows-only slot and function included), the Swift smoke test (ids hash checked), and
  both fuzz drivers (20,000 mutations per seed, every input inside its time budget, plus a 60 KB
  numeric seed).
- The release archives pass the new gate (no test hooks, private externals only, no debug marker,
  minos 12.0).

## 6. Re-review 1: resolution per id

Commits: **6938186** (the before-freeze rules), **92b2f78** (post_work's drop rule), **a369c81**
(the spec edits), **f3c8759** (the same-round fixes), and the freeze commit that writes these notes.

| Id | Ruling | Resolution | Commit |
|---|---|---|---|
| R1 | E1, E2 | hosts never drop a job while the core lives; the quit decision is reserved before the quit flow starts (a failed reservation answers REFUSE) | 6938186, 92b2f78, f3c8759 |
| R2 | E1, E2 | a helper run's end job reserved before `helper_spawn`, the slot in the run id's low 5 bits, posted even after `core_free` | f3c8759 |
| R3 | E1, E2 | the flow's end reserved at `flow_start` (NOMEM with the flags rolled back when it can't be); a commit is never dropped while the core lives | 6938186, f3c8759 |
| R4 | E3 | `core_helper_line`/`_ended` on main and every test hook run inside an entry frame; `helper_spawn` inside `CALLBACK`; `core_flow_progress` posts nothing on main; the callback line for `helper_spawn`/`helper_signal` in core.h | 6938186, f3c8759 |
| R5 | E4 | with `"os":"win"` the commit job only wakes the drain; `core_commit` renders once; both docs reworded | 6938186 |
| R6 | test | `t_self_alias` writes a new value each pass with no snapshot held and checks the section compacted inside the aliasing write; a dfuzz variant releases every snapshot first | f3c8759 |
| R7 | test | a scaling-step counter in test builds; `1e9999999`, `1e-9999999`, `0e9999999`, and 16 levels × 255 of them, at most 40 steps each | f3c8759 |
| R8 | split flag | `warned_thread` atomic (exchange, relaxed), `warned_nested` main-only; a two-thread off-main case under TSan without CORE_DEBUG (the new `tsanrel` build) | f3c8759 |
| R9 | wording | core.h's hash doc lists what is hashed and why events aren't; a note in ids_hash.h; decision 21 | 6938186 |
| R10 | spec edits | mac-core §3.2, §3.4, §3.5, §3.6, §4 and app.md §4.1 as ruled, with E1's and E4's wording and the start-up order in both shells | a369c81 |
| R11 | comment | the tab bits are the tab the window shows now; F_TAB is the one the core asks for; the shell note in both specs | 6938186, a369c81 |
| R12 | test | `LEAKCHECK` is `== 0` under CORE_DEBUG, `== -1` otherwise | f3c8759 |
| R13 | E2, test | a flow posts its end before lowering the count; the quit test drains main before its check | f3c8759 |
| R14 | enforce zeros | `core_new` refuses `host->reserved != 0` (logged), `core_text` a slot's, `core_mask` the context's; one test each | 6938186 |

**Mutation self-checks** (run in a scratch copy, not committed): with C13 reverted, the ASan unit
build fails with a heap-use-after-free in `arena_strdup` from the aliasing write; with an unbounded
per-power scaling loop, the step checks fail (4 checks). Both pass with the code as committed.

## 7. M5, texts and pure functions (wave 2, `w2/core-texts`): decisions

No core.h change: the pure functions are internal (`core/pure.h`, included by internal.h), for M4
and M6 to call; `core_mask` (core.h's) now has its body (`mask.c`). Until the G1 API spike passes,
none of them is wired into the snapshot (the G1 rule): `core_helper_line` doesn't parse yet (M4
applies parsed lines), `core_menu_state` passes the busy and Prepare state and M4 adds the
selection, the buttons and the update notice.

1. **texts.def** holds every user-facing text of 1.3.0's app (489 rows): buttons, lights and their
   VoiceOver texts, status lines, both tabs' words, every dialog's title, message and buttons, the
   watcher's dialog and notification, start-up refusals, the menus. The Mac column is 1.3.0's words
   byte for byte: `core/tests/texts-in-swift.py` (in `make check`) requires each text, and each
   of the tables texts.def includes, to be one whole string literal of app/Sources with its slots
   where the literal interpolates (402 texts), or to follow its designated template in the script:
   the whole Swift literals it is made of and what goes in each interpolation (87 texts: "this
   Mac" or a quoted drive in a sentence, the plurals, pieces Swift joins with +). It lints tip
   phrases in every text. The Windows column is `""` (CORE_E_NOTYET) until W-core.
2. **Log lines aren't texts**: the core's log lines are written by the code that logs, word for
   word (the watcher's rounds, today's), and go to `host->log`.
3. **One text per variant** where Swift splices a word into a sentence ("this Mac" or a quoted
   drive name, "your disk", a plural): `T_LOC_CHECKING_DRIVE` / `_THIS_MAC`, `T_EST_READY_ONE` /
   `_MANY`, so a column can word each whole. Joins of parts (", ", "\n", a final ".") stay code, as
   in Swift.
4. **menus.def** is today's whole menu bar in today's order. About, its separator, Hide, a
   separator, Quit, Edit and Window are Mac-only `native` rows; About and the separator are rows
   before Update, so the positional ids after them moved (core_ids_hash covers it; the starter
   tests now find Update at 3). `menu_rule` is `validateMenuItem`'s rules unchanged.
5. **The helper's lines** are read as JSONSerialization reads them (first of two equal keys wins, a
   trailing comma is allowed, at most 512 levels, its number classes and NSNumber's conversions,
   invalid UTF-8 repaired first as String(decoding:) does), checked against Swift on ~800 lines.
   Known differences, outside anything the helper prints: integers beyond the uint64 range
   (NSDecimalNumber's own conversions) and a `\u0000` inside a string (kept; a text stops at it).
   A whole output is split by the host at "\n" bytes; Swift's parseEstimate keeps "\r\n" as one
   Character (the helper never prints a CR).
6. **Masking** (`mask.c`) is each of redact's ICU patterns as a hand-written matcher with ICU's
   leftmost, greedy, backtracking result (each matcher's comment says why its split is unique),
   linear in the text (1 MB in ~50-70 ms, -O2). Checked against Swift on ~1,700 texts.
   **Review Q7, decided by the owner on 2026-11-10 ("yes"): the core masks at least everything
   1.3.0 masks, with Unicode tables. Done (`w3/core-mask-parity`)**: the differences this item
   listed are closed for 1.3.0 on a macOS whose ICU is Unicode 17 (see below). `\b` is ICU's
   (its `\w`: Alphabetic, Mark, Decimal_Number, Connector_Punctuation, U+200C/D; it looks past
   Grapheme_Extend and Cf code points, as rematch.cpp does), so "።password=" after Ethiopic
   punctuation is masked, and so is "password=" after a space and a lone combining mark.
   Keywords and names are compared after full case folding (ICU's URX_STRING_I), so
   "Anna Straße" masks "ANNA STRASSE" and "paßwd=" is a secret. The home folder is found exactly
   as NSString finds an ASCII home: U+212A for K and the other canonical equivalents of an ASCII
   character, and no match that ends before a code point joining its last one ("/Users/dj" +
   U+0301 stays, as in 1.3.0, and the later passes see it). The tables are
   `core/mask_tables.h`, generated from this Mac's ICU by `core/tools/gen_mask_tables.c`
   (`core/build.sh tables OUT`; `make core-test` fails when the committed header differs): 13.6
   KB of tables (Unicode 17.0), inside the 10-20 KB the owner accepted; mask.o grows by about
   18.5-19.5 KB per arch (arm64, x86_64), of which __text is +5-6 KB and the rest the tables.
   The generator also checks mask.c's hand-written classes (`\s`, the case-insensitive
   `[A-Za-z]`, which code points fold to ASCII letters) against ICU. Vectors: SV_MASK_UNICODE
   (~2,200 rows: the two cases Q7 named and their neighbours, every code point where `\w`,
   Grapheme_Extend or Cf changes, every code point case mapping changes as a name and against
   its mappings, NFC against NFD, the home before joining code points, 1,500 random texts from
   non-ASCII fragments), equal to Swift.
   Where the core masks **more** by design, SV_MASK_MORE pins C's result beside Swift's: a name
   taken alone that Swift counts as one character but the core's upper bound of Swift's count
   takes as two or more (spacing marks, Hangul, conjuncts, emoji modifiers, flags and ZWJ
   sequences, Prepend). With several names, masking one can hide a later name that contains it.
   Not on the Mac, where the later name is the ASCII short name.
   Left as they were: a non-ASCII home has no canonical equivalence (no NFD tables; macOS homes
   are ASCII). The tables follow this Mac's Unicode (17.0): on macOS 12-15, 1.3.0 runs that OS's
   older ICU, and a code point assigned since then, next to a keyword or a name, can be masked
   by 1.3.0 there but not by the core (owner question Q1: accepted and documented, owner
   decided 2026-11-11; no age tables). Windows home and path rules
   (drive letters, UNC, case-insensitive C:\Users\<name>) are W-core work under CORE_WIN_POLICY
   (app.md §5.10).
7. **Where Swift traps or reads 32 bits**: `Int(x)` beyond Int's range (durationText,
   configNumber) saturates here or takes String(x); durationText's `%d` keeps the low 32 bits of
   the hours, as `String(format:)` does on arm64; NSNumber's intValue of a big double saturates
   (arm64's behaviour; x86_64 would give INT64_MIN).
8. **Vectors from today's Swift, made at test time**: `core/tests/gen-vectors.sh OUT` builds
   `core/tests/swift/vectors.swift` with app/Sources (as `make test` does) and writes
   `OUT/vectors_swift.h` (~2 MB, not committed); `make core-test` makes it first, so a Swift fix the
   core doesn't mirror fails the core's tests. `core/tests/vectors.c` compares (10,300 checks
   with `core/tests/pure.c`, which covers the Swift that can't run alone: refresh()'s buttons and
   status lines, validateMenuItem, the reasons that read the Mac's files).
9. **core_selftest** now takes about 30 us (it checks 489 texts; core.h's comment says about
   4 us): still nothing next to `--swap`'s other work.
10. **Not in this part yet**: the Config.swift and Location marker/entry-size switch to common/
    (after bridge T1a), the kept-set reader (common/), Prepare's argv building, Report.swift's
    summary lines, last problem and issue link, Settings' sheet state, the watcher's
    checkKeptSets flow and the location label (they need facts M4 and M6 define).

## 8. M6, the action flows (wave 2, `w2/core-flows-2`): API additions and decisions

Built on M5 (`w2/core-texts`): every flow text is a texts.def row and goes through M5's composers
(`w_install_note`, `w_uninstall_all_message`, `w_own_files_removed`, `w_switch_message`,
`w_location_refusal`, `w_location_removal_note`, `w_update_failed_message`,
`w_cache_failure_reason`, `watcher_dialog_build`, `root_problem`, `root_run_texts`,
`update_failure_reason`, `plus_blocked`/`cache_blocked` with `w_feature_why`). M6 added only the
texts M5 didn't have: removeInLocation's seven reasons (`T_REMOVE_R_*`) and
createLocationFolder's marker reason (`T_CREATE_R_MARKER`), checked by texts-in-swift.py.

**API additions** (after the freeze, each additive: appended enum values, new enums, appended
.def rows, one new entry point, comments). One frozen rule was read too loosely by the first
version of this branch (admin's outcome bits contradicted "Returns the RootExit outcome"); the
review rejected it, and admin now returns the plain RootExit code as frozen, with
`core_admin_note` beside it. No frozen signature, layout, enum value or rule is changed; the
worker-callback list names the new core_admin_note (an additive clarification).
1. **`core_step` 10-48**: the flows' platform steps, each documented in core.h ("The flows'
   steps") with its arguments, its results and the Swift it runs. Steps return codes, numbers
   and raw facts; the core writes every text (review Q10). 46-48 (WATCHER_CHECK,
   MIGRATE_LEGACY, REMOVE_LEFTOVERS) are startupChecks' work block (Q20). THIS_MAC_DELETE's
   "facts" and "unflag" give the launch retry's inputs and its flag's removal; the core decides
   with M5-style pure ports of thisMacNeedsCleanup and mayRetryThisMacDeletion (state.c, pinned by
   Swift vectors) and logs Swift's lines (re-review F3). MODELS_INFO "saved" reads only whether a
   saved copy exists, without hashing (F10). SPACE returns need + 200 MiB per spaceShort's rule
   (core.h; F12). SELF_DELETE: 0: the cleanup is set to run and the app quits; nonzero: it
   couldn't be started (Windows: the after-exit stub didn't launch): the host doesn't quit, and
   the core logs one line, leaves the app idle and, when args[0] names the app, says so in a
   one-button alert (T_SELF_DELETE_FAILED, owner decided 2026-11-12).
2. **`core_admin_kind`** (install, reinstall, uninstall, remove-root-folder, forget). One call is
   one attempt; the core runs runRoot's loop (rekordbox before each attempt, "Permission needed",
   `sudo -k` at the end: the optional choice of mac-core §3.5). admin's `version` and `bundle_id`
   are rekordbox's (`FACT_REKORDBOX_VERSION`, "com.pioneerdj.rekordboxdj"; "" and "" for
   remove-root-folder (bundle_id NULL on Windows), NULL and NULL for forget; Q1, F17).
3. **`core_admin_note(core *, int32_t)`** with `CORE_ADMIN_NOTE_NOT_PERMITTED = 1` (Q8): flow only,
   inside admin; the core latches it in the life (`admin_note`), clears it before each attempt and
   reads it after admin returns.
4. **`core_model_outcome`**: the model steps' outcomes, mapped to M5's `T_MODEL_*`.
5. **`CORE_PROGRESS_DELETE = 4`**; DOWNLOAD's `done` -1 / -2 mean "Checking …" / "Downloading …".
6. **facts.def rows** (appended, unmerged rows changed before merge as ruled): the cheap
   `FACT_CHOSEN_MODEL`, `FACT_CHOSEN_CACHE`, `FACT_REKORDBOX_VERSION`; generic step results
   (`FACT_STEP_FLAG/FLAG2/CODE/TEXT/TEXT2`, `FACT_STEP_URL` for URLs only); BLOCKED's raw facts
   (`FACT_ENGINE`, `_SUPPORTED`, `FACT_REKORDBOX_SUPPORTED`, `FACT_ROOT_ORT_EXISTS`,
   `FACT_PROTOTYPE_EXISTS`, `FACT_LIB_EXISTS`, `FACT_LIB_SIGNED`, `FACT_ORT_VERSION`,
   `FACT_ORT_PREFIX`); the payload's (`FACT_PAYLOAD_*`; `FACT_PAYLOAD_AGE` not set when unknown;
   no `FACT_MODEL_INSTALLED_SHA`: the model's sha is `CORE_SLOW_MODEL_SHA`, memoised, asked only
   when not forced, Q19); the location's (`FACT_LOC_*`, sizes as kind + bytes); Settings'
   (`FACT_SET_*`, the size as kind + bytes, the location as kind + `FACT_SET_LABEL_PATH`; the
   core builds the label with M5's `T_LOCATION_*`).
7. **Rows added in re-review 1**: `FACT_THIS_MAC_MODEL_FOLDERS` (i64), and `T_LOCATION_SEP`
   (" › ", locationLabel's separator); `FACT_SET_LABEL_PATH` is the '/'-separated folders between
   the volume and the chosen folder's parent, and the core joins the drive and them (F7).
8. **menus.def**: M5's M_APP_QUIT title change (NONE to T_MENU_QUIT) is M5's, noted here as the
   review asked; M6 adds no menu rows.

**Decisions**
- **Busy state.** `flow_start_ex(…, busy)` (internal): a flow whose start was a main-thread dialog
  in 1.3.0 doesn't turn the busy state on; only its own start()/busy() does, so the buttons look as
  in 1.3.0 under a confirmation.
- **Revalidate (Q6).** After every dialog: the core is still there. Every confirmation before an
  action is also about the status gen and chosen() of one snapshot, the one its text, and the
  action, come from (F1); if they moved while it was up, the action stops quietly (the busy
  marker and the status cleared, "  changed while asking: stopped" logged, and nothing more shown
  in the flow: the flag stays set, F6). Guarded: the installs, reinstalls and uninstalls, the
  complete uninstall, Update, Clear, "Saved stems found", Use "drive"? and Use This Mac? (F2).
  Exempt: the dialogs asked inside a running action (the root folder's removal, another
  account's Stems Cache, "Remove RB Stems Plus Anyway": the action changes the guarded state
  itself, and their deletions keep their own gates, rekordbox_closed and the location gate),
  "Action interrupted" and the Stems Cache offer (their texts don't describe chosen() or the
  status, the action is built after the answer from fresh state, and an invalidation would lose
  the resume 1.3.0 kept). Until M4, nothing moves gen[DOM_STATUS], so a confirmation's guard is in
  practice chosen(). M4 bumps the status gen only when what a confirmation describes changes:
  installed features, the location config or its availability, and rekordbox's version. It is
  not bumped on activation, on size measures or by the clock (F8). Pending, M4/M6: Stop Keeping
  and Prepare's "doesn't fit" flow, whose guard is the estimate's gen (DOM_PREPARE); not in this
  branch.
- **Fail closed (Q3).** A REKORDBOX step that fails is "not safe": the action stops through its
  usual failure path with "  rekordbox check failed: stopped" as the only line about it (no
  "rekordbox is open" after it, F11); a queued action (resume, the watcher's request) is kept for
  the next time. A failed SPACE or BLOCKED step stops the action the same way ("  free space check
  failed" / "  feature check failed"; accepted in re-review 1). A failed MODELS_INFO never claims
  there's no saved copy (F14). An installed model's sha that isn't a sha256 recopies it (F13).
- **Clicks (Q11, F4, F5).** A click or menu choice while a flow runs is delayed (at most 8, in
  order, "queued" logged) until that flow ends, unless the flow opens a dialog or turns busy on:
  then it is ignored, as 1.3.0's modal session and disabled buttons ignored it. Delayed clicks are
  replayed one at a time, each behind the previous one's flow. A click during an action (busy on)
  is ignored at once. The flow's "asked" latch (in the life, reset at each flow's start) is set on
  the flow thread before a dialog, the folder picker and busy().
- **Activation (Q12)** asks the watcher's request after the refresh, with reopen's guards.
- **The cache offer (Q9).** A payload.json fetched while a flow runs leaves the offer pending; it
  is re-evaluated with its guards when the flows end and no click is queued.
- **"shown" once (F15).** A second {"ev":"shown"} logs "core: shown again: ignored".
- **Late progress (F20).** core_flow_progress after its flow ended changes nothing (CORE_DEBUG
  logs it once as a violation).
- **Start-up (Q20).** startupChecks' work block runs first in the start-up flow, in
  Controller.swift's order, before anything is resumed; then welcome, the update result, resume,
  the watcher's request, the cache offer and the update check. A resumed or requested action's
  body runs after the start-up block, as 1.3.0's work queue ran it. The start-up dialogs wait for
  the work block (the state line's checksum, a This Mac deletion retry); 1.3.0 showed them while
  it ran (F16).
- **window close** refuses while an action runs (windowShouldClose), unless a quit decision is
  pending (D2 still answers both).
- **Ownership (Q24).** flow_main frees its arg after copying it; a dropped job frees it too.
- **Tests (Q22).** `core/tests/flows_test.c` is a self-consistency check of the flows over a fake
  host (scripted answers, steps and admin results; every step failing, every dialog cancelled,
  core_free at every host call; no change after a failed check), with its texts from M5's
  table. The oracle for the sequences and frames is G0's harness traces (G2), not these tests.
  The F4, F6 and F9 tests were checked to fail with their fixes reverted.
- **Not done here**: Stop Keeping and Prepare's "doesn't fit" (above); the quit flow's playlist
  name and stopping the run, the refresh and prepareChanged behind the busy state (M4, marked
  `M4:` in flows.c); configSets and the config writer stay steps until M5's common/ grammar.
  Under native slow_fact the DLL answers the model's sha from sha_memo (§11).

## 9. W-core, the Windows additions (`w4/core-win`): API additions and decisions

Every addition is under `CORE_WIN_POLICY` (the Windows builds and the core's host test build,
never build-app.sh's Mac build), so the Mac objects stay byte-identical (checked against the
branch's base for `libcore.a`, both architectures, and by `make mac-unchanged` on every commit).
No frozen signature, layout, enum value or rule changes; `sizeof(core_host)` is unchanged.

**API additions** (each additive: appended enum values, appended .def rows, comments):
1. **texts.def's Windows column**: every text Windows shows has its Windows words; "" only for
   the Mac-only texts (the password sheet, "Permission needed", Time Machine, Apple silicon,
   macOS's privacy prompt, the re-sign, other accounts' install command, Hide/Window/Close/
   Minimize; the host contract in core.h
   and pure.h keeps the shared composers from reaching them on Windows).
   Each Windows text takes its Mac text's slots, except T_UPDATE_R_STAGING (no file named;
   `w_update_failed_message`'s Windows branch) and the three naming rekordbox's folder
   (T_PREP_NO_REKORDBOX, T_ROOT_MISSING_MSG, T_ROOT_FOLDER_CLEANUP_MSG), which take `{dir}` on
   Windows: `<Program Files>\rekordbox` under Windows' own Program Files, on whichever drive
   (`w_rb_dir_slot`; the DLL's core_new reads the folder, owner decided 2026-11-12); a shell that
   composes them itself passes that slot. The shell never computes a Program Files path itself:
   the DLL derives them from `native_core_programs` (review PF3: rekordbox's parent and our root in
   nw_steps_open; the prepare helper's `--ort`, `<Program Files>\RB Stems Plus\ort\onnxruntime.dll`,
   and `--rekordbox`, the folder a worker last verified as current, still holding the same
   rekordbox.exe (one stat at the spawn, PF1), are the DLL's own whatever the core's argv says,
   `w6/win-critical`: a run without a verified current folder isn't started, an estimate gets "",
   from which it loads nothing (PF2); the R1 folder's check comes with R1, CW1. Merging w4/core-win:
   keep both `nw_steps_rekordbox_folder` (R1's fresh lookup on a worker) and
   `nw_steps_rekordbox_known` (the spawn's snapshot), PF5).
   Windows-only rows at the end. Tests:
   `core/tests/win_texts.c`; W14a's table: `core/tools/win_texts_table.py`.
2. **`CORE_ADMIN_RESTORE = 5`**: the worker's restore verb (R1's "Put Back Pioneer's File").
3. **`CORE_PROGRESS_STILL_CLOSING = 5`**: the worker's @still-closing record ("rekordbox is
   still closing…" in the status line).
4. **`CORE_ADMIN_NOTE_RESTART_PENDING = 2`**: the worker's @restart record (I20).
5. **`CORE_SLOW_R1 = 5`** with **`FACT_R1_STATE`** (0 nothing, 1 the live DLL missing, 2
   foreign, 3 `ort\`'s copy missing; `w_r1_line` its line), and **`FACT_REKORDBOX_CLOSING`** in
   CORE_STEP_REKORDBOX's results (rekordbox's process runs with no window left): the Windows
   facts set is facts.def's win rows, not a separate struct.
6. **admin's Windows codes** (core.h's admin comment): 12 protocol and 14 floor (the update-first
   offer), 13 "run again" (the attempt runs once more), 1223 UAC declined (the user stopped).
7. **Click ids** `restore_pioneer` (R1's button) and `uninstall_from_settings` (the Apps &
   features entry's Uninstall; the shell sends it after "shown"), both refused by a Mac core.

**Decisions**
- The Windows flow variants are `core/flows_win.c`, included at the end of flows.c under
  CORE_WIN_POLICY (they use flows.c's static helpers); flows.c has only small hooks. Their tests
  are `core/tests/flows_win_test.c`, included by flows_test.c (its fake host), with a core
  started "os":"win"; the Mac flows run in the same build too.
- One admin call is one elevation: no re-sign loop, no FORGET. The worker's exit 5 asks Try Again
  before each further elevation (review C2).
- A rekordbox that lingers after its window closed is waited for with "rekordbox is still
  closing…" (polls of at most 500 ms against one monotonic deadline per flow, 90 s from its first
  wait; `life->win_closing_max_ms` shortens it in tests), then counts as open, and the "rekordbox
  open" texts say it is still closing (T_WIN_STILL_CLOSING_TRY, _MSG). The line comes down on
  every way out: the action's own line again when busy, none otherwise. The checks that only read
  "updating" (readyForQueuedAction, check_resume) or only whether rekordbox runs (the This Mac
  retry) never wait (RB_UPDATING). The worker's CORE_PROGRESS_STILL_CLOSING with done 1 means
  closed: the attempt's line again (review C11-C14).
- The update-first offer is one dialog [Update, Cancel], shown only when a newer RB Stems Plus is
  known (the saved payload.json's, else "latest" downloaded and verified now); Update starts the
  update flow once the offering flow has ended (with its own confirmation, as the menu's). With
  none known, or the download failing, one alert [OK] says so (T_WIN_NO_NEWER_MSG; review C1).
- Several rekordbox version folders are the worker's (it puts Pioneer's file back in every other
  one, app.md §5.5): the core passes only the current version, as the tests check.

**The in-front policy and the aggregator** (mac-core §3.6, app.md §4.3): `win_in_front(front,
modal, tab, held)` in `core/win.c` (core/build.sh's `WIN_SRCS`: in the test, fuzz and dll builds,
never in lib's Mac archive), for M4's gate to call when the core runs "os":"win". It is the Mac's
inFront() over WPF's facts, plus **`CORE_FRONT_ELEVATING`** (front_facts bit 3, Windows only): not
in front while a UAC elevation is in flight. The aggregator adds no event: it sends the existing
"activated" (from Application.Activated and the window leaving Minimized, coalesced per turn,
never for our own windows) and, for a second start, "activated" then "check_request"
(events.def's header). Tests: `core/tests/win_front.c`.

## 10. M4, the view model (wave 3, `w4/core-viewmodel`): API additions and decisions

Built on M5's texts and pure functions and M6's flows. `view.c` is Controller.swift's refresh(),
busy() and prepareChanged(quiet:), the location's onChange and the model's sha256 memo;
`prepare_tab.c` is PrepareView.swift's PrepareTab; `status_tab.c` its StatusTab. Each function names
the Swift it ports; the tests (`core/tests/view_test.c`) name the 1.3.0 commits they pin and expect
1.3.0's own words.

**API additions** (after the freeze, each additive: no frozen signature, layout, enum value or rule
changes):
1. **The start config's paths** (core_new's comment): `model`, `root_ort`, `rekordbox`, `kept`, the
   prepare helper's arguments and the model's sha256 key; `cache_default` is defaultCacheDir.
2. **`CORE_STEP_WATCHER_CHECK`'s result**: FACT_STEP_FLAG, updateWatcherState's `off`; the core
   logs its line and shows the reminders line when it changes.
3. **`CORE_STEP_NOTIFY`'s arguments**: the title and the body (a run ending in the background).
4. **`core_test_set_now`** (CORE_TEST_HOOKS only): the view model's wall clock for the tests.
5. **facts.def rows** (appended): the cheap facts refresh() and prepareFacts() read today
   (`FACT_CONFIG_*`, the location snapshot's `FACT_LOC_*`, `FACT_REKORDBOX_RUNNING`,
   `FACT_BRIDGE_MODELS`, `FACT_BRIDGE_KEEPS`, `FACT_MODEL_SIZE`/`_MTIME`, `FACT_APPLE_SILICON`,
   `FACT_HELPER_THERE`, `FACT_STEMS_QUALITY`, `FACT_OUR_MODELS`) and `FACT_KEPT_SETS`. The cheap
   sheet also carries M6's BLOCKED facts, FACT_LIB_SIGNED and FACT_ORT_VERSION only where
   stemsCacheBlocked reaches them.
6. **schema.def rows** (appended): the Manage buttons, the Prepare tab's labels and buttons
   (`F_PREP_NAME`, `_INFO`, `_INFO_SUB`, `_PREPARE_TITLE`, `_CHECK_ENABLED`, `_UNTIL`,
   `_UNTIL_ENABLED`, `_RUNNING`, `_PAUSE_TITLE`, `_OUTLINE_ENABLED`, `_OUTLINE_SCROLL_SEQ`), the
   Status tab's (`F_STATUS_AGAIN_ENABLED`, `_STOP_ENABLED`, `_RUN_LINE`); `F_PREP_KEEP_ENABLED`
   and `C_OUT_KIND` documented (0 folder, 1 playlist, 2 intelligent, 3 another kind).
7. **events.def**: `location` (LocationMonitor.onChange), until the monitor's memo moves into the
   core. **texts.def**: `T_RUN_STOPPING`.
8. **`core_step` 49-50** (`w5/core-viewmodel-2`): `CORE_STEP_FREE_SPACE` (freeBytes of a folder,
   "doesn't fit"'s room for a higher limit) and `CORE_STEP_STOP_KEEPING` (stopKeeping(name)),
   documented in core.h with the other steps.
9. **facts.def**: `FACT_CONFIG_MAX_DAYS` (configNumber(readConfig().maxDays)), written back
   beside a raised max_gb as writeConfig(maxGB:maxDays:) does.
10. **`CORE_STEP_DATE_TEXT` (51)** (`w6/core-m4-rest`): a local YYYY-MM-DD in the host's medium
   date style (PrepareView's dateOnly), for the run's "kept until" log line, so the Mac's line is
   1.3.0's text. Chosen over the shell sending the text with `{"ev":"until"}`, which can't cover
   the date the core picks itself (the default). Asked on a worker whenever the date is set,
   kept for that date; until it comes, or without it, the core writes YYYY-MM-DD. The Mac shell answers it (HostSteps.swift).
11. **`CORE_SLOW_LOCATION` (6)** (`w6/core-m4-rest`): measureCheapState's facts on a worker, the
   memo in the core (below). `CORE_SLOW_NOT_NOW` (2, Windows only, review NW1): not measured this
   time; the core keeps its memo, its size and its 10-minute timer (armed by a first such answer
   too), where any other nonzero answer still drops them. Since the DLL drops a late answer, a not
   now also arms a one-shot retry (review LN1): a measure again after 30 s, doubling to 10 minutes;
   a good answer resets it to 30 s, LOCATION_REMEASURE (a new target) brings it back to 30 s, and a
   plain nonzero answer or the core's end cancels it.

**Decisions**
- **Rows are data, as the data source returns them now.** The probe reads table rows per frame
  through the data source (mac-core §2.3), so a cell follows the state it shows ("Checking…"
  against "Not checked", the row being prepared) without waiting for a reloadData; only the cells
  that change are rewritten, so a track's line changes one row's rev. A new set of rows (by file,
  by set name) is a new list_gen; rows appended keep it. The outline is every playlist of the tree
  in pre-order with its parent row, depth and open state; the shell expands from C_OUT_OPEN and,
  like the tab, sends nothing back for a selection or an expansion the core made.
- **Helper runs (hrun, core.c)**: each run's lines are parsed on the host's reader thread by M5's
  parser into the run's own accumulator (the playlists, the track list, the estimate), handed to
  main by the run's reserved end job; an estimate's track lines and a run's lines go to main one
  job each, dropped when their gen moved; the estimate under way gets CORE_HELPER_STOP when the
  Prepare gen moves (HelperJob.cancel), the Status checks when StatusTab's does (DOM_SETS). A run
  that can't start ends later on main with status -1 and an empty stderr (today's err names the
  OS's reason, which helper_spawn doesn't return: the library's line says "can't be read").
- **The status gen** (M6's guard) moves when the installed features, chosen(), the location config
  or its measured availability, or rekordbox's version change between two refreshes.
- **In front**: the Mac's inFront() live from front_facts, the held click with modal_open; under
  CORE_WIN_POLICY with "os":"win", W-core's win_in_front.
- **stopAndWait** runs on the asking flow's own thread (the quit flow, preflight, Update), polling
  every 100 ms for up to 30 s, never spinning main.
- **Prepare's dialogs** (`w5/core-viewmodel-2`): prepareAction's "Can't prepare", "doesn't fit"
  (with "Limit not raised") and "Not enough free space", and Stop Keeping…'s confirmation, are
  flows (flows.c, `flows_prepare_ask`, `flows_stop_keeping`) that don't turn the busy state on,
  as their 1.3.0 main-thread sheets didn't. Main decides whether a dialog is needed and copies
  what it is about (the estimate's numbers, the playlist's id and name, the estimate's
  `est_seq`); the flow asks, measures the free space and writes the raised limit; an answer that
  prepares comes back to main (`prep_ask_go`), which applies the `!estimating, !refreshing,
  estimate == e` guard with the estimate's `est_seq` and the selected playlist. `est_seq` moves
  only when the estimate changes by value (`est_eq`: the numbers, the refusal, the tracks and
  the folders), so a quiet re-check that comes back with the same estimate (leaving the app and
  coming back) keeps the answer, as 1.3.0's `estimate == e` does. If the app came back to a
  different estimate meanwhile, it checks again (recheck()) and never runs on what the dialog
  said. One difference, stricter than 1.3.0 in the safe direction: an estimate that went to nil
  and came back the same still counts as a change, so the click checks again.
  The guard is main's, after the answer, as in 1.3.0, so the dialog's flow keeps M6's revalidate
  only for the core's liveness. Stop Keeping removes the set it asked about whatever the tab shows
  meanwhile (1.3.0's captured `s`), then main forgets its state and count and reloads. A click
  whose dialog can't start (another flow holds the flow thread, or out of memory) is ignored and
  logged as a click while an action runs. Stop Keeping… is gated by F_STATUS_STOP_ENABLED, as
  Prepare by F_PREP_PREPARE_ENABLED. **A change from 1.3.0** (owner decision 2026-11-12): Stop
  Keeping… is off while an action runs (the busy state, `action_running`: an install, an
  uninstall, a reinstall, the app's update, the report, a drive's clearing), because the action's
  flow holds the flow thread and the confirmation couldn't be asked; 1.3.0 left it on and asked.
  status_update_buttons runs when a flow starts and ends and when the password turns busy on.
  During a flow that doesn't turn busy on (the start-up checks, the watcher's request before its
  action, Prepare's dialogs) it stays on, and a click there is ignored and logged. The refused form is unreachable from the button (off for a
  refused playlist, and the held click needs it on), as in 1.3.0; its test asks the flow directly.
- **The Help menu** (`w5/core-viewmodel-2`): "help", "troubleshooting" and "issues" open
  Paths.swift's homeURL, troubleshootingURL and issuesPageURL through CORE_STEP_OPEN_URL as a
  short worker job (D4), during an action too (the app delegate's items stay on). The URLs are
  the core's constants (view.c), one copy for both OSes.
- **Done in `w6/core-m4-rest`**: checkWatcherOn on activation, as CORE_STEP_WATCHER_STATE
  (measure only; M72); the helpers' deadlines (runPrepareHelper's timeouts:
  --playlists and --tracks 60 s, an estimate 300 s, a Status check 120 s, a run none) through
  plat.h's cancellable timer, which plat_shim.h now carries with plat.h's names and contract (a
  thread per timer; the core on plat.h uses the real one); main sends CORE_HELPER_STOP when one
  passes; the click gate (an action's click starts only from a button shown on, a menu choice only
  when validateMenuItem allows it; flows_test.c's fake host, which never sends start, clicks with
  the test hook `click_gate_off`); the "kept until" line (item 10); VM4 (below); LocationMonitor's
  memo: CORE_SLOW_LOCATION measured at start, on activation, on mount, unmount, rename and
  bridge_state, and after an action, one at a time (a rerun when asked meanwhile); its facts
  replace the cheap sheet's location facts once one came, and a change runs onChange
  (view_location_changed). A host that answers nonzero keeps today's snapshot and
  `{"ev":"location"}`.
- **Done in `w6/core-m4-rest-2`**: the stems' size in the memo (LocationMonitor.measure and walk:
  the size stands while it is the folder in use's and under 10 minutes old, else CORE_SLOW_SIZE_WALK
  on CORE_WORK_WALK, one at a time with one more when asked during it (MR1), published if that
  folder is still in use; with no folder in use, not connected or failed; the first measurement is
  update(size: true), MR3), the 10-minute timer (plat.h's, armed once the host measures, cancelled
  with the size reset when the host declines), LOCATION_REMEASURE's invalidateSize (the only thing
  that voids a walk under way). The flows read the memo: their cheap facts carry its location
  facts, PAUSED is the memo's, LOCATION_NOW gets the memo's folder in use as args[0] and its kind 4
  is that snapshot's size (MR2), and SETTINGS may answer size kind 4, "the core's size". The
  tab-fix scenarios (view_test.c, flows_test.c), with what each pins: 0da5b7a, the run's log by
  playlist ID with paths masked (t_run_log_private; the outline's left alignment is the owner's
  round); 1ab82da, no check during an action (t_action_checks, t_status_quiet_during_action) or
  from a tab not in front (t_action_checks), not its "a run's list isn't overwritten by a check"
  or "a rekordbox session seen with nothing" parts; e6d8c06, the check on the first measurement
  and a drive gone (t_location_changed), the folder in use alone moving (t_folder_moved), new
  settings or location, only in front (t_settings_checks), coming back (t_come_back_quiet);
  9e34800 (t_run_not_saved, t_run_counts); d934b51 (t_action_checks, t_prepare_off_during_action,
  t_update_stops_preparing); the others as named in view_test.c. None for 39b025d (column widths:
  the owner's round) and 4e8711a (tccutil in the root uninstall script: the host's, run in tart).
- **Not done here** (M4's remaining work): the Mac shell's CORE_SLOW_LOCATION and size kind 4
  (M7; until then it keeps its LocationMonitor). The Mac shell's cheap sheet
  (app/Shell/ShellFacts.swift, `w6/mac-shell-facts`) writes every cheap row of facts.def,
  FACT_CONFIG_MAX_DAYS included (M7); the Windows host table (W8a) needs steps 49-51.
- **A's traces as snapshot frames in the core tests**: skipped (owner decided 2026-11-12); G2.1 in
  tart compares the real app with A's traces.
- **Left from the review** (ruled non-blocking):
  - **M3, queue the runs.** The 32 run slots (the run id's 5-bit slot) hold a normal Status reload
    (the helper keeps at most 5 sets: 11 runs), but a host whose stopped runs never end can run
    out. A FIFO of pending runs, started when a slot frees and the stale ones dropped when their
    gen moves, needs its own review; due before the Windows shell drives the Status tab.
  - **M4's range fields.** `{"ev":"until"}` now refuses a date outside tomorrow to a year from
    now (by local day) and an impossible one; the shell will need that range to draw the picker:
    additive `F_PREP_UNTIL_MIN` and `F_PREP_UNTIL_MAX` (YYYY-MM-DD), before W8's shell work.
  - **VM4, the facts after a dialog** (done in `w6/core-m4-rest`: prep_ask_go reads the facts and
    repeats the guard, with Raise's limit carried in prep_go_msg). It ran on the facts as they were
    when the answer came, without checking them again; 1.3.0 keeps the facts it checked before the sheet
    and reads them again only after Raise. It needs config.ini or the location to change outside
    the app while the sheet is up, with no event; the helper still enforces `--max-gb`. Fix:
    split run_selected so the caller passes a prep_now; in prep_ask_go read the facts and repeat
    prepare_action's guard (problems, cache_folder, `|max_gb - want| <= 0.001*max_gb`, where
    `want` is the raised value when Raise was chosen, carried in prep_go_msg, otherwise
    e->max_gb); on a mismatch, prep_check(c,1,0).
  - **MR11, the memo's strings in a flow's query** (`w6/core-m4-rest-2`'s review; done in
    `w6/core-mr11`: view_loc_q, the strings whole in the flow's arena, out of memory the query fails and the flow
    stops (t_loc_oom), t_loc_long). view_loc_now copied the memo's drive and folder in use into fixed buffers (256
    and 1024 bytes): unreachable then (Windows didn't answer CORE_SLOW_LOCATION; Mac paths fit
    PATH_MAX and 255-byte names), and due before M7 or W8a answers CORE_SLOW_LOCATION.

## 11. The "native" dispatch (wave 5, `w5/core-native-win`): decisions

Decision 22's `"native"` list, wired. No API change: core.h's host table, its rules and the config
keys are as frozen; everything here is under `CORE_WIN_POLICY` (core/build.sh's `WIN_SRCS`, never
lib's Mac archive), so the Mac objects are unchanged (`make mac-unchanged`).

**Decisions**
- **The M7 review's items** (w6/mac-shell-facts; done in `w6/mac-shell-parity` unless said):
  - **M71, facts by part: done.** core.h's `core_facts_parts` (additive) with `CORE_FACTS_REFRESH`,
    `_PREPARE` and `_KEPT`; host->facts writes at least the parts asked for and may write more (the
    Windows host, which writes every row, stays correct). `view_read_facts(c, f, parts)`: REFRESH
    for refresh, the location's change, the start, the flows' facts and their revalidation;
    REFRESH | PREPARE for prepareFacts; KEPT for the Status tab's sets. facts.def lists the parts.
    The Mac shell reads prepareFacts' files and the kept sets only when asked: 1.3.0's pattern. The
    view tests' fake host writes only the parts asked for; a read-count test (no kept-set or
    prepareFacts read on activation with Manage in front, one kept-set read with Status in front).
  - **M72, the reminders line: done in the core and the TEST shell.** LOAD and UNLOAD answer
    FACT_STEP_FLAG (the state afterwards; not set: nothing changes), and activation asks the
    additive `CORE_STEP_WATCHER_STATE` (52, after M4's DATE_TEXT 51), measure-only, as a
    reserved job on the flow queue. Tests: off then on at activation, off then on after a
    Reinstall, cleared after the last feature's removal, each change to off logged once. **Left for
    the switch-over** (it changes shipped bytes): Actions.swift's installWatcher and removeWatcher
    without their updateWatcherState, so the core is the line's one owner (until then, in a TEST
    build, the line can be logged twice); the prepared commit is on `w6/mac-shell-watcher-seam`.
  - **M73, the Mac sheet's tests: done.** shell-test: cacheBlocked branch by branch with stand-in
    reads (the facts written, codesign and the version read only where stemsCacheBlocked reaches
    them), config.ini fixtures (unreadable, a chosen folder, none), the bridge's models (only with
    rekordbox's own model), and each part's rows.
  - **CORE_SLOW_LOCATION (w6/core-m4-rest):** the shell answers it nonzero for now (its own
    snapshot and `{"ev":"location"}`, as core.h allows); moving onto the core's memo needs the
    mount, unmount, rename and bridge_state events from the shell, with LocationMonitor's size walk
    kept for Settings.
- **Trampolines, not new call sites.** Every host call passes `life->host.ctx` first; the shell's
  entries need the shell's ctx and the DLL's need their own state. So with entries listed,
  `core_new` keeps the shell's table (`life->shell`) and makes `life->host` a table of
  trampolines whose ctx is the life: a shell entry calls the shell's with its ctx, a listed one
  the backend with its state. `life->native` is then 0: the core's call sites are unchanged and
  reach a listed entry as any host entry. One indirect call more per host call, on Windows only.
- **The backend** (`core/native.h`'s `native_backend`) has core_host's entries with its state
  first, plus `open(c)` (on main in core_new, after the config: the backend copies what it keeps,
  since core_free frees the paths while flows and runs may still call it) and `close` (once, from
  the last reference's release). An entry the backend leaves NULL is "not provided" and fails
  cleanly, as before; an `open` that fails makes core_new fail. Only the DLL that links win/c's
  modules (`CORE_NATIVE_WIN`) has one (`native_win_backend`); the host test builds have none, and
  the tests put a fake one in with `core_test_native` (CORE_TEST_HOOKS, internal: not exported).
- **The elevation bit.** With admin native, the shell can't see a UAC elevation, so the core's
  front_facts trampoline adds `CORE_FRONT_ELEVATING` while a native admin call is in flight (a
  count in the life: the whole call, from before ShellExecuteEx until the worker's exit is
  handled, app.md §4.3). A shell whose admin isn't native keeps setting the bit itself.
- **The DLL's backend** (`win/c/core_native.c`, core/build.sh dll's `DLL_WINC`, with
  common/plat_win.c and the pinned pocketfft it needs): post_work on two serial queues (FLOW,
  QUIT), a maintenance queue (WALK) and the thread pool (FACT, BACKGROUND), failing fast when a
  job can't be queued, as core.h asks of a host; helper_spawn and helper_signal through
  helper_proc_win.c (`paths.helper` the exe; a call with `--keep` and no `--estimate` is a run,
  stopped by stdin's EOF; any other is terminated; stdout split into whole lines up to 4 MiB,
  a longer one dropped and logged once; stderr on its own pipe, drained by its own thread, and
  the end's err its last 4 KiB, as core.h asks); slow_fact's MODEL_SHA and BRIDGE_SHA
  (FACT_SHA256 of the key's file, memoised by win/c/sha_memo.c: keyed by the path and the open
  handle's size, 100 ns last-write and change times (FileBasicInfo) and 128-bit file id
  (plat_file_stat; a reparse point such as a WOF-compressed file is not refused), read before and
  after the hash and stored only when they match, so the install flow's model check (flows.c) and
  the view model's ask hash an unchanged file once; the stamp is a cache key, not proof of
  content: a same-size rewrite that keeps or restores it hits, core.h's contract, as on the Mac);
  DRIVE (FACT_FREE_BYTES, what this account may still write on the location's volume;
  FACT_DRIVE_STATE unset, as on the Mac), SIZE_WALK (FACT_SIZE_BYTES, entriesSize) and ALPHATHETA
  (FACT_SIGNATURE_OK, compat.c's memoised AlphaTheta pin; nothing there is "not signed", no answer
  from the pin "couldn't be measured") through win/c/native_facts.c; R1 (key: the current rekordbox
  version folder; FACT_R1_STATE through native_facts.c's native_r1: compat_r1_facts against our
  bridges' list from the app's verified saved payload and installed-release record, checked with
  the compiled-in keys; without either, an unlisted bridge counts as ours, as the watcher's rule).
  step (`w5/core-native-steps`; win/c/core_native_steps.c over win/c/native_steps.c's rules,
  host-tested with fakes): PAYLOAD (update.c's update_check over WinHTTP for "latest" and "fetch",
  the saved payload\ under its floor for "saved" and "check", update_seen for each verified one,
  the age from payload\payload.json's last write; manifestDue with the version list's "blocked" part from BLOCKED's facts; FACT_PAYLOAD_APP_VERSION and _BRIDGE_SHA are the windows block's) and
  CACHE_PREPARE (the flow payload's bridge, `support\payload\<version>\<windows.bridge.file>`,
  hashed uncached against its windows.bridge.sha256 and handed to the native admin, which stages
  it only when the verified installed-release record names it, NA2; Pioneer's ORT's sha256 from
  compat.c's read of the current version folder, `paths.rekordbox` on Windows); DOWNLOAD
  (`w5/core-native-steps-2`: update.c's update_fetch_asset from the payload's release tag into
  `support\payload\<version>\`, the model under its own `model.file` name, resumed from a part;
  progress -1 "Checking", then bytes of the total, else -2; no reuse from another payload's folder
  yet); REKORDBOX (compat.c's process list, and rekordbox.exe's visible windows: running with none
  is the linger, FACT_REKORDBOX_CLOSING); SPACE (spaceShort's measure with Windows' files: the
  model's download left, its copy and rekordbox's model file in the support folder; for the cache
  Pioneer's onnxruntime.dll saved under the root's `pioneer\<v>\` unless SHA256SUMS completes that
  set and ort\'s copy on the root's volume, the bridge staged in the current version folder on
  its own; each file charged to its volume, the ones on one volume summed).
  `w5/core-native-steps-3`: the support folder's small files, as the watcher reads them, written
  atomically with the core's own texts (SET_CHOSEN's `installed`, BUSY_MARKER's `busy` with this
  process's pid and start time; a failed write is logged and the step still answers 0, as the
  Mac's `try?`), WELCOME (`welcomed`), REQUEST (`request`'s age from its last write, 0 when in the
  future; "take" removes it); PRUNE_PAYLOADS (once the flow's model is there, the other versions'
  folders listed and deleted through one handle on `support\payload`, opened without following
  links, support's too: a link at either keeps everything, as the Mac's mayRemove); BRIDGE_SHA and STATE_LINE
  (the current version folder's onnxruntime.dll and rekordbox's model hashed through the backend's
  sha256 memo; STATE_LINE's "bridge" is R1's live DLL being ours, its "chosen" Swift's tuple text);
  CACHE_FOLDERS (the current version folder, and the root, nw_steps' root; neither followed);
  UPDATE_CHECK (update_in_place: 1 not `<apps>\<own>\` or not `current`'s; 2 the apps folder not
  writable); OPEN_URL (http and https only, ShellExecuteW) and REVEAL
  (SHOpenFolderAndSelectItems), COM initialized on the calling worker for the call.
  `w5/core-native-steps-4`: CONFIG_WRITE and CONFIG_SETS (`support\config.ini`, win/c/native_config.c's
  copy of writeConfigValues and configSets, common/cfg_ini.c's grammar; compared with today's
  Swift in win/c's fixtures, which need Swift to write config.ini: a sandbox stops it, and then
  the fixtures are made again at the next run and winc-test fails unless
  RBSP_WINC_NO_SWIFT_WRITE=1; one known difference: Windows never writes a config.ini over
  CFG_MAX, 64 KB, which it couldn't read again, where the Mac's writer does), FREE_SPACE (the free bytes this account may use, GetDiskFreeSpaceEx), STOP_KEEPING
  (`paths.kept`), CLEAR_THIS_MAC (native_steps.c's native_clear_cache: only when
  `paths.cache_default` is exactly `support\Cache`, else refused and logged; `Cache` deleted by
  that name through one handle on support opened without following a link, as PRUNE_PAYLOADS; a
  link or reparse point there is left, FACT_STEP_FLAG). Not native yet, for want of the shell or a location monitor in
  the DLL:
  PAUSED and LOCATION_* (the location monitor), SETTINGS and THIS_MAC_DELETE
  "facts" (its snapshot), NOTIFY (a toast needs the app's AUMID), DOCK, LAUNCH_AGENT_*,
  WATCHER_CHECK and SELF_DELETE (the shell's after-exit stub), REPORT_ZIP (the zip is the shell's,
  report.h). `w5/core-native-steps-5`: REMOVE_OWN_FOLDERS (support, or all of it but
  `originals\`, told by its file id whatever its name's case; only when paths.support is this
  account's LocalAppData + `\rbstemsplus` as the DLL reads it (ASCII case ignored), else, or with
  none, "refused to delete" and nothing deleted, review S53; support opened without following a
  link, any error but "not there" deletes nothing in it; the saved stems, `Cache\`, and the logs,
  `Logs\`, are support's own entries on Windows and go with it, so no folder the shell's paths
  name is deleted on its own, review S43's rule; a link or reparse point is left and counts as not
  gone only at support, `Cache\` or `Logs\`, and a file at support is refused; any other link in
  support is removed as itself, what it points to untouched, as the Mac's), APP_FATE (0 with FACT_STEP_PATH the
  apps folder when the app runs from its own install and may write that folder: UPDATE_CHECK's
  rule; else 2; never 1), LEFTOVER (support\busy's action when its writer is gone, waited for up
  to 2 s; this process's own live marker is none, one of its pid with another start, a crash's
  pid reused, is offered, native_leftover, review S55; a writer whose start can't be asked counts as live;
  no `sudo -k`). `w5/core-native-models`: MODEL_COPY, MODEL_RESTORE and MODELS_INFO through
  win/c/models.c, Models.swift's port (app.md §5.3): rekordbox's model folder is paths.model's
  (`%APPDATA%\Pioneer\rekordbox6\models\demucs3_model\`), the saved originals
  `support\originals\` (`<sha256>.onnx` and `.engine`), ours the built-in model (Paths.swift's,
  checked by win/c's run.sh), the verified saved payload's model and previous_models. Each folder
  is opened without following a link and every name used through that handle, so a link where a
  model is expected is "can't be read" (MODEL_COPY: "isn't a file"); only this code's temporary
  files and a damaged copy it set aside itself are ever removed; Pioneer's model is saved and
  verified before ours replaces it, and the replacement is a rename over it after a flush, a check
  and a second hash. Differences from the Mac: placing a saved copy checks for the name first
  (plat.h has no exclusive rename; originals\ is the account's own); the copy is flushed as it is
  written, so a failed flush reads as the copy's own failure; the quitting wait (`critical.run`)
  comes with the shell's critical_begin/end (the S3 queue); a model rekordbox itself writes between
  the second hash and the rename is the Mac's window too (Models.swift's design; the core runs these
  steps only with rekordbox closed and Pioneer's updater not running: FACT_REKORDBOX_OPEN,
  FACT_PIONEER_UPDATING; review MD2); a look at rekordbox's model that fails is never "none" and its
  folder's link is never "nothing to put back" (MD1, MD7); originals\ is opened through a handle
  on support, neither followed, so a linked support or originals is refused before anything is
  read, deleted or written there (MD5; links in the folders above support are followed, as on the
  Mac, and support is made only when missing, so a dangling junction there makes nothing: MR1, MR3;
  native_support_write likewise); MODELS_INFO's
  FACT_ORIGINALS_VERIFIED fails closed: a saved copy that can't be opened or read, a `<sha>.onnx`
  that isn't a regular file (a reparse point: WOF, dedup, cloud; VU2), an originals\ that can't
  be opened (not missing, not a folder, not a link) or listed whole, counts as verified, so Remove
  Anyway keeps it (the Mac safe-remove review's J1); FACT_ORIGINALS_SAVED likewise never says
  none for an originals\ that can't be read or a lone non-regular `<sha>.onnx` (VU4, VQ4). One
  look at originals\ per decision (MP1). MODEL_RESTORE: an originals\ that can't be opened or
  listed whole is MODELS_NOT_BACK; a `<sha>.onnx` that can't be opened or isn't a regular file is
  left out of the candidates (a good copy next to it still goes back, the Mac's SR2/MP6), but
  makes "already in place" and "no saved copy" MODELS_NOT_BACK instead (VU1, VQ1; a dedicated
  code waits for the Mac's). MODEL_COPY over a model that isn't ours fails MODELS_NOT_SAVED when
  originals\ can't be opened or listed whole or holds such an unknown copy (VQ2). Stricter than
  the Mac by design: a reparse or linked `<sha>.onnx` is a real copy that can't be checked (VU2),
  so install refuses beside one even over a model provably Pioneer's, and win/c's fixtures count
  those rows as differing for good (VQ3); MIGRATE_LEGACY answers no (1.0's
  layout is the Mac's). win/c's fixtures run 1,764 generated cases through Models.swift and
  models.c (install, a bad copy, restore, what's left behind, saved and verified originals and
  their order; the outcome and both folders afterwards must agree; Swift must be able to write,
  as for config.ini). `w5/core-native-models-2`: REMOVE_LEFTOVERS (not while appBusy: the model
  folder's `.hdemucs.new` and `.hdemucs.restore` and originals\'s `<sha256>.onnx.part`, each by its
  fixed name and as `<name>.<digits>.<digits>` (a run's own name, review MD3/WF1), each only as a
  regular file; the update's leftover folders are update.c's own sweep) and REMOVE_LOCATION
  (native_steps.c's native_remove_location: the path `X:\…\RB Stems Plus Cache.noindex` with plain
  components, opened one component at a time from the drive's root without following a link and
  never onto another volume, common/marker.c's check of the folder's marker against the id, then
  the model folders and "._" files (Clear) or everything with the marker last and the folder only
  once it is empty (the uninstall); no progress counts yet; the `\\?\Volume{GUID}\` form waits for
  T3c's Windows path rules and is refused as "not connected"). Its review (MT1): that last delete
  is common/plat_win.h's plat_remove_empty_dir_at, by the identity taken through the handle that
  was checked, one no-follow handle, never by name and never recursive (a file written in or a
  folder swapped in meanwhile stays, 7; on FAT32/exFAT an empty folder recreated in the same
  entry slot can share the id and be removed, the Mac's worst case: MQ1); the marker skipped in any case (MT2); an id exactly 36
  characters (MT3); names of any length (MT5). Mac follow-up (outside mac-unchanged): Location.swift
  deletes a case-renamed marker (`.RBSTEMSPLUS-CACHE`) as an ordinary entry, MT2's twin.
- **core_win.h (additive, `w5/core-native-steps-5`): the DLL's Windows-only exports.** Beside
  core.h, never in the Mac's library, all `core_*` (the export gate): the one copy for the WPF
  shell's start (`core_win_one_take`, `_window`, `_window_close`, `_close`: lifecycle_win.c's
  Local\ mutex and message-only window, the second start's hand-off; CORE_WIN_ONE_* are
  lifecycle.h's LIFE_ONE_*, asserted; all four on the UI thread, which owns the mutex) and
  `core_win_process_start` (another process's start time, the busy marker's clock). The DLL now
  links win/c/lifecycle, lifecycle_win and osversion_win; core/build.sh checks the five are
  exported. Unverified on Windows until the VM run.
  `w6/win-critical` appends `core_win_critical_running` and `core_win_critical_wait(ms)`: win/c/shell.c's
  CriticalWork, one per process (`critical_process`), which the DLL now links; the gate checks the
  seven. The elevated worker's run counts in it (native_admin.c, from its first pipe line to the
  elevation's end). The same branch makes UPDATE_SWAP (update_step_swap with the shell's quit,
  core_win_set_quit; no quit registered: 3, nothing started; its download's progress as DOWNLOAD's)
  and UPDATE_RESULT (update_result_take: FACT_STEP_FLAG, FACT_STEP_TEXT the "failed <key>" line,
  said once) native.
  `w5/models-critical` appends `core_win_critical_close()` (the gate checks eight): CriticalWork's
  admission, closed one-way by the shell's CloseGate when the quit is decided (Proceed,
  WM_ENDSESSION(TRUE), WPF's session-end Closing, the veto's quit; review MC1/MC2). After it no
  new step starts: a new elevation answers ADMIN_EXIT_USER_STOPPED with no UAC prompt
  (native_admin_run's admit; flows_win.c logs "UAC declined, or the app is quitting: stopped",
  MC5), and MODEL_COPY and MODEL_RESTORE, which count in CriticalWork for the copy through the
  rename only (models.c's critical_try_begin, models.critical NULL being the process's; the save
  and the prompts stay outside, as Models.swift's critical.run: MC3/MC4), answer MODELS_STOPPED
  (1, no code) with nothing changed. A step already running, or the worker already past its
  prompt, is still waited for (review WC1/WC2, "must land with models"). Follow-up (MC5, shared
  flows.c and core.h, so it touches mac-unchanged): a quiet stop for a refused MODEL_COPY or
  MODEL_RESTORE (today a refused restore offers Remove Anyway, which can't show while quitting
  and counts as Cancel: nothing is deleted).
- **BLOCKED on Windows (additive, `w5/core-native-steps-2`).** facts.def appends one Windows row,
  `FACT_ORT_LISTED` (BLOCKED "cache"'s, and the cheap sheet's for the view): Pioneer's
  onnxruntime.dll's sha256 is in the verified payload's `windows.compatible.ort_sha256`. The core
  decides with `cache_blocked_win` (lights.c, CORE_WIN_POLICY only; flows.c's cache_why and
  view.c's gate call it for "os":"win"): stemsCacheBlocked's checks in its order, the version
  prefix replaced by that flag, so FEAT_ORT_UNLISTED ("checks again") when the sha256 isn't listed.
  Windows has no built-in compatible list (the Mac falls back to its builtInCompatible when no
  payload.json is saved, so it always has one): with no verified payload nothing is listed, and
  rekordbox, the engine and the ORT are each "not supported yet, checks again", which also makes
  the launch's check due (manifestDue's blocked part). That blocked part (blockedByVersionList) is
  the core's own rules over the native facts (native.h's `native_blocked_by_list`: plus_blocked
  and cache_blocked_win, each reason counting only when it "checks again"), so rekordbox not
  installed (no current version folder), no engine yet and a standard account (the DLL's
  `native_user_is_admin`) never make it due; a part whose facts have no answer counts as 0, and
  it is asked only when the age and version gates say "not due". The Mac objects are unchanged
  (os-rows). Every other kind
  returns CORE_E_STATE, as a step entry not provided; the paths that checked for a NULL step now
  call it and get that code. A shell lists `"step"` in `"native"` only once every kind its flows
  reach is native (REKORDBOX, BLOCKED, SPACE, DOWNLOAD, UPDATE_CHECK...): a partial set lets a flow
  run further than a NULL entry did (review ST5; the update flow now stops on a failed
  UPDATE_CHECK). The DLL gate is
  now app.md §7's allowlist (KERNEL32 required; ADVAPI32, USER32, SHELL32, ole32, comctl32, ntdll
  and the UCRT) and no export but `core_*`. Unverified on Windows until W13a's VM run.
- **Native admin** (`w5/core-native-win-2`; win/c/core_native_admin.c, its rules in
  win/c/native_admin.c, with the glue that watches each elevation's lines, driven by the client
  tests' fake elevation): one call is one run of admin.c's client: the worker it picks (the app's
  own bin\ worker, named by the verified installed-release record's `windows.app.admin_sha256`,
  install-worker first when it is missing: two UAC prompts, I75), elevated with admin_win.c, UAC's
  prompt owned by the first visible top-level window of the core's main thread. Before every
  elevation of a bin\ worker (the install-worker runner too) the client's hold callback holds it
  with plat_owner_hold (the exe and its 4 folders up to Program Files, opened sharing READ only:
  owned and writable only by Administrators, SYSTEM or TrustedInstaller, no reparse point; the
  volume root isn't checked), hashes the exe again under the hold (uncached) against the sha256
  it was picked by, and releases the hold as soon as ShellExecuteExW returns (the elevation's
  started callback); a refusal or a different hash is 4 with the path logged, no UAC prompt,
  never a fallback to the app folder's copy (reviews NA4, NT1; app.md W13a A7). The source folder
  (`support\admin-source`) holds the installed-release pair (the one that verified: the record's
  folder, or its `prev\` after a stopped write, review NA5), the app folder's worker and, for the
  install, the flow's checked bridge (set by CACHE_PREPARE, the native step's; until then an
  install is "a check failed", 4), its copy hashed again against the sha256 the flow checked
  ("exactly those bytes"; that hash is uncached, sha_file, never the slow_fact memo). The install stages only the bridge the app's own verified record
  names (its `windows.bridge.sha256` and `.file`); any other, or no record, is "update the app
  first" (12) before any UAC prompt, since a newer release's bridge beside this app's record
  mixes releases (I76, review NA2). A source folder that can't be prepared is 4 with a verified
  record (an update wouldn't help), "update first" without one (NA6). The kinds map to the worker's verbs (install and reinstall: install);
  FORGET is 0 at once. Returns: the worker's code, 12 or 14 for "update the app first", 1223 for
  UAC declined; a worker that couldn't be started is 1 (the action failed), never core.h's "< 0"
  silent stop, which is the Mac's cancelled password (core.h's Windows paragraph says so). Each
  run logs its result line as the Mac's host does ("  ok: <verb>" or "  FAILED: <verb> [exit n]",
  the raw code, a negative or NTSTATUS one as 0x%08X), and a worker that couldn't be started
  logs ShellExecuteEx's error (review NA3). The pipe's first line is
  ADMIN_ATTEMPT 1, `@still-closing` STILL_CLOSING 0 and `@closed` (rekordbox's process gone; the run's
  end as the backstop) STILL_CLOSING 1, `@restart` the RESTART_PENDING note. The root is the worker's,
  `<Program Files>\RB Stems Plus` from `native_core_programs` (Windows' own Program Files, read once
  by core_new: plat_program_files, the worker's rule); the keys payload.json is checked with are compiled in by
  core/build.sh dll from keys/ (`native_key_texts`). bundle_id and ort_sha are unused: the worker
  checks rekordbox's files itself.
- Tests: `core/tests/unit.c`'s `t_native` (the listed entries reach the backend with its state,
  the others the shell with its ctx; a missing entry and a failed open fail cleanly; close once,
  on the thread that drops the last reference: a run's end on a reader thread after core_free;
  a good open followed by a failing core_new, through `core_test_native_default`, the test
  builds' one mutable hook); `flows_win_test.c`'s `t_win_native_admin` (the install's admin on
  the flow thread reaches the backend, ELEVATING while it blocks, the user's stop afterwards).
  `make winc-test` builds the core DLL, release and --test-hooks, with their gates.

## 12. The probe's timing points from the core (`w6/core-actionend`): API addition

- **`core_test_set_mark(c, mark, ctx)`** (CORE_TEST_HOOKS only, additive; main only). The TEST
  builds' probe gets the P3 timing points the core decides: the core calls `mark(ctx, name)` on
  main, inside a host callback, at each one. One point so far: `"trigger.actionEnd"`, when an
  action's busy state ends (busy(nil), `view_busy_off`), where today's `Controller.busy(nil)` marks
  it; G2's `P3.actionEnd` pairs it with the next `refresh.end` (tests/perf/stats.py). NULL: none.
- B's busy-marker removal is the flow's BUSY_MARKER step, off main, before EF_IDLE (flows.c `fx_idle`). A's `setBusyMarker(nil)` runs on main after its mark. So A's P3.actionEnd holds one unlink that B's does not (microseconds, B's real main-thread cost).
- When a flow shows a dialog right after busy(nil) (Report saved, Uninstall stopped, the Uninstall All confirmation after "Checking where…", Remove Anyway), B's commit is a later main-queue job, so B's P3.actionEnd and P3.longest.actionEnd may include the dialog's set-up. A's P3.longest can too (its turn end comes after refresh.end). This is G2.1's scheduling difference; P3.actionEnd is info only (the P3 gate is perf-activation's).
- **Not a `core_host` entry** (an appended `host->mark` was the first plan). core_host is frozen
  at 136 bytes, which the C# shell asserts (Native.cs `HostSize`), and core_new refuses a smaller
  table. A test-hooks entry would make the test-hooks DLL refuse W0's C# shell, and an
  entry in every build would change the release layout. A test-hooks setter keeps the layout,
  needs no native.c trampoline, and the release gate's `core_test_` check already covers it.
- **Release gate.** core/build.sh lib and dll (release) refuse an archive or DLL holding a
  core timing point's name (`trigger.actionEnd`; each new point is added to both gates).
  tests/harness/check-release.sh's probe strings include `trigger.actionEnd`.
- **Shell.** app/Shell's CoreShell sets it after core_new (`#if TEST_BUILD`). `coreMark` records
  `probeMark("trigger.actionEnd")`, and a test sees each name through `onMark`.
- **Tests.** view_test `t_action_end_mark`: one mark per action end, on main, inside a callback,
  none for a cancelled confirmation, none once the mark is reset to NULL. tests/mac/shell-test.swift:
  Help > Create Report... gives exactly one `trigger.actionEnd` through the shell.

## 13. The first release's menu rows (`w6/win-prepare-menus`): config addition

- **`"prepare_tabs"` in the start config** (additive: a new optional key, CORE_WIN_POLICY only,
  with `"os":"win"` only; the Mac build doesn't know it and a Windows core refuses it with
  `"os":"mac"`). A boolean: true when the shell shows the Status and Prepare tabs (the Prepare
  release, a TEST build's prepare scenarios). Absent or false (the first release, app.md §4.5,
  I10, I86): `core_menu_state` answers `CORE_MENU_HIDDEN` and not enabled for View > Status,
  View > Prepare, the Prepare menu and every row under it, so their shortcuts (Ctrl+1, Ctrl+2,
  Ctrl+R, Ctrl+.) and `{"ev":"menu"}` events do nothing ("a menu item that is off is ignored").
  The default is the safe one: a shell that doesn't send the key gets the first release's menus.
- **The rest of the way in (PM2, hardening).** One rule, `view_tabs_hidden(c)`, also keeps the
  core out of the hidden tabs by every other path: `{"ev":"tab"}` to Status or Prepare is ignored
  (logged), the hidden tabs' clicks (`tab_status`, `tab_prepare`, `prepare_playlist`,
  `check_again`, `pause`, `stop`) are off, and the start tab is Manage even with something
  installed. The first-release shell sends none of these; with `"prepare_tabs":true` and on the
  Mac nothing changes.
- **Mac-neutral by construction.** The field (`core.prepare_tabs`), menu_facts' `tabs_hidden`,
  the config key and the rule in `menu_rule` are all under `#ifdef CORE_WIN_POLICY`, which lib's
  Mac archive never defines; `make mac-unchanged` checks it.
- **Tests.** view_test `t_win_prepare_tabs`: each row's state with the key absent, false and true,
  and with the Mac's config in the same build; the menu events tab_prepare and tab_status move
  F_TAB only when shown; the refused forms (with "os":"mac", not a boolean). unit.c: the Mac build
  refuses the key. ShellTests `PrepareTabsMenus`: Host.WithPrepareTabs's config through the core.

## 14. Windows' unchecked ORT (`w6/host-facts-hf4-hf7`, HF7): additions

- **facts.def appends `FACT_LIB_UNCHECKED`** (flag, Windows only): the cheap sheet's BLOCKED "cache" rules
  had no answer for a supported rekordbox (the live onnxruntime.dll or `ort\`'s copy couldn't be looked
  at, or the pin had none). The sheet still writes FACT_REKORDBOX_SUPPORTED, and FACT_ROOT_ORT_EXISTS once
  read; the FACT_LIB_* rows stay unset.
- **pure.h appends `FEAT_NOT_CHECKED`** and `cache_blocked_win` takes it as a third argument
  (`lib_unchecked`, after the test-version rule): not FEAT_NO_ORT ("can't find it: reinstall rekordbox"),
  and kept out of `feature_why_checks_again`. Its line is texts.def's appended Windows row
  `T_WIN_CACHE_NOT_CHECKED`. The flows pass 0: their BLOCKED step fails on no answer (FEAT_CHECK_FAILED).
- Tests: pure.c (the rule, the line), view_test `t_win_lib_unchecked` (the status line and the install
  button), host_facts_test (the rows), native_facts_test (`has_root_ort` with the -1).

## 15. Windows' warm on activation (`w6/activation-warm`, HG3): additions

- **`core_win_cheap_facts` returns `int32_t`** (it returned void; the export's name is unchanged):
  1 the sheet carries FACT_LIB_UNCHECKED, 0 it doesn't, -1 nothing read (no REFRESH part, or a
  NULL argument). host_facts.c's `host_facts_write` returns the same 1 or 0.
- **events.def appends `facts_warm`** (Windows only, no members): one refresh (`view_refresh`), no
  re-check, no request. The WPF shell sends it when the warm it started on "activated" ends.
- **Shell.** Host.Send, after "activated": when the last sheet said unchecked and no warm runs (the
  start's included; when it was the start's, its end posts "facts_warm" once), `core_win_warm_facts`
  on a worker, then "facts_warm" posted to main. Pins stay off the UI thread (HF2).
- Tests: host_facts_test (the answer per case), view_test `t_win_facts_warm` (the line goes on the
  refresh; members refused), ShellTests `ActivationWarm` (no warm when checked or nothing read, one
  at a time, off main, one refresh after it, none after a failed warm or once stopped).

## 16. Windows' nameless drive texts (`w5/nameless-drive-texts`, NR2): additions

- **texts.def appends 16 Windows-only rows**, `T_WIN_LOC_CHECKING_NAMELESS` through
  `T_WIN_OLD_DRIVE_STAYS_NAMELESS`, after HF7's `T_WIN_CACHE_NOT_CHECKED` (no id moves; the range
  stays contiguous, which pure.c's loop over it relies on). Each is a `{drive}` text's form for a
  drive with no name (""), saying "Stems Cache's drive" or "its drive", with no `{drive}` slot.
- **pure.h adds `TB_NAMELESS(b, drive)`** (Windows and "" only) and **`nameless_text(id)`**, which
  maps a named text to its nameless row (both under CORE_WIN_POLICY). drive_text, location_light,
  paused_text and the not-connected uninstall, removal-note, switch and left-behind texts pick
  them; the Mac's texts are unchanged.
- A connected drive with neither a label nor a letter (folder-mounted, or its letter removed)
  also gives "". Its connected-only texts (Uninstall All, Clear, deleting, Settings' location
  row, the install note) have no nameless form yet: they are added before LIVE=1 (review NT1-NT5).
- Tests: pure.c (every row's slots), view_test `t_win_nameless_light` (each light and the paused
  tip), flows_win (the not-connected flow texts): no `""` in the output.

## 17. Windows' nameless connected-drive texts (`w5/nameless-texts-2`, NT1-NT5): additions

- **texts.def appends 12 Windows-only rows** after `T_WIN_OLD_DRIVE_STAYS_NAMELESS` (no id moves;
  `T_WIN_LOC_CHECKING_NAMELESS..T_WIN_REFUSE_NAMELESS` stays contiguous): the nameless forms of
  T_UNINSTALL_ALL_DRIVE, T_REMOVAL_NOTE_CONNECTED, T_DELETING_DRIVE(_N), T_LEFT_DRIVE_PARTLY,
  T_LEFT_DRIVE_NOT_NAMED, T_CLEAR_LOCATION_MSG, T_CLEARING_DRIVE(_N), T_NOT_CLEARED_DRIVE_MSG and
  T_LOCATION_LABEL (`T_WIN_LOCATION_LABEL_NAMELESS`, whose `{path}` is " › " and the folders below
  the root, "" at the root), all mapped in `nameless_text`; and `T_WIN_REFUSE_NAMELESS`.
- **pure.h adds `location_refuse_nameless(os, drive)`** (under CORE_WIN_POLICY): Windows and a
  drive named "" or NULL. change_location asks it for a probe that accepted the folder and for
  the probe's read-only (3) and file-system (7) refusals, which would name the drive (review N21),
  and refuses with `T_WIN_REFUSE_NAMELESS` (owner, 2026-11-15), so the Use, Found, Looking and
  Can't create texts need no nameless form. The Windows LOCATION_PROBE step doesn't exist yet.
  core.h's contract has it answer "" for a volume that was read and has no label and no letter
  (review N22).
- **CORE_PROGRESS_DELETE**: delete_texts and clear_location store the nameless ids in
  `del_text`/`del_text_n` when the drive is "", and the progress then passes only `{n}`.
- **view_loc_now gains `drive_set`, `kind` and `probe`** (under CORE_WIN_POLICY; -1 unset). On
  Windows the install note's paused line names the memo's drive only for LOC_CHOSEN with
  PROBE_AVAILABLE (native_location.c's `in_use`), "" then in its nameless form, else NULL ("your
  disk", also for LOC_BRIDGE_TOO_OLD with a named drive: NT5). A host's own snapshot (no memo)
  keeps "" as NULL.
- **Use This PC** passes the previous drive's "" through on Windows when FACT_SET_LOCATION is 1,
  so its nameless sentence is said (NT4).
- Tests: pure.c (the composers, the rule, every row's slots), flows_win `t_win_nameless_connected`
  (Uninstall All with the deletion's lines and what's left, Remove Anyway's note, Settings' row and
  Clear), `t_win_use_this_pc_nameless`, `t_win_refuse_nameless` (probe codes 0, 3, 7), view_test
  `t_win_install_note_drive`: no `""` in the output. The Mac's texts are unchanged.

## 18. Windows' kept-uninstall dialog (`w6/uninstall-kept-dialog`; owner, 2026-11-16): addition

- **texts.def appends one Windows-only row**, `T_WIN_UNINSTALL_KEPT`, after
  `T_WIN_REFUSE_NAMELESS` (no id moves; the nameless range stays contiguous). The after-exit
  (win/c/afterexit.c) shows it through `ax_os.kept_dialog`, only when it kept the app files and
  the Apps & features entry because a program still ran from the apps folder after the 60 s, or
  the process list stayed unknown. It names no program (the path is in the log). The Mac's texts
  are unchanged.
- **Before the dialog**, the after-exit registers the temp folder's RunOnce value
  (`ax_os.clean_at_logon`), so a restart with the dialog still open still cleans it up at the next
  logon; `clean_later` still runs once the dialog is closed.
