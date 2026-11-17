// The shell's Mac tests (win/shell/build.sh): the shell's WPF-free C# against the real core, built
// here as a dylib with CORE_WIN_POLICY and the test hooks. A fake window (FakeUi) stands in for
// WPF: its "dispatcher" is a queue this test's main thread pumps, and the core's main thread is
// this one (core_new runs here). Exit code: the number of failed checks.
//
//   dotnet ShellTests.dll path/to/librbsp-core.dylib
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using System.Runtime.InteropServices;
using System.Threading;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class FakeUi : IShellUi
    {
        public readonly BlockingCollection<Action> Queue = new BlockingCollection<Action>();
        public readonly ManageModel Model = new ManageModel();
        public readonly List<string> Lines = new List<string>();
        public readonly List<DialogForm> Dialogs = new List<DialogForm>();
        public int Renders, FullReloads, OffMain;
        public int AnswerButton = 1;
        /// set: ShowDialog signals Entered, then blocks until Gate is set (the drop test)
        public ManualResetEventSlim Gate, Entered;
        public int MainThread = Environment.CurrentManagedThreadId;

        public void PostDrain(Action drain) => Queue.Add(drain);

        public int ThrowRenders;   // the next renders that throw (a view failing)

        public void Render(IntPtr snap, IntPtr delta)
        {
            if (Environment.CurrentManagedThreadId != MainThread) OffMain++;
            Renders++;
            if (ThrowRenders > 0) { ThrowRenders--; throw new InvalidOperationException("a view failed"); }
            if (delta == IntPtr.Zero) FullReloads++;
            if (Snap.Has(Snap.Moved(delta), Section.S_APP)) Model.Apply(snap, delta == IntPtr.Zero);
        }

        public uint FrontFacts() => CoreApi.CORE_FRONT_ACTIVE | CoreApi.CORE_FRONT_WINDOW_LOADED | ((uint)CoreApi.CORE_TAB_MANAGE << CoreApi.CORE_FRONT_TAB_SHIFT);
        public bool ModalOpen() => false;

        public DialogAnswer ShowDialog(DialogForm form)
        {
            lock (Dialogs) Dialogs.Add(form);
            if (Gate != null) { Entered?.Set(); Gate.Wait(); }
            return new DialogAnswer { Button = AnswerButton };
        }

        public int PickFolder(string start, out string path) { path = null; return 1; }
        public void CloseReply(int what, bool proceed) { }
        public void Log(string line) { lock (Lines) Lines.Add(line); }

        public bool Logged(string s) { lock (Lines) return Lines.Exists(l => l.Contains(s)); }

        /// The main thread's loop: run what was posted until cond holds (or ms pass).
        public bool Pump(Func<bool> cond, int ms = 5000)
        {
            var sw = Stopwatch.StartNew();
            while (!cond())
            {
                if (sw.ElapsedMilliseconds > ms) return false;
                if (Queue.TryTake(out var a, 10)) a();
            }
            return true;
        }
    }

    /// The core DLL's update entries, faked: each answer set by the test, each call recorded.
    internal sealed class FakeStart : IStartNative
    {
        public int Wait = StartSequence.SwapReady, Failed, Os, At = StartSequence.UpdateFailNone;
        public bool StubOk = true;
        public readonly List<string> Calls = new List<string>();
        public int FailedBlockMs;     // SwapFailed blocks this long, then records "failed-end"
        public bool FailedNever;      // SwapFailed never returns
        public LogBuffer Buf;         // Log goes through a real LogBuffer (its lock)
        public void Add(string c) { lock (Calls) Calls.Add(c); }
        public int SwapWait(uint ms) { Add("wait " + ms); return Wait; }
        public int SwapFailed()
        {
            Add("failed");
            if (FailedNever) Thread.Sleep(Timeout.Infinite);
            if (FailedBlockMs > 0) { Thread.Sleep(FailedBlockMs); Add("failed-end"); }
            return Failed;
        }
        public int OsCheck() { Add("os"); return Os; }
        public int AtStart() { Add("at_start"); return At; }
        public bool StartStub() { Add("stub"); return StubOk; }
        public void WaitOldApp(uint ms) { Add("waitold " + ms); }
        public int Prelinks;
        public void Prelink() { Prelinks++; }
        public readonly List<string> Lines = new List<string>();
        public void Log(string line) { if (Buf != null) Buf.Add(line); lock (Lines) Lines.Add(line); }
        public string Seq { get { lock (Calls) return string.Join(",", Calls); } }
    }

    /// The close's side, faked: the core's answer, CriticalWork, and the threads run inline.
    /// one guard case's fake clock and kill count
    internal sealed class GuardCase { public long Now; public int Kills; }

    internal sealed class FakeClose : ICloseSide
    {
        public int Answer = CoreApi.CORE_CLOSE_NOW;
        public bool Running, Idle = true;
        public readonly List<string> Calls = new List<string>();
        public readonly Queue<Action> Off = new Queue<Action>(), Ui = new Queue<Action>();
        public int Ask(int what) { Calls.Add("ask " + what); return Answer; }
        public bool CriticalRunning() => Running;
        public long Now;
        public long NowMs() => Now;
        /// a wait that times out takes its whole time on the fake clock
        public bool CriticalWait(int ms) { ClosesAtWait = CriticalCloses; Calls.Add("wait " + ms); if (!Idle) Now += ms; return Idle; }
        /// how many admission closes there had been when the last wait began (-1: no wait yet)
        public int ClosesAtWait = -1;
        public void RunOff(Action a) => Off.Enqueue(a);
        public void Post(Action a) => Ui.Enqueue(a);
        /// CriticalWork's admission closed (counted apart from Calls, so the sequences stay); a
        /// close before it is counted too (WC1: every Approved closes admission first)
        public int CriticalCloses, ClosesBeforeCriticalClose;
        public void CriticalClose() => CriticalCloses++;
        public void Close() { if (CriticalCloses == 0) ClosesBeforeCriticalClose++; Calls.Add("close"); }
        public void BlockReason(bool on) => Calls.Add(on ? "reason on" : "reason off");
        public int Quits;
        public void Quitting() => Quits++;
        public void Log(string line) => Calls.Add("log");
        /// the worker's actions, then the UI's, until neither queues more (at most 50 rounds)
        public void Settle()
        {
            for (int round = 0; round < 50 && (Off.Count > 0 || Ui.Count > 0); round++)
            {
                while (Off.Count > 0) Off.Dequeue()();
                while (Ui.Count > 0) Ui.Dequeue()();
            }
        }
        public string Seq => string.Join(",", Calls);
    }

    internal static class Tests
    {
        private static int failures, passes;

        private static void Check(bool ok, string what)
        {
            if (ok) passes++; else failures++;
            Console.WriteLine((ok ? "PASS " : "FAIL ") + what);
        }

        // every test host's main-only entries called off main (SA2): the tests' own recorder, never a
        // throw (core.h: no exception escapes a native callback), checked once after every test;
        // MainOnly's deliberate calls go to its own recorder
        private static readonly List<string> offMainSeen = new List<string>();

        private static Host NewHost(FakeUi ui, bool shellWork = false)
        {
            var h = new Host(ui, shellWork);
            h.OffMain = w => { lock (offMainSeen) offMainSeen.Add(w); };
            return h;
        }

        private static int Main(string[] args)
        {
            string dylib = args.Length > 0 ? args[0] : "librbsp-core.dylib";
            NativeLibrary.SetDllImportResolver(typeof(Core).Assembly, (name, asm, path) =>
                name == "rbsp-core.dll" ? NativeLibrary.Load(dylib) : IntPtr.Zero);
            try
            {
                Ids();
                Pure();
                Swap();
                Close();
                QuitWindow();
                OneCopyTests();
                UninstallStart();
                PrepareTab();
                PrepareGuards();
                StatusTab();
                BigSheets();
                RowNames();
                BindingModes();
                UntilOnce();
                ProbeRecords();
                ProbeDriver();
                ProbeTurnsAccounting();
                PresenterOrder();
                StopRace();
                Commit();
                MainOnly();
                PrepareTabsMenus();
                ActivationWarm();
                ActivationWarmStartOverlap();
                Sink();
                Aggregator();
                MenuKeys();
                Place();
                LifeCycle();
                Check(offMainSeen.Count == 0, "no main-only host entry ran off main outside MainOnly (" + string.Join(",", offMainSeen) + ")");
            }
            catch (Exception e) { Check(false, "threw: " + e); }
            Console.WriteLine("shell tests: " + passes + " passed, " + failures + " failed");
            return failures;
        }

        // the generated ids (CoreIds.cs, gen_ids.c) are the core's: the hash over every row's
        // name, kind, table, OS tag and position, and the frozen layouts
        private static void Ids()
        {
            Check(Core.core_api_version() == CoreApi.Version, "core_api_version " + Core.core_api_version() + " is CoreIds.cs's " + CoreApi.Version);
            ulong h = Core.core_ids_hash();
            Check(h == CoreApi.IdsHash, "core_ids_hash 0x" + h.ToString("x16") + " is CoreIds.cs's 0x" + CoreApi.IdsHash.ToString("x16"));
            Check(Core.core_selftest() == 0, "core_selftest");
            Check(Marshal.SizeOf<CoreHost>() == Core.HostSize && Marshal.SizeOf<CoreMenuItem>() == Core.MenuItemSize
                  && Marshal.SizeOf<CoreSlot>() == Core.SlotSize && Marshal.SizeOf<CoreMaskCtx>() == Core.MaskCtxSize,
                  "the frozen layouts: core_host 136, core_menu_item 40, core_slot 24, core_mask_ctx 40");
            // bound by name: a few rows read through the generated names
            Check(Texts.Get(CoreText.T_APP_NAME) == "RB Stems Plus", "texts by name: T_APP_NAME");
            Check(Texts.Get(CoreText.T_BTN_INSTALL_PLUS) == "Install Demucs v4", "texts by name: T_BTN_INSTALL_PLUS");
            string s = Texts.Get(CoreText.T_ABOUT_GB, (Slot.gb, "1.5"));
            Check(s == " (about 1.5 GB)", "a text with a slot: T_ABOUT_GB {gb} (\"" + s + "\")");
            Check(Texts.Get(CoreText.T_ABOUT_GB) == "", "a text missing its slot is refused (\"\")");
            int n = Core.core_menu_count(CoreApi.CORE_OS_WIN), named = 0;
            for (int i = 0; i < n; i++)
                if (Core.core_menu_at(CoreApi.CORE_OS_WIN, i, out CoreMenuItem m) == CoreApi.CORE_OK && Enum.IsDefined(typeof(Menu), m.id)) named++;
            Check(n > 0 && named == n, "every Windows menu row has a generated name (" + named + " of " + n + ")");
        }

        private static void Pure()
        {
            Check(Json.Quote("a\"b\\c\nd\u0001é") == "\"a\\\"b\\\\c\\nd\\u0001é\"", "Json.Quote escapes quotes, backslashes and controls");
            Check(Json.Click("install_plus") == "{\"ev\":\"click\",\"id\":\"install_plus\"}", "Json.Click");

            var f = new DialogForm();
            f.Buttons.AddRange(new[] { "Continue", "Cancel", "Don't Ask Again…" });
            f.Left.AddRange(new[] { false, false, true });
            Check(string.Join(",", f.InlineOrder()) == "2,1,0", "an inline dialog: the left-placed button first, then the first button on the right");

            string dir = Path.Combine(Path.GetTempPath(), "rbsp-shell-test-" + Environment.ProcessId);
            string path = Path.Combine(dir, "Logs", "app.log");
            var log = new LogBuffer(path, () => new DateTime(2026, 11, 12, 10, 0, 5));
            bool first = log.Add("hello"), second = log.Add("world");
            Check(first && !second, "the log pane's flush is posted once per batch");
            Check(log.Take() == "10:00:05  hello\n10:00:05  world\n" && log.Take() == "", "the pane's lines with the time, taken once");
            Check(log.Add("again"), "a new batch posts its flush again");
            Check(File.ReadAllText(path) == "2026-11-12 10:00:05  hello\n2026-11-12 10:00:05  world\n2026-11-12 10:00:05  again\n",
                  "the log file's lines with the date (appendLog's form), its folder made");
            Directory.Delete(dir, true);
        }

        // a guard that never fires on its own (a clock standing at 0)
        private static SwapGuard G(IStartNative n) => new SwapGuard(n, () => { }, () => 0, 5);

        // the update's share of the start (Startup.cs): --swap's order, its failures, update_at_start
        private static void Swap()
        {
            int exit;
            bool sw;
            Func<string> fine = () => null;
            int shown = 0;
            Func<string> counted = () => { shown++; return null; };

            var n = new FakeStart();
            Check(StartSequence.Run(false, null, n, counted, G(n), out exit, out sw) == StartOutcome.Go && n.Seq == "at_start" && shown == 0,
                  "a normal start: update_at_start only, no off-screen window (" + n.Seq + ")");
            n = new FakeStart { At = StartSequence.UpdateBusy };
            Check(StartSequence.Run(false, null, n, fine, G(n), out exit, out sw) == StartOutcome.Exit && exit == 0, "an update still running (UPDATE_BUSY): exit 0, no UI");
            n = new FakeStart { At = 6 };
            Check(StartSequence.Run(false, null, n, fine, G(n), out exit, out sw) == StartOutcome.Go, "a failed update's key: the start goes on (UPDATE_RESULT says it)");
            n = new FakeStart();
            Check(StartSequence.Run(false, "the self-test failed", n, fine, G(n), out exit, out sw) == StartOutcome.Refuse && n.Calls.Count == 0,
                  "a core refused outside --swap: the refusal, nothing else called");

            n = new FakeStart();
            shown = 0;
            Check(StartSequence.Run(true, null, n, counted, G(n), out exit, out sw) == StartOutcome.Go && n.Seq == "wait 60000,os,at_start" && shown == 1,
                  "--swap: the wait (60 s), the minimum, the off-screen start, then update_at_start (" + n.Seq + ")");
            n = new FakeStart { Wait = StartSequence.SwapNone };
            shown = 0;
            Check(StartSequence.Run(true, null, n, counted, G(n), out exit, out sw) == StartOutcome.Go && n.Seq == "wait 60000,at_start" && shown == 0,
                  "--swap that no pending names: a normal start");
            n = new FakeStart { Wait = StartSequence.SwapOldRuns };
            Check(StartSequence.Run(true, null, n, counted, G(n), out exit, out sw) == StartOutcome.Exit && exit == 1 && n.Seq == "wait 60000,failed",
                  "--swap, the old app still running: failed swap, exit, no UI (" + n.Seq + ")");
            n = new FakeStart();
            Check(StartSequence.Run(true, "the self-test failed (3)", n, counted, G(n), out exit, out sw) == StartOutcome.Exit && n.Seq == "wait 60000,failed",
                  "--swap, core_selftest failed: failed swap after the wait, nothing more (" + n.Seq + ")");
            n = new FakeStart { Os = 1 };
            Check(StartSequence.Run(true, null, n, counted, G(n), out exit, out sw) == StartOutcome.Exit && n.Seq == "wait 60000,os,failed",
                  "--swap below the minimum Windows: failed swap");
            n = new FakeStart { Os = 2 };
            Check(StartSequence.Run(true, null, n, counted, G(n), out exit, out sw) == StartOutcome.Exit && n.Seq == "wait 60000,os,failed", "--swap below .NET 4.8.1: failed swap");
            n = new FakeStart();
            Check(StartSequence.Run(true, null, n, () => "XamlParseException", G(n), out exit, out sw) == StartOutcome.Exit && exit == 1 && n.Seq == "wait 60000,os,failed",
                  "--swap, the off-screen WPF start failed: failed swap, no UPDATE_RESULT kept here");
            n = new FakeStart { Os = -1 };
            Check(StartSequence.Run(true, null, n, fine, G(n), out exit, out sw) == StartOutcome.Go, "--swap, the minimum not askable: not a failure");
            n = new FakeStart { Failed = -1 };
            Check(StartSequence.Run(true, null, n, () => "x", G(n), out exit, out sw) == StartOutcome.Exit && n.Seq == "wait 60000,os,failed,stub",
                  "--swap, the result can't be written: the stub started by the shell");
            n = new FakeStart { Wait = -1 };
            Check(StartSequence.Run(true, "rbsp-core.dll can't be loaded (126)", n, counted, G(n), out exit, out sw) == StartOutcome.Exit && exit == 1 && n.Seq == "wait 60000,waitold 60000,stub",
                  "--swap without a usable core: the old app waited for, then the stub started by the shell, pending left for it (" + n.Seq + ")");
            n = new FakeStart { At = StartSequence.UpdateBusy };
            Check(StartSequence.Run(true, null, n, fine, G(n), out exit, out sw) == StartOutcome.Exit && exit == 0, "--swap whose checks passed, then UPDATE_BUSY: exit, no UI");
            // the swapping flag: only a READY --swap whose checks all passed
            n = new FakeStart();
            StartSequence.Run(true, null, n, fine, G(n), out exit, out sw);
            Check(sw, "swapping: a READY --swap whose checks passed");
            n = new FakeStart { Wait = StartSequence.SwapNone };
            StartSequence.Run(true, null, n, fine, G(n), out exit, out sw);
            Check(!sw, "not swapping: a --swap no pending names");
            n = new FakeStart();
            StartSequence.Run(false, null, n, fine, G(n), out exit, out sw);
            Check(!sw, "not swapping: a normal start");
            n = new FakeStart();
            StartSequence.Run(true, null, n, () => "x", G(n), out exit, out sw);
            Check(!sw, "not swapping: a failed check");
            n = new FakeStart { At = StartSequence.UpdateBusy };
            StartSequence.Run(true, null, n, fine, G(n), out exit, out sw);
            Check(!sw, "not swapping: UPDATE_BUSY");

            // Recover: the result and the restart; the shell's stub when the core can't
            n = new FakeStart();
            StartSequence.Recover(n);
            Check(n.Seq == "failed", "Recover: core_win_swap_failed (it restarts the old app)");
            n = new FakeStart { Failed = -1 };
            StartSequence.Recover(n);
            Check(n.Seq == "failed,stub", "Recover without the core's entry: the stub started by the shell");
            n = new FakeStart { Failed = 5 };
            StartSequence.Recover(n);
            Check(n.Seq == "failed" && n.Lines.Exists(l => l.Contains("(5)")), "Recover, the result or restart failing: logged");

            // update\pending as update.c writes it (WU9)
            var pr = PendingRecord.Parse("1.5.2\n1.5.3\n4242\n1794470400\n");
            Check(pr != null && pr.From == "1.5.2" && pr.To == "1.5.3" && pr.Pid == 4242 && pr.Started == 1794470400, "pending: parsed");
            Check(PendingRecord.Parse("1.5.2\n1.5.3\n4242\n1794470400") == null && PendingRecord.Parse("1.5.2\n1.5.3\n4242\n") == null
                  && PendingRecord.Parse("1.5\n1.5.3\n1\n1\n") == null && PendingRecord.Parse("1.5.2\n1.5.3\n-1\n1\n") == null
                  && PendingRecord.Parse("1.5.2\n1.5.3\n99999999999\n1\n") == null && PendingRecord.Parse("01.5.2\n1.5.3\n1\n1\n") == null
                  && PendingRecord.Parse("") == null && PendingRecord.Parse(null) == null, "pending: anything else refused");

            Guard();
            Check(StartSequence.MinimumWhy(0) == null && StartSequence.MinimumWhy(-1) == null && StartSequence.MinimumWhy(1) != null, "the minimum's reasons");
        }

        // the swap guard (WU2): a fake clock and a fake kill
        private static void Guard()
        {
            bool Until(Func<bool> c) { var t = Stopwatch.StartNew(); while (!c()) { if (t.ElapsedMilliseconds > 5000) return false; Thread.Sleep(1); } return true; }

            var c1 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            var n = new FakeStart();
            var g = new SwapGuard(n, () => Interlocked.Increment(ref c1.Kills), () => Interlocked.Read(ref c1.Now), 1);
            g.Arm(SwapGuard.StageMs);
            Check(n.Prelinks == 1, "arming binds the recovery's entry first");
            Interlocked.Exchange(ref c1.Now, SwapGuard.StageMs - 1);
            Thread.Sleep(30);
            Check(c1.Kills == 0 && !g.HasFired, "not before its time");
            Interlocked.Exchange(ref c1.Now, SwapGuard.StageMs);
            Check(Until(() => c1.Kills == 1), "at its time: fired and killed");
            Thread.Sleep(30);
            Check(c1.Kills == 1 && n.Seq == "failed", "once: the recovery (failed swap, the old app restarted), then one kill (" + n.Seq + ")");
            Check(!g.Finish(), "Finish after it fired: false (the caller does nothing more)");

            // a winning Finish: no kill, ever
            var c2 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart();
            g = new SwapGuard(n, () => Interlocked.Increment(ref c2.Kills), () => Interlocked.Read(ref c2.Now), 1);
            g.Arm(SwapGuard.StageMs);
            Check(g.Finish(), "Finish in time: true");
            Interlocked.Exchange(ref c2.Now, 10 * SwapGuard.StageMs);
            Thread.Sleep(30);
            Check(c2.Kills == 0 && n.Calls.Count == 0, "after a winning Finish: no recovery, no kill");

            // re-armed: the real start gets its own 30 s
            var c3 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart();
            g = new SwapGuard(n, () => Interlocked.Increment(ref c3.Kills), () => Interlocked.Read(ref c3.Now), 1);
            g.Arm(SwapGuard.StageMs);
            Interlocked.Exchange(ref c3.Now, SwapGuard.StageMs - 10);
            g.Arm(SwapGuard.StageMs);
            Interlocked.Exchange(ref c3.Now, SwapGuard.StageMs + 10);
            Thread.Sleep(30);
            Check(c3.Kills == 0 && n.Prelinks == 1, "re-armed: the deadline moves, one thread");

            // a hung off-screen start: the guard fires during it; the sequence then recovers nothing more
            var c4 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart();
            g = new SwapGuard(n, () => Interlocked.Increment(ref c4.Kills), () => Interlocked.Read(ref c4.Now), 1);
            Func<string> hang = () => { Interlocked.Exchange(ref c4.Now, SwapGuard.StageMs + SwapGuard.GraceMs); Until(() => Volatile.Read(ref c4.Kills) == 1); return "hung"; };
            var r = StartSequence.Run(true, null, n, hang, g, out int exit, out bool sw);
            Check(r == StartOutcome.Exit && c4.Kills >= 1 && n.Seq == "wait 60000,os,failed", "a hung off-screen start: the guard's one recovery, its kill, none by the sequence (" + n.Seq + ")");

            // WV3: the off-screen stage's guard waits its 30 s plus the grace
            var c5 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart();
            g = new SwapGuard(n, () => Interlocked.Increment(ref c5.Kills), () => Interlocked.Read(ref c5.Now), 1);
            Func<string> slow = () => { Interlocked.Exchange(ref c5.Now, SwapGuard.StageMs + 1); Thread.Sleep(30); return "the off-screen window drew no frame in 30 s"; };
            r = StartSequence.Run(true, null, n, slow, g, out exit, out sw);
            Check(r == StartOutcome.Exit && c5.Kills == 0 && n.Seq == "wait 60000,os,failed", "the off-screen start's own 30 s timeout wins over the guard's grace: a clean recovery, no kill (" + n.Seq + ")");

            // WV1: a recovery that blocks 300 ms, an off-screen failure after the deadline: Run returns after it and the kill
            var c6 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart { FailedBlockMs = 300 };
            FakeStart nn = n;
            g = new SwapGuard(n, () => { nn.Add("kill"); Interlocked.Increment(ref c6.Kills); }, () => Interlocked.Read(ref c6.Now), 1);
            Func<string> late = () => { Interlocked.Exchange(ref c6.Now, SwapGuard.StageMs + SwapGuard.GraceMs); Until(() => g.HasFired); return "late"; };
            r = StartSequence.Run(true, null, n, late, g, out exit, out sw);
            string seq = n.Seq;
            Check(r == StartOutcome.Exit && seq.StartsWith("wait 60000,os,failed,failed-end,kill"), "a fired guard: Run returns only after the recovery's end and the kill (" + seq + ")");

            // WV2: the log's lock held: the kill still comes, within the recovery's bound
            var c7 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart { Buf = new LogBuffer(null) };
            g = new SwapGuard(n, () => Interlocked.Increment(ref c7.Kills), () => Interlocked.Read(ref c7.Now), 1, 300);
            g.Arm(SwapGuard.StageMs);
            object gate = typeof(LogBuffer).GetField("gate", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(n.Buf);
            var held = Stopwatch.StartNew();
            lock (gate)
            {
                Interlocked.Exchange(ref c7.Now, SwapGuard.StageMs);
                Check(Until(() => Volatile.Read(ref c7.Kills) == 1) && held.ElapsedMilliseconds < 3000, "the log's lock held by another thread: killed within the recovery's bound (" + held.ElapsedMilliseconds + " ms)");
            }

            // a SwapFailed that never returns: the kill after the bound
            var c8 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart { FailedNever = true };
            g = new SwapGuard(n, () => Interlocked.Increment(ref c8.Kills), () => Interlocked.Read(ref c8.Now), 1, 300);
            g.Arm(SwapGuard.StageMs);
            var t0 = Stopwatch.StartNew();
            Interlocked.Exchange(ref c8.Now, SwapGuard.StageMs);
            Check(Until(() => Volatile.Read(ref c8.Kills) == 1) && t0.ElapsedMilliseconds >= 250, "a recovery that never returns: killed after its bound (" + t0.ElapsedMilliseconds + " ms)");

            // Settle without a fire: nothing
            var c9 = new GuardCase();   // its own clock and kills: an earlier case's guard thread never touches them (WC11)
            n = new FakeStart();
            g = new SwapGuard(n, () => Interlocked.Increment(ref c9.Kills), () => 0, 1);
            g.Settle();
            g.Arm(SwapGuard.StageMs);
            g.Settle();
            Check(g.Finish() && c9.Kills == 0, "Settle unfired, armed or not: returns, no kill");
            g.Settle();
            Check(c9.Kills == 0, "Settle after Finish: no kill");
        }

        // the flow-based close, the flow's quit, the logoff (CloseGate.cs)
        private static void Close()
        {
            var f = new FakeClose();
            var g = new CloseGate(f);
            Check(!g.OnClosing() && f.Seq == "ask " + CoreApi.CORE_CLOSE_WINDOW && g.Closing, "Closing: cancelled, the core asked once; NOW decides the close");
            Check(!g.OnClosing() && f.Calls.Count == 1, "a second Closing while the close waits: cancelled, not asked again");
            f.Settle();
            Check(f.Seq == "ask 0,wait 60000,close", "CriticalWork waited for off the UI thread (60 s), then the window closed (" + f.Seq + ")");
            Check(g.OnClosing(), "the Closing that follows lets the window go");

            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_LATER };
            g = new CloseGate(f);
            Check(!g.OnClosing() && !g.Closing, "LATER: cancelled, nothing decided");
            g.OnReply(CoreApi.CORE_CLOSE_WINDOW, false);
            f.Settle();
            Check(!g.Closing && !f.Seq.Contains("close"), "close_reply(0): the window stays");
            g.OnReply(CoreApi.CORE_CLOSE_QUIT, true);
            f.Settle();
            Check(f.Seq.EndsWith("wait 60000,close") && g.OnClosing(), "close_reply(QUIT, 1): the wait, then the close");
            g.OnReply(CoreApi.CORE_CLOSE_QUIT, true);
            f.Settle();
            Check(f.Calls.FindAll(c => c == "close").Count == 1, "a second reply closes nothing twice");

            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_REFUSE };
            g = new CloseGate(f);
            Check(!g.OnClosing() && !g.Closing && f.Off.Count == 0, "REFUSE: the window stays");
            g.OnReply(CoreApi.CORE_CLOSE_WATCHER_DONE, true);
            Check(!g.Closing, "a reply for the watcher is no close of the window");

            f = new FakeClose { Idle = false };
            g = new CloseGate(f);
            g.QuitNow();
            f.Settle();
            Check(f.Seq == "wait 60000,log,close" && g.OnClosing(), "the flow's quit (UPDATE_SWAP): no question, the bounded wait, logged when it ran out, closed");

            // the logoff, in WPF's order: SessionEnding, then (no veto) Closing and Exit, or (a veto)
            // the window stays and WM_ENDSESSION comes on it
            f = new FakeClose();
            g = new CloseGate(f);
            Check(!g.OnQueryEndSession() && g.SessionEnding && f.Calls.Count == 0, "SessionEnding with nothing running: no veto, no reason");
            Check(g.OnClosing() && f.Calls.Count == 0, "WPF's shutdown closes the window: no question to the core");
            g.EndWait();
            Check(f.Seq == "reason off", "Exit: nothing running, no wait (" + f.Seq + ")");

            f = new FakeClose();
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnClosing();
            f.Running = true;   // a step started after the query
            g.EndWait();
            g.EndWait();
            Check(f.Seq == "wait " + CloseGate.LogoffWaitMs + ",reason off", "Exit: a step that started after the query, one bounded wait (" + f.Seq + ")");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            Check(g.OnQueryEndSession() && f.Seq == "reason on", "SessionEnding while a step must finish: veto, the reason set");
            g.OnEndSession(true);
            g.OnEndSession(true);
            Check(f.Seq == "reason on,wait " + CloseGate.LogoffWaitMs + ",reason off", "WM_ENDSESSION(TRUE) after the veto: one bounded wait, then the reason goes (" + f.Seq + ")");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnEndSession(false);
            Check(!g.SessionEnding && f.Seq == "reason on,reason off", "WM_ENDSESSION(FALSE) after the veto: the reason goes, the app stays");
            f.Running = false;
            Check(!g.OnClosing() && f.Seq.EndsWith("ask 0"), "after a cancelled logoff the close asks the core again");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.EndWait();
            Check(f.Calls.Count == 0, "the app's exit without a session ending: no wait");

            // WV4: after a veto the app quits once the step ends, so the logoff goes on; a
            // cancelled logoff's quit is dropped
            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            Check(g.OnQueryEndSession() && f.Off.Count == 1, "a veto: the step's end is waited for off the UI thread");
            f.Running = false;
            f.Settle();
            Check(f.Seq == "reason on,wait " + CloseGate.LogoffWaitMs + ",reason off,close", "veto, then the step ends: the reason goes, then the close (" + f.Seq + ")");
            Check(g.OnClosing() && f.Calls.FindAll(c => c.StartsWith("ask")).Count == 0, "that close goes through, the core not asked");
            g.OnEndSession(true);
            g.EndWait();
            Check(f.Seq == "reason on,wait " + CloseGate.LogoffWaitMs + ",reason off,close,reason off", "WM_ENDSESSION(TRUE) and Exit after it: nothing running, no second wait (" + f.Seq + ")");

            f = new FakeClose { Running = true, Idle = false };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            f.Settle();
            Check(f.Seq == "reason on,wait " + CloseGate.LogoffWaitMs + ",log,reason off,close", "veto, the step still runs after 30 s: logged, then quit anyway (" + f.Seq + ")");
            g.EndWait();
            Check(f.Seq == "reason on,wait " + CloseGate.LogoffWaitMs + ",log,reason off,close,reason off", "a first wait that timed out: Exit doesn't wait again (" + f.Seq + ")");

            // WC2: the wait sees the step end, then another begins before the close is posted: no
            // close while it runs; a second wait for no more than the time left; then the close
            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            f.Off.Dequeue()();        // the wait returns idle (the step ended); its Post is queued
            f.Now = 10000;            // 10 s later another step begins before the Post runs
            f.Ui.Dequeue()();         // the Post: a step runs, before the deadline: waits again
            Check(!f.Seq.Contains("close") && f.Off.Count == 1, "a step begun before the close: no close while it runs (" + f.Seq + ")");
            f.Running = false;
            f.Settle();
            Check(f.Seq == "reason on,wait 30000,wait 20000,reason off,close", "the second wait no longer than the time left, then the close (" + f.Seq + ")");

            // the close while idle, then a step begins: Exit waits only what is left of the 30 s
            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            f.Running = false;
            f.Settle();
            g.OnClosing();
            f.Now = 5000;
            f.Running = true;
            g.EndWait();
            Check(f.Seq == "reason on,wait 30000,reason off,close,wait 25000,reason off", "a step begun after the veto's close: Exit waits the time left only (" + f.Seq + ")");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnEndSession(false);
            f.Settle();
            Check(f.Seq == "reason on,reason off,wait " + CloseGate.LogoffWaitMs && !g.Closing, "veto, ENDSESSION(FALSE), then the step ends: no close (" + f.Seq + ")");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnEndSession(false);
            f.Running = false;
            Check(!g.OnQueryEndSession() && g.SessionEnding, "a second query with nothing running: no veto");
            f.Settle();
            Check(f.Seq == "reason on,reason off,wait " + CloseGate.LogoffWaitMs && !g.Closing, "veto, cancel, a second query: the first veto's quit is dropped (" + f.Seq + ")");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnQueryEndSession();
            Check(f.Off.Count == 2, "two vetoes, two waits");
            f.Running = false;
            f.Settle();
            Check(f.Calls.FindAll(c => c == "close").Count == 1, "only the latest veto's quit closes (" + f.Seq + ")");
        }

        // a decided quit takes the one-copy window down at once (CloseGate's Quitting)
        private static void QuitWindow()
        {
            var f = new FakeClose();
            var g = new CloseGate(f);
            g.OnClosing();
            Check(f.Quits == 1 && f.Off.Count == 1, "NOW: the window goes as the quit is decided, before the wait");
            f.Settle();
            g.OnClosing();
            Check(f.Quits == 1, "the close that follows: once");

            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_LATER };
            g = new CloseGate(f);
            g.OnClosing();
            g.OnReply(CoreApi.CORE_CLOSE_WINDOW, false);
            Check(f.Quits == 0, "LATER, then a reply of 0: the window stays");
            g.OnReply(CoreApi.CORE_CLOSE_WINDOW, true);
            Check(f.Quits == 1, "a reply of 1: it goes");

            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_REFUSE };
            g = new CloseGate(f);
            g.OnClosing();
            Check(f.Quits == 0, "REFUSE: it stays");

            f = new FakeClose();
            g = new CloseGate(f);
            g.QuitNow();
            Check(f.Quits == 1, "the flow's quit: it goes");

            f = new FakeClose();
            g = new CloseGate(f);
            g.OnQueryEndSession();
            Check(f.Quits == 0, "a query with no veto: not yet");
            g.OnClosing();
            Check(f.Quits == 1, "WPF's shutdown closes the window: it goes");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            Check(f.Quits == 0, "a veto: it stays while the step runs");
            f.Running = false;
            f.Settle();
            Check(f.Quits == 1 && f.Seq.EndsWith("close"), "the veto's quit: it goes before the close");

            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnEndSession(false);
            f.Settle();
            Check(f.Quits == 0, "a cancelled logoff: it stays");
            Check(f.CriticalCloses == 0, "a cancelled logoff: admission stays open");

            // CriticalWork's admission (WC1, MC2): closed as soon as the quit is decided, so the wait
            // sees every step, and before every Approved
            f = new FakeClose();
            g = new CloseGate(f);
            g.OnClosing();
            Check(f.CriticalCloses == 1, "NOW: admission closed at once, before the wait");
            f.Settle();
            Check(f.ClosesAtWait == 1 && f.CriticalCloses == 1 && f.ClosesBeforeCriticalClose == 0,
                  "NOW: closed before the wait, still closed between the idle return and the close");
            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_LATER };
            g = new CloseGate(f);
            g.OnClosing();
            Check(f.CriticalCloses == 0, "LATER: admission open until the reply");
            g.OnReply(CoreApi.CORE_CLOSE_WINDOW, true);
            Check(f.CriticalCloses == 1, "close_reply(proceed): admission closed at once");
            f.Settle();
            Check(f.ClosesAtWait == 1 && f.ClosesBeforeCriticalClose == 0, "close_reply(proceed): closed before the wait and the close");
            f = new FakeClose();
            g = new CloseGate(f);
            g.QuitNow();
            Check(f.CriticalCloses == 1, "the flow's quit: admission closed at once");
            f.Settle();
            Check(f.ClosesAtWait == 1 && f.CriticalCloses == 1 && f.ClosesBeforeCriticalClose == 0, "the flow's quit: closed before the wait and the close");
            // MC1: WM_ENDSESSION(TRUE) is final: admission closed before its wait
            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            Check(f.CriticalCloses == 0, "a veto: admission open (the logoff can still be cancelled)");
            g.OnEndSession(true);
            Check(f.CriticalCloses >= 1 && f.ClosesAtWait >= 1, "veto, then WM_ENDSESSION(TRUE): admission closed before the wait returns");
            f = new FakeClose();
            g = new CloseGate(f);
            g.OnQueryEndSession();
            g.OnEndSession(true);
            Check(f.CriticalCloses == 1, "no veto, WM_ENDSESSION(TRUE) while idle: admission closed");
            f = new FakeClose { Running = true };
            g = new CloseGate(f);
            g.OnQueryEndSession();
            f.Running = false;
            f.Settle();
            Check(f.CriticalCloses == 1 && f.ClosesBeforeCriticalClose == 0, "the veto's quit: admission closed before the close");
            f = new FakeClose();
            g = new CloseGate(f);
            g.OnQueryEndSession();
            Check(g.OnClosing() && f.CriticalCloses == 1, "WPF's shutdown at the session's end: admission closed");
            f = new FakeClose { Answer = CoreApi.CORE_CLOSE_REFUSE };
            g = new CloseGate(f);
            g.OnClosing();
            f.Settle();
            Check(f.CriticalCloses == 0, "REFUSE: admission stays open");
        }

        // one copy at a time (OneCopy.cs over core_win_one_*)
        private sealed class FakeOne : IOneCopyNative
        {
            public int Answer, WindowErr;
            public readonly List<string> Calls = new List<string>();
            public Action Signal;
            public int Take(uint ms, out IntPtr one)
            {
                Calls.Add("take " + ms);
                one = Answer == OneCopy.Held ? (IntPtr)42 : IntPtr.Zero;
                return Answer;
            }
            public int Window(IntPtr one, Action signal) { Calls.Add("window " + one); Signal = signal; return WindowErr; }
            public void WindowClose(IntPtr one) => Calls.Add("window close " + one);
            public void Close(IntPtr one) => Calls.Add("close " + one);
            public string Seq => string.Join(",", Calls);
        }

        // --uninstall (LaunchArgs.cs): the Apps & features entry's start, through the stub
        private static void UninstallStart()
        {
            Check(LaunchArgs.IsUninstall(new[] { "--uninstall" }), "--uninstall alone: the complete uninstall");
            Check(!LaunchArgs.IsUninstall(new string[0]) && !LaunchArgs.IsUninstall(null) && !LaunchArgs.IsUninstall(new[] { "--swap" })
                  && !LaunchArgs.IsUninstall(new[] { "--uninstall", "--swap" }) && !LaunchArgs.IsUninstall(new[] { "--UNINSTALL" })
                  && !LaunchArgs.IsUninstall(new[] { "--uninstallx" }),
                  "anything else: another start");
            Check(LaunchArgs.ForUninstall(OneCopyStart.ExitQuietly) == OneCopyStart.AlreadyOpen
                  && LaunchArgs.ForUninstall(OneCopyStart.AlreadyOpen) == OneCopyStart.AlreadyOpen
                  && LaunchArgs.ForUninstall(OneCopyStart.Run) == OneCopyStart.Run && LaunchArgs.ForUninstall(OneCopyStart.CantOpen) == OneCopyStart.CantOpen,
                  "another copy open: it comes forward and this start says Already open; never a second uninstall");
            var sent = new List<string>();
            var lines = new List<string>();
            var u = new UninstallOnce(true, j => { sent.Add(j); return CoreApi.CORE_OK; }, lines.Add);
            Check(u.Shown() && !u.Shown() && !u.Shown() && sent.Count == 1 && sent[0] == "{\"ev\":\"click\",\"id\":\"uninstall_from_settings\"}"
                  && lines.Count == 1,
                  "the click uninstall_from_settings (flows_win.c: held until the start-up flow is done), exactly once, at the first shown ("
                  + string.Join(" ", sent) + ")");
            var none = new UninstallOnce(false, j => { sent.Add(j); return CoreApi.CORE_OK; }, lines.Add);
            Check(!none.Shown() && sent.Count == 1, "a normal start sends nothing");

            // the quit's kind (UN5): the uninstall's (ctx 1) leaves its deleted folder deleted
            string dir = Path.Combine(Path.GetTempPath(), "rbsp-shell-quit-" + Environment.ProcessId);
            string path = Path.Combine(dir, "Logs", "app.log");
            var buf = new LogBuffer(path, () => new DateTime(2026, 11, 13, 10, 0, 0));
            var kind = new QuitKind(buf);
            kind.Quit(IntPtr.Zero);
            buf.Add("before");
            Check(kind.SavePlace && File.Exists(path), "an update's quit (ctx 0): the window place kept, the log file written");
            Directory.Delete(dir, true);   // REMOVE_OWN_FOLDERS
            kind.Quit(QuitKind.Uninstall);
            buf.Add("after the uninstall");
            Check(!kind.SavePlace && !Directory.Exists(dir), "the uninstall's quit (ctx 1): no window place, no log line makes the folder again");
            Check(buf.Take().Contains("after the uninstall"), "the pane still gets the line");
            kind.Quit(IntPtr.Zero);
            buf.Add("later");
            Check(!kind.SavePlace && !Directory.Exists(dir), "for good: a later quit doesn't turn it back");
        }

        private static void OneCopyTests()
        {
            var lines = new List<string>();
            var n = new FakeOne();
            var o = new OneCopy(n, lines.Add);
            Check(o.Take() == OneCopyStart.Run && n.Seq == "take " + OneCopy.WaitMs, "HELD: the app runs (waiting 10 s for a copy that is quitting)");
            int signals = 0;
            o.Window(() => signals++);
            o.Window(() => signals++);
            Check(n.Seq.EndsWith("window 42") && n.Calls.Count == 2, "the window, once, on the mutex taken");
            n.Signal();
            Check(signals == 1, "a second start's signal reaches the shell");
            o.Quitting();
            o.Quitting();
            o.Close();
            o.Close();
            Check(n.Seq == "take 10000,window 42,window close 42,close 42", "quitting: the window goes once; then the mutex once (" + n.Seq + ")");

            n = new FakeOne();
            o = new OneCopy(n, lines.Add);
            o.Take();
            o.Close();
            Check(n.Seq == "take 10000,close 42", "no window made: the close alone");

            n = new FakeOne { WindowErr = 5 };
            o = new OneCopy(n, lines.Add);
            lines.Clear();
            o.Take();
            o.Window(() => { });
            o.Quitting();
            o.Close();
            Check(n.Seq == "take 10000,window 42,close 42" && lines.Count == 1, "a window that can't be made: logged, the app runs, nothing to take down (" + n.Seq + ")");

            foreach (var (answer, start, what) in new[] {
                (OneCopy.Handed, OneCopyStart.ExitQuietly, "HANDED: exit quietly"),
                (OneCopy.Busy, OneCopyStart.AlreadyOpen, "BUSY: Already open"),
                (OneCopy.Failed, OneCopyStart.CantOpen, "FAILED: can't open"),
                (7, OneCopyStart.CantOpen, "an unknown answer: can't open") })
            {
                n = new FakeOne { Answer = answer };
                o = new OneCopy(n, lines.Add);
                bool ok = o.Take() == start;
                o.Window(() => { });
                o.Quitting();
                o.Close();
                Check(ok && n.Seq == "take 10000", what + ", and nothing more is called (" + n.Seq + ")");
            }
        }

        // the probe's records (ProbeCore.cs) in Probe.swift's formats: stats.py and trace.py read both
        private static void ProbeRecords()
        {
            var conf = ProbeConf.Parse("mode = perf\nout= C:\\probe out \nhelper=x=y\n# a comment\n");
            Check(ProbeConf.Mode(conf) == ProbeMode.Perf && conf["out"] == "C:\\probe out" && conf["helper"] == "x=y", "probe.conf: key=value lines, trimmed, the first '=' splits");
            Check(ProbeConf.Mode(ProbeConf.Parse("mode=perf\n")) == ProbeMode.Off && ProbeConf.Mode(ProbeConf.Parse("mode=fast\nout=x\n")) == ProbeMode.Off
                  && ProbeConf.Mode(ProbeConf.Parse("mode=perf-min\nout=x")) == ProbeMode.PerfMin && ProbeConf.Mode(ProbeConf.Parse("mode=trace\nout=x")) == ProbeMode.Trace,
                  "no out, or an unknown mode: off; perf-min and trace read");

            long t = 0;
            var lines = new List<string>();
            var p = new ProbeRecorder(ProbeMode.Perf, () => t, lines.Add);
            p.Start(7, -5000, "arm64");
            t = 10; p.Mark("trigger.activate");
            t = 20; p.Field("c.plusIcon", "setImage", "0");
            t = 21; p.Field("c.plusIcon", "setImage", "0");
            t = 22; p.Field("c.buttons", "setEnabled");
            t = 23; p.Field("c.status", "setStringValue");
            t = 30; p.Turn(1234);
            t = 40; p.Event("windowDrawn");
            t = 41; p.Dialog(true, "x");
            t = 42; p.Command("click", "c.installPlusBtn");
            string tsv = p.PerfTsv(7, -5000, "arm64");
            Check(lines.Count == 1 && lines[0].Contains("\"mode\":\"perf\"") && lines[0].Contains("\"launch_ns\":-5000"), "perf mode: only the start line goes to the trace file");
            Check(tsv.StartsWith("# mode=perf pid=7 launch_ns=-5000 probe_busy_ns=0 wait_ns=0 arch=arm64") &&
                  tsv.Contains("\n10\ttp\ttrigger.activate\t0\n20\tfield\tc.plusIcon:setImage\t0\n23\tfield\tc.status:setStringValue\t0\n30\tturn\tturn\t1234\n40\tev\twindowDrawn\t0\n41\tev\tdialog\t1\n42\tcmd\tclick c.installPlusBtn\t0\n"),
                  "perf-<pid>.tsv: Probe.swift's header and rows; the same look twice is one change, a field off the short list isn't recorded (" + tsv.Replace("\n", "|") + ")");
            Check(p.LastChange == 23 && p.Count("trigger.activate") == 1 && p.Count("windowDrawn") == 1 && p.Count("dialog") == 1 && p.Count("cmd") == 1,
                  "counts for the driver's waits; a field set is the quiet clock's last change");

            // every event the shell sends the core, recorded in trace and perf mode, counted in all
            var sl = new List<string>();
            var st = new ProbeRecorder(ProbeMode.Trace, () => t, sl.Add);
            t = 50; st.Sent("{\"ev\":\"tab\",\"tab\":1}");
            t = 51; st.Sent("{\"ev\":\"until\",\"date\":\"2026-12-01\"}");
            t = 52; st.Sent("{\"ev\":\"until\",\"date\":\"2026-12-02\"}", 49);
            Check(sl.Count == 3 && sl[0] == "{\"k\":\"ev\",\"t\":50,\"e\":\"sent\",\"v\":\"{\\\"ev\\\":\\\"tab\\\",\\\"tab\\\":1}\"}" && sl[2].Contains("\"t\":49,"),
                  "trace: ev sent with the event's JSON, at the send's time (" + string.Join(" | ", sl) + ")");
            Check(st.Count("sent") == 3 && st.Count("sent:tab") == 1 && st.Count("sent:until") == 2 && st.Count("sent:folder") == 0,
                  "counted as sent and sent:<ev> (the count verb's)");
            var spl = new List<string>();
            var sp = new ProbeRecorder(ProbeMode.Perf, () => t, spl.Add);
            t = 60; sp.Sent("{\"ev\":\"folder\",\"gen\":3,\"id\":\"a\tb\"}");
            t = 61; sp.Sent("{\"ev\":\"click\",\"id\":\"prepare\"}");
            string spt = sp.PerfTsv(1, 0, "arm64");
            Check(spt.Contains("\n60\tsent\t{\"ev\":\"folder\",\"gen\":3,\"id\":\"a b\"}\t0\n61\tsent\t{\"ev\":\"click\",\"id\":\"prepare\"}\t0\n") && sp.Count("sent:folder") == 1,
                  "perf: a sent row named by the JSON, made one line (" + spt.Replace("\n", "|") + ")");
            var sml = new List<string>();
            var sm = new ProbeRecorder(ProbeMode.PerfMin, () => t, sml.Add);
            sm.Sent("{\"ev\":\"select\",\"gen\":1,\"id\":\"x\"}");
            Check(sm.PerfTsv(1, 0, "arm64") == null && sm.Count("sent:select") == 1, "perf-min: counted, no row");
            Check(spl.Count == 0 && sml.Count == 0, "perf modes: no jsonl line for a sent event (its JSON may hold the user's paths)");
            Check(ProbeRecorder.SentName("{\"ev\":\"activated\"}") == "activated" && ProbeRecorder.SentName("{\"x\":1}") == "?"
                  && ProbeRecorder.SentName("{\"ev\":\"\"}") == "?" && ProbeRecorder.SentName(null) == "?" && ProbeRecorder.SentName("{\"ev\":\"cut") == "?",
                  "the event's name: its leading ev, else ?");

            var m = new ProbeRecorder(ProbeMode.PerfMin, () => t, lines.Add);
            m.Event("didResignActive");
            m.Event("didBecomeActive");
            Check(m.PerfTsv(1, 0, "x64").Contains("\tev\tdidBecomeActive\t") && !m.PerfTsv(1, 0, "x64").Contains("didResignActive"), "perf-min: the metrics' events only");
            Check(new ProbeRecorder(ProbeMode.Perf, () => t, lines.Add).PerfTsv(1, 0, "x64") == null, "nothing recorded: no file");

            lines.Clear();
            var tr = new ProbeRecorder(ProbeMode.Trace, () => t, lines.Add);
            var s = new Dictionary<string, string> { ["c.status.str"] = "Checking…", ["c.spinner.anim"] = "1", ["win.vis"] = "1" };
            t = 98; tr.Sample(new Dictionary<string, string>(s));
            t = 100; tr.Turn(5);
            t = 114; tr.Sample(new Dictionary<string, string>(s));
            t = 116; tr.Turn(6);
            s["c.status.str"] = "Ready \"now\""; s.Remove("c.spinner.anim");
            t = 120; tr.Turn(1);   // a turn without a sample: no frame
            t = 128; tr.Sample(new Dictionary<string, string> { ["c.status.str"] = "gone by the next sample" });
            t = 130; tr.Sample(new Dictionary<string, string>(s));
            t = 133; tr.Turn(7);
            t = 140; tr.Mark("refresh.end");
            tr.Field("c.status", "setStringValue");
            Check(lines.Count == 3 && lines[0] == "{\"k\":\"frame\",\"t\":100,\"n\":1,\"busy\":5,\"d\":{\"c.spinner.anim\":\"1\",\"c.status.str\":\"Checking…\",\"win.vis\":\"1\"}}"
                  && lines[1] == "{\"k\":\"frame\",\"t\":133,\"n\":4,\"busy\":7,\"d\":{\"c.spinner.anim\":null,\"c.status.str\":\"Ready \\\"now\\\"\"}}"
                  && lines[2] == "{\"k\":\"ev\",\"t\":140,\"e\":\"tp\",\"v\":\"refresh.end\"}",
                  "trace mode: a frame per sampled turn that changed, at its end (keys sorted, a key gone as null; the turn's last sample), none for an unchanged or unsampled turn; no field events (" + string.Join(" | ", lines) + ")");
            Check(tr.Last("c.status.str") == "Ready \"now\"" && tr.Last("c.spinner.anim") == "" && tr.LastChange == 133, "the last frame's values, for the waits");

            var taken = new HashSet<string>();
            string a = ProbeIds.Menu("menu", "_File", taken); taken.Add(a);
            string b = ProbeIds.Menu(a, "E_xit", taken); taken.Add(b);
            string c = ProbeIds.Menu(a, "Exit", taken); taken.Add(c);
            string d = ProbeIds.Menu("menu", "Rock__n.Roll", taken);
            Check(a == "menu.File" && b == "menu.File.Exit" && c == "menu.File.Exit#2" && d == "menu.Rock_n_Roll", "menu ids: the title path without access keys, '.' as '_', a second of a name #2 (" + a + ", " + b + ", " + c + ", " + d + ")");
            var fieldsTsv = File.ReadAllLines(Path.Combine(AppContext.BaseDirectory, "fields.tsv"));
            var macIds = new HashSet<string>(fieldsTsv.Select(l => l.Split('\t')[0]));
            // both ways against tests/harness/scenarios-win.tsv's always-per-OS lines: every Windows
            // id is the Mac's or listed there, every Mac id is mapped or listed there
            var winTsv = File.ReadAllLines(Path.Combine(AppContext.BaseDirectory, "scenarios-win.tsv"));
            List<string> Always(string rows) => winTsv.Where(l => l.StartsWith("# always per-OS (" + rows + "): ", StringComparison.Ordinal))
                .SelectMany(l => l.Substring(l.IndexOf("): ", StringComparison.Ordinal) + 3).Split(',')).ToList();
            bool Listed(string id, List<string> pats) => pats.Any(p => p.EndsWith(".*", StringComparison.Ordinal) ? id.StartsWith(p.Substring(0, p.Length - 1), StringComparison.Ordinal) : id == p);
            List<string> everyRow = Always("every row"), firstRows = Always("first rows");
            var mapped = new HashSet<string>(ProbeIds.Mapped);
            var winOnly = mapped.Where(id => !macIds.Contains(id) && !Listed(id, everyRow)).ToList();
            var macOnly = macIds.Where(id => !mapped.Contains(id) && !Listed(id, everyRow) && !Listed(id, firstRows)).ToList();
            Check(everyRow.Contains("c.notice") && firstRows.Contains("prepare.*") && winOnly.Count == 0 && macOnly.Count == 0,
                  "every Windows field id is tests/harness/fields.tsv's or always per-OS, and every Mac one mapped or always per-OS (" + string.Join(", ", winOnly.Concat(macOnly)) + ")");
            // the "run" scenarios' c.* fields all exist on Windows (SP7: c.updateItem)
            var unmapped = new List<string>();
            foreach (var row in winTsv.Where(l => !l.StartsWith("#", StringComparison.Ordinal)).Select(l => l.Split('\t')).Where(r => r.Length > 2 && r[2] == "run"))
                foreach (System.Text.RegularExpressions.Match used in System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "scenarios", row[0] + ".sh")), @"\bc\.[A-Za-z0-9]+"))
                    if (!mapped.Contains(used.Value)) unmapped.Add(row[0] + ": " + used.Value);
            Check(unmapped.Count == 0, "every c.* field a \"run\" scenario uses is mapped on Windows (" + string.Join(", ", unmapped) + ")");
            Check(!ProbeIds.Oracle("c.text") && ProbeIds.Oracle("c.status") && ProbeIds.Rgb(0x10, 0x7c, 0x10) == "#107c10", "the log is excluded as on the Mac; tints as #rrggbb");
            Check(ProbeIds.Tab("ManageTab") == 2 && ProbeIds.Tab("PrepareTab") == 1 && ProbeIds.Tab("StatusTab") == 0 && ProbeIds.Tab("") == -1,
                  "tab.sel: the shared tab id (Manage 2, as the Mac's), not the tab's place among Windows' tabs");
            Check(ProbeIds.FieldOf("menu.Manage.Stems Cache.Install.en") == "menu.Manage.Stems Cache.Install" && ProbeIds.FieldOf("c.status.str") == "c.status" && ProbeIds.FieldOf("x") == null,
                  "a key's field: the key without its last property, never its parent menu");
            Check(ProbeIds.Mapped.Contains("c.updateItem") && macIds.Contains(ProbeIds.UpdateItem) && ProbeIds.Menu("menu", ProbeIds.AppMenu, new HashSet<string>()) == "menu.App",
                  "the update item is c.updateItem, the app menu \"App\", as on the Mac");
        }

        // the UI thread's turns (ProbeTurns, SP1-SP4): a fake clock and the dispatcher's hooks as
        // WPF raises them; the turn ends at DispatcherInactive, after the render operation
        private static void ProbeTurnsAccounting()
        {
            const long ms = 1000000;
            long t = 0;
            var lines = new List<string>();
            var acks = new List<string>();

            // a render operation: 1 ms, Rendering's sample (2 ms, the probe's), then layout and
            // render (6 ms), then the dispatcher goes inactive: the frame and its 7 ms in one turn
            var tr = new ProbeRecorder(ProbeMode.Trace, () => t, lines.Add);
            var w = new ProbeWaits(() => t, (id, r) => acks.Add(id + " " + r));
            var turns = new ProbeTurns(() => t, tr, w);
            t = 0; turns.Started();
            t = 1 * ms; turns.Sample(() => { t += 2 * ms; return new Dictionary<string, string> { ["c.status.str"] = "Ready" }; });
            t = 9 * ms; turns.Completed();
            w.Add("q", 1000, () => { t += 3 * ms; return false; });   // a pending wait whose test takes 3 ms
            acks.Clear();
            t = 10 * ms; turns.Inactive();
            Check(lines.Count == 1 && lines[0] == "{\"k\":\"frame\",\"t\":10000000,\"n\":1,\"busy\":7000000,\"d\":{\"c.status.str\":\"Ready\"}}",
                  "the frame lands in its own turn, its busy time the render operation's without the sample (" + string.Join(" | ", lines) + ")");
            Check(tr.BusyNs == 2 * ms && tr.WaitNs == 3 * ms, "the sample is the probe's busy time, the wait's test wait_ns, neither the app's (" + tr.BusyNs + ", " + tr.WaitNs + ")");
            t = 20 * ms; turns.Started(); t = 21 * ms; turns.Completed();
            t = 22 * ms; turns.Inactive();
            Check(tr.WaitNs == 6 * ms && lines.Count == 1, "the next turn: no sample, no frame; the waits again at its end");

            // perf mode: one turn record per inactive, nested operations counted once
            var pl = new List<string>();
            var perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            var pw = new ProbeWaits(() => t, (id, r) => acks.Add(id + " " + r));
            var pt = new ProbeTurns(() => t, perf, pw);
            t = 0; pt.Started();            // an outer operation, 10 ms of its own
            t = 4 * ms; pt.Started();       // a nested one, 3 ms
            t = 7 * ms; pt.Completed();
            t = 13 * ms; pt.Completed();
            t = 15 * ms; pt.Inactive();
            Check(perf.PerfTsv(1, 0, "x64").Contains("\n15000000\tturn\tturn\t13000000\n"), "nested operations: each counted once, 13 ms in all (" + perf.PerfTsv(1, 0, "x64").Replace("\n", "|") + ")");

            Func<ProbeRecorder, List<long>> turnsOf = r => r.PerfTsv(1, 0, "x64").Split('\n').Where(l => l.Contains("\tturn\t")).Select(l => long.Parse(l.Split('\t')[3])).ToList();

            // a dialog through the real Presenter (SR21): its build and first layout (30 ms) and a
            // 1 ms child in the frame's first turn; the 5 s idle and the input-led close (4 ms:
            // teardown, the reply's read-back) left out; the Pump's own 300 ns after
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, pw);
            var posted = new Queue<Action>();
            var failed = new List<Exception>();
            var pr = new Presenter(posted.Enqueue, failed.Add, pt.ModalBegin, pt.ModalEnd);
            pr.Run(() =>
            {
                t += 30 * ms;                         // construction, the HWND, SizeToContent's layout
                pt.Started(); t += 1 * ms; pt.Completed();
                pt.Inactive();                        // the frame's first turn
                t += 5000 * ms;                       // idle
                t += 4 * ms;                          // the close
            });
            t = 0; pt.Started();                      // the Pump's operation
            var pump = posted.Dequeue();
            t = 100; pump();
            t += 300; pt.Completed();
            pt.Inactive();
            var turnsN = turnsOf(perf);
            Check(failed.Count == 0 && turnsN.SequenceEqual(new[] { 31 * ms + 100, 300L }),
                  "a dialog's build and first layout in its frame's first turn; its idle and the input-led close left out (" + string.Join(",", turnsN) + ")");

            // a picker whose own loop raised no turn: the whole bracket left out, as nested time
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, pw);
            t = 0; pt.Started(); t = 100; pt.ModalBegin(); t += 5000 * ms; pt.ModalEnd(); t += 300; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 400L }), "no turn inside the bracket: its 5 s left out, the 400 ns around it the operation's (" + string.Join(",", turnsN) + ")");

            // nested time before the bracket (3 ms of a child) stays the child's: 3 + 2 + 2 ms
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, pw);
            t = 0; pt.Started(); t = 1 * ms; pt.Started(); t = 4 * ms; pt.Completed();
            t = 5 * ms; pt.ModalBegin(); t = 100 * ms; pt.ModalEnd(); t = 102 * ms; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 7 * ms }), "a turnless bracket after a child: the child's 3 ms and the operation's own 4 ms (" + string.Join(",", turnsN) + ")");

            // two turns in the frame: its idle ends at the next child's start
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, pw);
            t = 0; pt.Started(); pt.ModalBegin();
            t = 10 * ms; pt.Inactive();               // the build
            t = 2000 * ms; pt.Started(); t = 2003 * ms; pt.Completed(); pt.Inactive();
            t = 3000 * ms; pt.ModalEnd(); t += 1; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 10 * ms, 3 * ms, 1L }) && perf.BusyNs == 0,
                  "per turn: the build, then the child alone (the idle before it left out), then the operation's last 1 ns (" + string.Join(",", turnsN) + ")");

            // X1: a child after the frame's last turn closes the dialog (the driver's answer): its
            // 2 ms, the teardown's 1 ms, the tail's 1 ms
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, new ProbeWaits(() => t, (id, r) => { }));
            t = 0; pt.Started(); pt.ModalBegin(); t = 10 * ms; pt.Inactive();
            t = 2000 * ms; pt.Started(); t = 2002 * ms; pt.Completed();
            t = 2003 * ms; pt.ModalEnd(); t = 2004 * ms; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 10 * ms, 4 * ms }),
                  "a child after the frame's last turn closes it (the driver's answer): the child's 2 ms, the teardown and the read-back counted once (" + string.Join(",", turnsN) + ")");

            // X2: a sample (2 ms) during the frame's idle span: never the operation's
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, new ProbeWaits(() => t, (id, r) => { }));
            t = 0; pt.Started(); pt.ModalBegin(); t = 10 * ms; pt.Inactive();
            t = 1000 * ms; pt.Sample(() => { t += 2 * ms; return new Dictionary<string, string>(); });
            t = 3000 * ms; pt.ModalEnd(); t = 3003 * ms; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 10 * ms, 3 * ms }), "a sample during the frame's idle span: never the operation's (" + string.Join(",", turnsN) + ")");

            // X3: two turns in the frame with no operation between them: the idle between is not busy
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pt = new ProbeTurns(() => t, perf, new ProbeWaits(() => t, (id, r) => { }));
            t = 0; pt.Started(); pt.ModalBegin(); t = 10 * ms; pt.Inactive();
            t = 1000 * ms; pt.Inactive();
            t = 3000 * ms; pt.ModalEnd(); t = 3003 * ms; pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 10 * ms, 0L, 3 * ms }), "two turns with no operation between them: the idle between is not busy (" + string.Join(",", turnsN) + ")");

            // the waits' checks and a turn record costing 2 ms inside a frame: wait_ns and
            // probe_busy_ns, never the showing operation's (its later turn 0)
            perf = new ProbeRecorder(ProbeMode.Perf, () => { t += 2 * ms; return t; }, pl.Add);
            pw = new ProbeWaits(() => t, (id, r) => acks.Add(id + " " + r));
            pt = new ProbeTurns(() => t, perf, pw);
            pw.Add("slow", 100000, () => { t += 5 * ms; return false; });
            t = 0; pt.Started(); pt.ModalBegin();
            t = 2 * ms; pt.Inactive();                // 2 ms of build; then 2 ms of record, 5 ms of waits
            t += 1 * ms; pt.ModalEnd(); pt.Completed(); pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 2 * ms, 0L }) && perf.WaitNs == 10 * ms && perf.BusyNs == 4 * ms,
                  "a frame's waits and records: wait_ns and probe_busy_ns, not the operation's (" + string.Join(",", turnsN) + "; " + perf.WaitNs + ", " + perf.BusyNs + ")");

            // a nested frame's turn with no bracket: the operation's 1 ms so far, the waits and the
            // rest (idle) not its own
            perf = new ProbeRecorder(ProbeMode.Perf, () => t, pl.Add);
            pw = new ProbeWaits(() => t, (id, r) => acks.Add(id + " " + r));
            pt = new ProbeTurns(() => t, perf, pw);
            pw.Add("slow", 100000, () => { t += 5 * ms; return false; });
            t = 0; pt.Started();
            t = 1 * ms; pt.Inactive();                // 5 ms of waits
            t = 8 * ms; pt.Completed();
            t = 9 * ms; pt.Inactive();
            turnsN = turnsOf(perf);
            Check(turnsN.SequenceEqual(new[] { 1 * ms, 0L }) && perf.WaitNs == 10 * ms, "a nested turn: the operation's time so far in it, its waits wait_ns (" + string.Join(",", turnsN) + ")");

            // the turn's record costing 2 ms: probe_busy_ns in perf mode, not in perf-min (Probe.swift's
            // rule), the app's turns unchanged in both
            foreach (var mode in new[] { ProbeMode.Perf, ProbeMode.PerfMin })
            {
                var costly = new ProbeRecorder(mode, () => { t += 2 * ms; return t; }, pl.Add);
                var ct = new ProbeTurns(() => t, costly, new ProbeWaits(() => t, (id, r) => { }));
                t = 0; ct.Started(); t = 1 * ms; ct.Completed(); ct.Inactive();
                ct.Started(); t += 1 * ms; ct.Completed(); ct.Inactive();
                turnsN = turnsOf(costly);
                long want = mode == ProbeMode.Perf ? 4 * ms : 0;
                Check(costly.BusyNs == want && turnsN.SequenceEqual(new[] { 1 * ms, 1 * ms }),
                      ProbeConf.Name(mode) + ": a 2 ms record " + (want > 0 ? "in" : "not in") + " probe_busy_ns, the turns the app's 1 ms each (" + costly.BusyNs + "; " + string.Join(",", turnsN) + ")");
            }

            // perf-min: the turn recorded
            var min = new ProbeRecorder(ProbeMode.PerfMin, () => t, pl.Add);
            var mt = new ProbeTurns(() => t, min, new ProbeWaits(() => t, (id, r) => { }));
            t = 0; mt.Started(); t = 1; mt.Completed(); mt.Inactive();
            Check(min.BusyNs == 0 && min.PerfTsv(1, 0, "x64").Contains("\tturn\tturn\t1\n"), "perf-min: the turn recorded, no probe_busy_ns");

            // a completion with nothing started (never an aborted operation's) changes nothing
            mt.Completed(); mt.ModalEnd(); mt.Inactive();
            Check(mt.Depth == 0 && min.PerfTsv(1, 0, "x64").Contains("\tturn\tturn\t0\n"), "stray hooks: no negative or borrowed time");
        }

        // the driver's commands and waits
        private static void ProbeDriver()
        {
            // probe.conf's stand-in cheap facts (W13b): typed, by facts.def's name, refused when unknown
            var bad = new List<string>();
            var facts = ProbeFacts.Parse(ProbeConf.Parse("mode=trace\nout=x\nfact.HELPER_THERE=flag:1\nfact.APPLE_SILICON=flag:1\n"
                + "fact.CONFIG_MAX_GB=str:20\nfact.LOC_PAUSED_FLOOR=i64:-1\nfact.REKORDBOX_VERSION=str:7.2.19=x\n"
                + "fact.NOSUCH=flag:1\nfact.HELPER_THERE2=flag:1\nfact.3=flag:1\nfact.helper_there=flag:1\nfact.ENGINE=flag:2\nfact.STEMS_QUALITY=i64:x\nfact.ORT_LISTED=1\n"), bad);
            Check(facts.Count == 5 && bad.Count == 7, "facts: five good lines taken, seven refused (" + string.Join("; ", bad) + ")");
            var wrote = new List<string>();
            ProbeFacts.Write(facts, (id, v) => wrote.Add("flag " + (Fact)id + " " + v), (id, v) => wrote.Add("i64 " + (Fact)id + " " + v),
                             (id, v) => wrote.Add("str " + (Fact)id + " " + v));
            Check(wrote.SequenceEqual(new[] { "flag FACT_APPLE_SILICON 1", "str FACT_CONFIG_MAX_GB 20", "flag FACT_HELPER_THERE 1", "i64 FACT_LOC_PAUSED_FLOOR -1",
                                              "str FACT_REKORDBOX_VERSION 7.2.19=x" }),
                  "facts: each through its kind's writer, by name order, a str keeping its '=' (" + string.Join(" | ", wrote) + ")");
            Check(ProbeFacts.Parse(ProbeConf.Parse("mode=trace\nout=x\n"), bad).Count == 0, "facts: none without fact. lines");
            // a valid kind that isn't the fact's (the core would drop the write without a word): refused
            var bad2 = new List<string>();
            var wrong = ProbeFacts.Parse(ProbeConf.Parse("fact.ENGINE=flag:1\nfact.CONFIG_MAX_GB=i64:20\n"), bad2);
            Check(wrong.Count == 0 && bad2.Count == 2 && bad2.All(b => b.Contains("facts.def says str, not ")) && facts.Count + wrong.Count == 5 && bad.Count + bad2.Count == 9,
                  "facts: the wrong kind refused by facts.def's own (5 taken, 9 refused in all: " + string.Join("; ", bad2) + ")");
            // esc: a str of several lines on probe.conf's one line (the Status tab's sets, vm/w13b-fixture.ps1)
            var bad3 = new List<string>();
            var esc = ProbeFacts.Parse(ProbeConf.Parse("fact.KEPT_SETS=esc:1001\\t13 Dec 2026\\t12\\n1002\\t\\t0\\nC:\\\\x\\t-\\t3\n"
                + "fact.ENGINE=esc:a\\qb\nfact.CONFIG_MAX_GB=esc:20\\\nfact.REKORDBOX_VERSION=str:C:\\new\\tracks\n"), bad3);
            Check(esc.Count == 2 && esc[0].Id == Fact.FACT_KEPT_SETS && esc[0].S == "1001\t13 Dec 2026\t12\n1002\t\t0\nC:\\x\t-\t3"
                  && esc[1].Id == Fact.FACT_REKORDBOX_VERSION && esc[1].S == "C:\\new\\tracks",
                  "facts: esc's \\n \\t \\\\ decoded; str keeps every '\\' (" + string.Join(" | ", esc.Select(x => x.S)) + ")");
            Check(bad3.Count == 2 && bad3.All(b => b.Contains("esc:TEXT")) && !bad3.Any(b => b.Contains("a\\qb")),
                  "facts: an unknown escape and a lone '\\' at the end refused, the value never logged (" + string.Join("; ", bad3) + ")");
            var big = string.Join("\\n", Enumerable.Range(0, 5000).Select(i => (1000 + i) + "\\t13 Dec 2026\\t" + (i % 40)));
            var bigFacts = ProbeFacts.Parse(ProbeConf.Parse("fact.KEPT_SETS=esc:" + big + "\n"), new List<string>());
            Check(bigFacts.Count == 1 && bigFacts[0].S.Split('\n').Length == 5000 && bigFacts[0].S.Split('\n')[4999] == "5999\t13 Dec 2026\t39",
                  "facts: a 5,000-set esc line, as the fixture writes it, gives 5,000 lines");
            var kindless = Enum.GetValues(typeof(Fact)).Cast<Fact>().Where(f => "fis".IndexOf(FactKinds.Of(f)) < 0).ToList();
            Check(kindless.Count == 0 && FactKinds.Of(Fact.FACT_HELPER_THERE) == 'f' && FactKinds.Of(Fact.FACT_LOC_PAUSED_FLOOR) == 'i' && FactKinds.Of(Fact.FACT_CONFIG_MAX_GB) == 's',
                  "facts: gen_ids' FactKinds covers every Fact (" + string.Join(",", kindless) + ")");

            var c = ProbeCommand.Parse("17 wait 2000 c.status.str=Ready to go");
            Check(c.Id == "17" && c.Verb == "wait" && c.Arg == "2000 c.status.str=Ready to go", "a command: id, verb, the rest as its argument");
            Check(ProbeCommand.Parse("17") == null && ProbeCommand.Parse("") == null && ProbeCommand.Parse("3 quit").Arg == "", "too short: none; no argument: empty");

            long t = 0;
            var acks = new List<string>();
            var w = new ProbeWaits(() => t, (id, r) => acks.Add(id + " " + r));
            string status = "Checking…";
            var test = ProbeWaits.Condition("2000 c.status.str=Ready = set", k => k == "c.status.str" ? status : "", out long ms);
            Check(test != null && ms == 2000 && !test(), "wait: the key, '=' and the rest as the value");
            w.Add("1", ms, test);
            var neg = ProbeWaits.Condition("50 app.modal!=", k => "", out long ms2);
            w.Add("2", ms2, neg);
            t = 10 * 1000000; w.Check();
            Check(acks.Count == 0 && w.Pending == 2, "neither yet");
            status = "Ready = set";
            t = 20 * 1000000; w.Check();
            Check(acks.SequenceEqual(new[] { "1 ok" }), "the first met: acked once (" + string.Join(",", acks) + ")");
            t = 60 * 1000000; w.Check();
            Check(acks.SequenceEqual(new[] { "1 ok", "2 timeout" }) && w.Pending == 0, "the second past its deadline: timeout");
            w.Add("3", 10, () => true);
            Check(acks.Last() == "3 ok" && w.Pending == 0, "met already: acked at once");
            Check(ProbeWaits.Condition("x c=1", k => "", out _) == null && ProbeWaits.Condition("10 nokey", k => "", out _) == null, "a bad wait: usage");
            Check(ProbeWaits.Numbers("200 5000", 2).SequenceEqual(new long[] { 200, 5000 }) && ProbeWaits.Numbers("200", 2) == null && ProbeWaits.Numbers("-1 2", 2) == null, "quiet's numbers");

            // the start record's arch: the host from IsWow64Process2, never Prism's emulated OSArchitecture
            const Architecture A64 = Architecture.Arm64, X64 = Architecture.X64, X86 = Architecture.X86;
            Check(ProbeArch.Label(A64, ProbeArch.MachineArm64, A64) == "arm64", "arch: an ARM64 process: arm64");
            Check(ProbeArch.Label(X64, ProbeArch.MachineArm64, X64) == "x64-prism", "arch: x64 on ARM64, OSArchitecture saying X64 (Prism): x64-prism");
            Check(ProbeArch.Label(X64, 0x8664, X64) == "x64" && ProbeArch.Label(X64, 0x8664, A64) == "x64", "arch: x64 on an x64 host: x64, whatever the fallback says");
            Check(ProbeArch.Label(X64, 0, A64) == "x64-prism" && ProbeArch.Label(X64, 0, X64) == "x64", "arch: no IsWow64Process2 (native 0): OSArchitecture decides");
            Check(ProbeArch.Label(X86, 0x8664, X64) == "x86" && ProbeArch.Label(X86, ProbeArch.MachineArm64, X64) == "x86",
                  "arch: an x86 process says x86 (stats.py refuses it), never x64");
            const PortableExecutableKinds IL = PortableExecutableKinds.ILOnly;
            Check(ProbeArch.Exe(IL, ImageFileMachine.I386) == "anycpu" && ProbeArch.Exe(IL | PortableExecutableKinds.PE32Plus, ImageFileMachine.AMD64) == "x64",
                  "exe: AnyCPU (ILOnly, I386) and x64-forced (ILOnly|PE32Plus, AMD64), from the real PE enums");
            Check(ProbeArch.Exe(IL | PortableExecutableKinds.Preferred32Bit, ImageFileMachine.I386) == "other"
                  && ProbeArch.Exe(IL | PortableExecutableKinds.Required32Bit, ImageFileMachine.I386) == "other"
                  && ProbeArch.Exe(IL | PortableExecutableKinds.PE32Plus, ImageFileMachine.IA64) == "other"
                  && ProbeArch.Exe(IL, ImageFileMachine.AMD64) == "other",
                  "exe: 32-bit preferred or required, another machine, or a PE32 claiming AMD64: other");
            long t0 = 0;
            var rx = new ProbeRecorder(ProbeMode.Perf, () => t0, _ => { }) { Exe = "anycpu" };
            var sx = new List<string>();
            var rs = new ProbeRecorder(ProbeMode.Perf, () => t0, sx.Add) { Exe = "anycpu" };
            rs.Start(3, -1, "x64-prism");
            t0 = 5; rx.Event("windowDrawn");
            Check(rx.PerfTsv(3, -1, "x64-prism").StartsWith("# mode=perf pid=3 launch_ns=-1 probe_busy_ns=0 wait_ns=0 arch=x64-prism exe=anycpu shell=wpf\n")
                  && sx.Count == 1 && sx[0].Contains("\"arch\":\"x64-prism\",\"exe\":\"anycpu\",\"shell\":\"wpf\""),
                  "exe after arch in the tsv header and the start record (" + sx.FirstOrDefault() + ")");
        }

        // the Prepare tab's model (PrepareModel.cs) over a fake snapshot: every control from its
        // field, rows by delta, the echo guard, user intent only from the input side
        private sealed class FakePrep : IPrepareSource
        {
            public readonly Dictionary<Field, object> F = new Dictionary<Field, object>();
            public readonly Dictionary<(Column, int), object> Cells = new Dictionary<(Column, int), object>();
            public readonly Dictionary<Table, int> RowsOf = new Dictionary<Table, int>();
            public readonly Dictionary<Table, ulong> Gen = new Dictionary<Table, ulong>();
            public readonly Dictionary<Table, int[]> Ch = new Dictionary<Table, int[]>();
            public readonly Dictionary<Table, int> Old = new Dictionary<Table, int>();
            public bool Full = true;
            public string Str(Field f) => F.TryGetValue(f, out var v) ? (string)v : "";
            public bool Flag(Field f) => F.TryGetValue(f, out var v) && (bool)v;
            public long I64(Field f) => F.TryGetValue(f, out var v) ? (long)v : 0;
            public double F64(Field f) => F.TryGetValue(f, out var v) ? (double)v : 0;
            public int Rows(Table t) => RowsOf.TryGetValue(t, out int n) ? n : 0;
            public ulong ListGen(Table t) => Gen.TryGetValue(t, out ulong g) ? g : 1;
            public string CellStr(Column c, int row) => Cells.TryGetValue((c, row), out var v) ? (string)v : "";
            public long CellI64(Column c, int row) => Cells.TryGetValue((c, row), out var v) ? (long)v : 0;
            public bool CellFlag(Column c, int row) => Cells.TryGetValue((c, row), out var v) && (bool)v;
            public bool Reload(Table t) => Full;
            public int OldRows(Table t) => Old.TryGetValue(t, out int n) ? n : Rows(t);
            public int[] Changed(Table t) => Ch.TryGetValue(t, out var c) ? c : new int[0];
            /// one outline row: parent, title, kind, open, secondary
            public void Out(int r, long parent, string title, long kind, bool open = false, bool secondary = false)
            {
                Cells[(Column.C_OUT_PARENT, r)] = parent;
                Cells[(Column.C_OUT_ID, r)] = "id" + r;
                Cells[(Column.C_OUT_TITLE, r)] = title;
                Cells[(Column.C_OUT_KIND, r)] = kind;
                Cells[(Column.C_OUT_OPEN, r)] = open;
                Cells[(Column.C_OUT_SECONDARY, r)] = secondary;
            }
            public void Trk(int r, string name, string result, string why)
            {
                Cells[(Column.C_TRK_NAME, r)] = name;
                Cells[(Column.C_TRK_RESULT, r)] = result;
                Cells[(Column.C_TRK_WHY, r)] = why;
            }
            /// one kept set: name, tracks, until, state
            public void SetRow(int r, string name, string tracks, string until, string state)
            {
                Cells[(Column.C_SET_NAME, r)] = name;
                Cells[(Column.C_SET_TRACKS, r)] = tracks;
                Cells[(Column.C_SET_UNTIL, r)] = until;
                Cells[(Column.C_SET_STATE, r)] = state;
            }
            /// the next render is a delta: these rows changed
            public void Delta(Table t, params int[] rows) { Full = false; Ch.Clear(); Old.Clear(); Ch[t] = rows; }
        }

        // The rows' Narrator names (app.md §4.7): their cells as shown, never the row type's name,
        // raised again when a cell changes (the container's Name is bound to Spoken).
        private static void RowNames()
        {
            var s = new FakePrep();
            s.RowsOf[Table.T_PREP_TRACKS] = 2;
            s.Trk(0, "a.mp3", "Ready", "");
            s.Trk(1, "b.mp3", "Skipped", "too short");
            s.RowsOf[Table.T_STATUS_SETS] = 1;
            s.SetRow(0, "Warm-up", "12", "13 Dec 2026", "Ready");
            s.RowsOf[Table.T_PREP_OUTLINE] = 1;
            s.Out(0, -1, "Warm-up (12)", 1);
            var p = new PrepareModel(_ => { });
            var st = new StatusModel(_ => { });
            p.Apply(s);
            st.Apply(s);
            Check(p.Tracks[0].Spoken == "a.mp3, Ready" && p.Tracks[0].ToString() == "a.mp3, Ready", "a track row reads its cells, the empty note left out (" + p.Tracks[0] + ")");
            Check(p.Tracks[1].ToString() == "b.mp3, Skipped, too short", "a track row reads every cell in column order (" + p.Tracks[1] + ")");
            Check(st.Sets[0].ToString() == "Warm-up, 12, 13 Dec 2026, Ready", "a kept set reads its cells (" + st.Sets[0] + ")");
            Check(p.OutlineAt(0).ToString() == "Warm-up (12)", "an outline row reads its title (" + p.OutlineAt(0) + ")");
            var raised = new List<string>();
            p.Tracks[0].PropertyChanged += (o, e) => raised.Add(e.PropertyName);
            st.Sets[0].PropertyChanged += (o, e) => raised.Add("set." + e.PropertyName);
            s.Trk(0, "a.mp3", "Prepared", "");
            s.SetRow(0, "Warm-up", "12", "13 Dec 2026", "Checking…");
            s.Delta(Table.T_PREP_TRACKS, 0);
            s.Ch[Table.T_STATUS_SETS] = new[] { 0 };
            p.Apply(s);
            st.Apply(s);
            Check(raised.Contains("Spoken") && raised.Contains("set.Spoken") && p.Tracks[0].Spoken == "a.mp3, Prepared" && st.Sets[0].Spoken.EndsWith("Checking…"),
                "a changed cell raises the row's name again (" + string.Join(",", raised) + ")");
        }

        // W13b's 5,000-row sheets (vm/w13b-fixture.ps1's shape) through the WPF-free models, the half
        // of the spike the Mac can run: one rebuild each, a one-row delta touches one row and keeps
        // the other rows' objects, nothing is sent by the renders, the user's selection sends one
        // select. What only the VM shows: the recycling panels, the drawing and the scroll (w13b.ps1).
        private static void BigSheets()
        {
            const int N = 5000, Folders = 50, Per = 100;
            var sent = new List<string>();
            var st = new StatusModel(sent.Add);
            var stRenders = new List<string>();
            st.SetsRendered = (whole, n) => stRenders.Add((whole ? "reload " : "rows ") + n);
            var s = new FakePrep();
            s.RowsOf[Table.T_STATUS_SETS] = N;
            for (int r = 0; r < N; r++) s.SetRow(r, "Set " + r, (r % 40).ToString(), "13 Dec 2026", "Ready");
            s.F[Field.F_STATUS_SELECTED_ROW] = -1L;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            st.Apply(s);
            long statusMs = sw.ElapsedMilliseconds;
            Check(st.Sets.Count == N && st.Sets[N - 1].Name == "Set 4999" && stRenders.Count == 1 && stRenders[0] == "reload " + N && sent.Count == 0,
                  "5,000 sets: one rebuild, every row, nothing sent (" + statusMs + " ms on this Mac; " + string.Join(",", stRenders) + ")");
            var keep = st.Sets[10];
            s.SetRow(2500, "Set 2500", "41", "13 Dec 2026", "Needs preparing again");
            s.Delta(Table.T_STATUS_SETS, 2500);
            stRenders.Clear();
            st.Apply(s);
            Check(st.Sets[2500].State == "Needs preparing again" && ReferenceEquals(st.Sets[10], keep) && stRenders.Count == 1 && stRenders[0] == "rows 2500" && sent.Count == 0,
                  "5,000 sets: a one-row delta updates that row in place (" + string.Join(",", stRenders) + ")");
            st.SelectedIndex = 4999;
            Check(sent.Count == 1 && sent[0] == "{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":4999,\"gen\":1}", "5,000 sets: the user's selection of the last row sends one select");

            sent.Clear();
            var pm = new PrepareModel(sent.Add);
            var outR = new List<string>();
            var trkR = new List<string>();
            pm.OutlineRendered = (whole, n) => outR.Add((whole ? "reload " : "rows ") + n);
            pm.TracksRendered = (whole, n) => trkR.Add((whole ? "reload " : "rows ") + n);
            var p = new FakePrep();
            // the fixture's Library: a folder of 50 folders of 100 playlists (5,051 rows), every
            // other folder open (mixed expansion); then 5,000 tracks of the chosen playlist
            int rows = 0;
            p.Out(rows++, -1, "Library", 0, open: true);
            for (int f = 0; f < Folders; f++)
            {
                int folder = rows;
                p.Out(rows++, 0, "Folder " + f, 0, open: f % 2 == 0);
                for (int k = 0; k < Per; k++) p.Out(rows++, folder, "Set " + (f * Per + k) + " (12)", 1);
            }
            p.RowsOf[Table.T_PREP_OUTLINE] = rows;
            p.RowsOf[Table.T_PREP_TRACKS] = N;
            for (int r = 0; r < N; r++) p.Trk(r, "big-" + (r + 1) + ".mp3", "To prepare", "about 4 min");
            p.F[Field.F_PREP_SELECTED_ROW] = 2L;
            p.F[Field.F_PREP_OUTLINE_ENABLED] = true;
            sw.Restart();
            pm.Apply(p);
            long prepMs = sw.ElapsedMilliseconds;
            Check(rows == 5051 && pm.Roots.Count == 1 && pm.Roots[0].Children.Count == Folders && pm.Roots[0].Children[1].Children.Count == Per
                  && pm.Roots[0].Children[0].IsExpanded && !pm.Roots[0].Children[1].IsExpanded && pm.Tracks.Count == N && sent.Count == 0
                  && outR.Count == 1 && outR[0].StartsWith("reload") && trkR.Count == 1 && trkR[0] == "reload " + N,
                  "a 5,051-row outline with mixed expansion and 5,000 tracks: one rebuild each, nothing sent (" + prepMs + " ms on this Mac)");
            var keepTrack = pm.Tracks[0];
            p.Trk(4999, "big-5000.mp3", "Ready", "");
            p.Delta(Table.T_PREP_TRACKS, 4999);
            trkR.Clear();
            pm.Apply(p);
            Check(pm.Tracks[4999].Result == "Ready" && ReferenceEquals(pm.Tracks[0], keepTrack) && trkR.Count == 1 && trkR[0] == "rows 4999" && sent.Count == 0,
                  "5,000 tracks: a one-row delta updates that row in place (" + string.Join(",", trkR) + ")");
            // a one-row outline delta (row 1 retitled, its expansion the same): that row in place,
            // no rebuild (the outline's delta path fills rows without OutlineRendered: only a
            // reload calls it), nothing sent; then the user's collapse of one folder: one folder event
            var keepRow = pm.OutlineAt(2);
            outR.Clear();
            p.Out(1, 0, "Folder 0 renamed", 0, open: true);
            p.Delta(Table.T_PREP_OUTLINE, 1);
            pm.Apply(p);
            Check(sent.Count == 0 && outR.Count == 0 && pm.OutlineAt(1).Title == "Folder 0 renamed"
                  && ReferenceEquals(pm.OutlineAt(2), keepRow),
                  "the outline's one-row delta: that row in place, the others kept, no rebuild, nothing sent (" + string.Join(",", outR) + "; " + string.Join(" ", sent) + ")");
            pm.OutlineAt(1).IsExpanded = false;
            Check(sent.Count == 1 && sent[0] == "{\"ev\":\"folder\",\"row\":1,\"open\":false,\"gen\":1}", "the user's collapse in the big outline: one folder event (" + string.Join(" ", sent) + ")");
        }

        // The Status tab's view model (StatusModel.cs, W8b): every control from its S_STATUS field,
        // rows by delta, the echo guard, intent only from input (the Mac shell's ShellStatusTab).
        private static void StatusTab()
        {
            var sent = new List<string>();
            var m = new StatusModel(sent.Add);
            var changed = new List<string>();
            m.PropertyChanged += (o, e) => changed.Add(e.PropertyName);
            var renders = new List<string>();
            m.SetsRendered = (whole, n) => renders.Add((whole ? "reload " : "rows ") + n);
            Check(!m.AgainEnabled && !m.StopEnabled && m.RunLine == "" && m.SelectedIndex == -1 && m.Sets.Count == 0,
                "Status: off and empty until the first render (never a default on)");
            var s = new FakePrep();
            s.RowsOf[Table.T_STATUS_SETS] = 2;
            s.SetRow(0, "Warm-up", "12", "13 Dec 2026", "Ready");
            s.SetRow(1, "Peak", "–", "–", "Checking…");
            s.F[Field.F_STATUS_SELECTED_ROW] = 1L;
            s.F[Field.F_STATUS_AGAIN_ENABLED] = true;
            s.F[Field.F_STATUS_STOP_ENABLED] = true;
            s.F[Field.F_STATUS_RUN_LINE] = "Preparing Peak: 3 of 9";
            m.Apply(s);
            Check(m.Sets.Count == 2 && m.Sets[0].Name == "Warm-up" && m.Sets[0].Tracks == "12" && m.Sets[0].Until == "13 Dec 2026"
                && m.Sets[0].State == "Ready" && m.Sets[1].Tracks == "–" && m.Sets[1].State == "Checking…", "Status: each row's cells from C_SET_*");
            Check(m.SelectedIndex == 1, "Status: the selection from F_STATUS_SELECTED_ROW");
            Check(m.AgainEnabled && m.StopEnabled, "Status: Prepare Again and Stop Keeping… from their fields");
            Check(m.RunLine == "Preparing Peak: 3 of 9", "Status: the run line from F_STATUS_RUN_LINE");
            Check(sent.Count == 0, "Status: a render sends nothing (" + string.Join(" ", sent) + ")");
            // each field turned and back
            foreach (var f in new[] { Field.F_STATUS_AGAIN_ENABLED, Field.F_STATUS_STOP_ENABLED })
            {
                s.Full = false; s.Ch.Clear();
                s.F[f] = false; m.Apply(s);
                bool off = f == Field.F_STATUS_AGAIN_ENABLED ? !m.AgainEnabled && m.StopEnabled : m.AgainEnabled && !m.StopEnabled;
                s.F[f] = true; m.Apply(s);
                Check(off && m.AgainEnabled && m.StopEnabled, "Status: " + f + " turned and back, the other button untouched");
            }
            s.F[Field.F_STATUS_RUN_LINE] = ""; changed.Clear(); m.Apply(s);
            Check(m.RunLine == "" && changed.Contains(nameof(StatusModel.RunLine)), "Status: no run, no line");
            changed.Clear(); m.Apply(s);
            Check(changed.Count == 0, "Status: an unchanged render raises nothing (" + string.Join(",", changed) + ")");
            // the user's selection: sent once, with the list's gen; the one shown again sends nothing
            m.SelectedIndex = 0;
            Check(sent.Count == 1 && sent[0] == "{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":0,\"gen\":1}", "Status: a click on a row sends select (" + string.Join(" ", sent) + ")");
            m.SelectedIndex = 0;
            Check(sent.Count == 1, "Status: the same row again sends nothing");
            m.SelectedIndex = -1;
            Check(sent.Count == 2 && sent[1].Contains("\"row\":-1"), "Status: a deselect is sent too, as the Mac's -1");
            {
                var was = System.Globalization.CultureInfo.CurrentCulture;
                var minus = (System.Globalization.CultureInfo)System.Globalization.CultureInfo.InvariantCulture.Clone();
                minus.NumberFormat.NegativeSign = "\u2212";
                System.Globalization.CultureInfo.CurrentCulture = minus;
                try { m.SelectedIndex = 0; m.SelectedIndex = -1; }
                finally { System.Globalization.CultureInfo.CurrentCulture = was; }
                Check(sent.Count == 4 && sent[3].Contains("\"row\":-1,"), "Status: a deselect's -1 is ASCII under a culture whose minus is U+2212 (" + sent[sent.Count - 1] + ")");
                sent.RemoveRange(2, sent.Count - 2);
                System.Globalization.CultureInfo.CurrentCulture = minus;
                string num, tab;
                try { num = Json.Num(-12L); tab = Json.Tab(2); }
                finally { System.Globalization.CultureInfo.CurrentCulture = was; }
                Check(num == "-12" && tab == "{\"ev\":\"tab\",\"tab\":2}", "Json.Num and Json.Tab write ASCII numbers under that culture (" + num + " " + tab + ")");
            }
            // the core keeps its selection (row 1): the render puts it back, no echo
            sent.Clear(); changed.Clear();
            m.Apply(s);
            Check(m.SelectedIndex == 1 && changed.Contains(nameof(StatusModel.SelectedIndex)) && sent.Count == 0, "Status: the core's selection drawn again, nothing sent");
            // a rebuild resets the list's selection inside the guard (the binding's -1): nothing sent
            m.Sets.CollectionChanged += (o, e) => { if (e.Action == System.Collections.Specialized.NotifyCollectionChangedAction.Reset) m.SelectedIndex = -1; };
            renders.Clear();
            s.Gen[Table.T_STATUS_SETS] = 2;
            m.Apply(s);
            Check(sent.Count == 0 && m.SelectedIndex == 1, "Status: a new gen's reset sends no select, the core's row selected again (" + string.Join(" ", sent) + ")");
            Check(string.Join(",", renders) == "reload 2", "Status: a new gen reloads once (" + string.Join(",", renders) + ")");
            m.SelectedIndex = 0;
            Check(sent.Count == 1 && sent[0].EndsWith("\"gen\":2}"), "Status: a select carries the new gen");
            // a delta: the changed row in place, appended rows added, the others kept
            var keep = m.Sets[0];
            renders.Clear();
            s.SetRow(1, "Peak", "9", "20 Dec 2026", "Ready");
            s.SetRow(2, "Closing", "4", "1 Jan 2027", "Not checked");
            s.RowsOf[Table.T_STATUS_SETS] = 3;
            s.Delta(Table.T_STATUS_SETS, 1);
            m.Apply(s);
            Check(m.Sets.Count == 3 && m.Sets[0] == keep && m.Sets[1].Tracks == "9" && m.Sets[1].State == "Ready" && m.Sets[2].Name == "Closing",
                "Status: a delta updates the changed row in place and appends the new one");
            Check(string.Join(",", renders) == "rows 1", "Status: the probe's list events for a delta (" + string.Join(",", renders) + ")");
            // fewer rows (a set no longer kept): rebuilt; a selection past the end is none
            s.Full = false; s.Ch.Clear();
            s.RowsOf[Table.T_STATUS_SETS] = 1;
            s.F[Field.F_STATUS_SELECTED_ROW] = 2L;
            m.Apply(s);
            Check(m.Sets.Count == 1 && m.SelectedIndex == -1, "Status: fewer rows rebuild the list; a selected row past the end is none");
            // the clicks: the buttons and a row's double click (Prepare Again's id, as the Mac's doubleAction)
            sent.Clear();
            m.Click(StatusModel.PrepareAgainId);
            m.Click(StatusModel.StopKeepingId);
            m.RowDoubleClicked();
            Check(string.Join(" ", sent) == Json.Click("prepare_again") + " " + Json.Click("stop_keeping") + " " + Json.Click("prepare_again"),
                "Status: Prepare Again, Stop Keeping… and a row's double click send their clicks (" + string.Join(" ", sent) + ")");
        }

        private static void PrepareTab()
        {
            var sent = new List<string>();
            var m = new PrepareModel(sent.Add);
            var changed = new List<string>();
            m.PropertyChanged += (o, e) => changed.Add(e.PropertyName);
            var renders = new List<string>();
            m.TracksRendered = (whole, n) => renders.Add((whole ? "reload " : "rows ") + n);
            var s = new FakePrep();
            s.F[Field.F_PREP_NAME] = "Warm-up";
            s.F[Field.F_PREP_INFO] = "12 tracks";
            s.F[Field.F_PREP_INFO_SUB] = "3 to prepare";
            s.F[Field.F_PREP_PREPARE_TITLE] = "Prepare";
            s.F[Field.F_PREP_PREPARE_ENABLED] = true;
            s.F[Field.F_PREP_CHECK_ENABLED] = true;
            s.F[Field.F_PREP_UNTIL] = "2026-12-13";
            s.F[Field.F_PREP_UNTIL_ENABLED] = true;
            s.F[Field.F_PREP_PAUSE_TITLE] = "Pause";
            s.F[Field.F_PREP_PROGRESS] = 0.25;
            s.F[Field.F_PREP_OUTLINE_ENABLED] = true;
            s.F[Field.F_PREP_SELECTED_ROW] = 2L;
            // a folder (0) holding two playlists (1, 2), a playlist at the top (3), an intelligent one (4)
            s.RowsOf[Table.T_PREP_OUTLINE] = 5;
            s.Out(0, -1, "Sets", 0, open: true);
            s.Out(1, 0, "Warm-up (12)", 1);
            s.Out(2, 0, "Peak (40)", 1);
            s.Out(3, -1, "Chill (8)", 1);
            s.Out(4, -1, "Recent", 2, secondary: true);
            s.RowsOf[Table.T_PREP_TRACKS] = 2;
            s.Trk(0, "a.mp3", "Ready", "");
            s.Trk(1, "b.mp3", "To prepare", "about 2 min");
            m.Apply(s);
            Check(m.Name == "Warm-up" && m.Info == "12 tracks" && m.InfoSub == "3 to prepare" && m.PrepareTitle == "Prepare" && m.PrepareEnabled
                  && m.CheckEnabled && m.Until == "2026-12-13" && m.UntilEnabled && !m.Running && m.NotRunning && m.PauseTitle == "Pause"
                  && m.Progress == 0.25 && m.OutlineEnabled && m.SelectedRow == 2, "Prepare tab: every control from its field");
            Check(m.Roots.Count == 3 && m.Roots[0].Children.Count == 2 && m.Roots[0].Children[1].Title == "Peak (40)" && m.Roots[0].IsFolder
                  && m.Roots[0].IsExpanded && !m.Roots[1].IsFolder && m.Roots[2].Secondary && m.Roots[2].Kind == 2,
                  "the outline: a tree from the pre-order rows, a folder open as the core says, the secondary row");
            Check(m.OutlineAt(2).IsSelected && !m.OutlineAt(1).IsSelected && m.Tracks.Count == 2 && m.Tracks[1].Why == "about 2 min",
                  "the core's selection shown; the tracks' rows");
            Check(sent.Count == 0, "a render sends nothing (" + string.Join(" ", sent) + ")");
            Check(m.OutlineById("id3") == m.OutlineAt(3) && m.OutlineById("nope") == null, "a row by rekordbox's id (the probe's verbs)");

            // the user's input: a selection, the one shown again, a deselection
            m.OutlineAt(3).IsSelected = true;
            Check(sent.Count == 1 && sent[0] == "{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":3,\"gen\":1}", "a click on a row: one select with the list's gen (" + string.Join(" ", sent) + ")");
            m.OutlineAt(2).IsSelected = false;
            m.OutlineAt(3).IsSelected = false;
            m.OutlineAt(3).IsSelected = true;
            Check(sent.Count == 1, "a deselection, and the shown row again: nothing more (" + sent.Count + ")");
            m.OutlineAt(0).IsExpanded = false;
            Check(sent.Count == 2 && sent[1] == "{\"ev\":\"folder\",\"row\":0,\"open\":false,\"gen\":1}", "the user's collapse: one folder event");
            sent.Clear();

            // a delta: the changed rows in place, the others' objects kept; the render's own
            // expansion and selection send nothing
            var keep = m.OutlineAt(1);
            var keepTrack = m.Tracks[0];
            s.Out(1, 0, "Warm-up (13)", 1);
            s.Out(0, -1, "Sets", 0, open: true);
            s.F[Field.F_PREP_SELECTED_ROW] = 1L;
            s.Delta(Table.T_PREP_OUTLINE, 0, 1);
            s.Ch[Table.T_PREP_TRACKS] = new[] { 1 };
            s.Trk(1, "b.mp3", "Preparing…", "");
            changed.Clear();
            m.Apply(s);
            Check(m.OutlineAt(1) == keep && keep.Title == "Warm-up (13)" && m.Roots[0].IsExpanded && keep.IsSelected && !m.OutlineAt(3).IsSelected
                  && m.Tracks[0] == keepTrack && m.Tracks[1].Result == "Preparing…", "a delta: rows updated in place, the same objects");
            Check(sent.Count == 0, "the render's expansion and selection: no echo (" + string.Join(" ", sent) + ")");
            Check(changed.Contains("SelectedRow") && !changed.Contains("Name") && !changed.Contains("Info"), "only changed scalars raised (" + string.Join(",", changed) + ")");

            // the estimate's rows as they come: appended, the first ones kept
            s.Delta(Table.T_PREP_TRACKS);
            s.RowsOf[Table.T_PREP_TRACKS] = 4;
            s.Trk(2, "c.mp3", "Checking…", "");
            s.Trk(3, "d.mp3", "Checking…", "");
            m.Apply(s);
            Check(m.Tracks.Count == 4 && m.Tracks[0] == keepTrack && m.Tracks[3].Name == "d.mp3", "rows appended as they come, the first ones kept");
            Check(string.Join(",", renders) == "reload 2,rows 1", "the probe's list events: the whole list once, then the first changed row; appended rows none (" + string.Join(",", renders) + ")");

            // a new list_gen: the list rebuilt once
            s.Full = true;
            s.Gen[Table.T_PREP_TRACKS] = 2;
            s.RowsOf[Table.T_PREP_TRACKS] = 1;
            s.Trk(0, "x.mp3", "Ready", "");
            m.Apply(s);
            Check(m.Tracks.Count == 1 && m.Tracks[0] != keepTrack && m.Tracks[0].Name == "x.mp3", "a new list_gen: rebuilt");

            // a run: the bar, Pause/Resume and Stop in place of Check Again and Prepare
            changed.Clear();
            s.F[Field.F_PREP_RUNNING] = true;
            s.F[Field.F_PREP_PAUSE_TITLE] = "Resume";
            s.F[Field.F_PREP_PROGRESS] = 0.5;
            m.Apply(s);
            Check(m.Running && !m.NotRunning && m.PauseTitle == "Resume" && m.Progress == 0.5 && changed.Contains("NotRunning"), "a run: Running, NotRunning raised, Resume, the progress");

            // the one-shots: each once per new value
            int scrolled = -1, outlineScrolls = 0, focus = 0;
            m.ScrollTracksTo += r => scrolled = r;
            m.ScrollOutlineTo += r => outlineScrolls++;
            m.FocusOutline += () => focus++;
            s.F[Field.F_PREP_SCROLL_ROW] = 0L;
            s.F[Field.F_PREP_SCROLL_SEQ] = 5L;
            s.F[Field.F_PREP_OUTLINE_SCROLL_SEQ] = 2L;
            s.F[Field.F_PREP_FOCUS_SEQ] = 7L;
            m.Apply(s);
            m.Apply(s);
            Check(scrolled == 0 && outlineScrolls == 1 && focus == 1, "the one-shots: once per new value (" + scrolled + ", " + outlineScrolls + ", " + focus + ")");

            // the date and the buttons
            sent.Clear();
            m.UntilPicked(new DateTime(2026, 12, 13));
            m.UntilPicked(new DateTime(2027, 1, 2));
            m.Click("pause");
            Check(sent.Count == 2 && sent[0] == "{\"ev\":\"until\",\"date\":\"2027-01-02\"}" && sent[1] == "{\"ev\":\"click\",\"id\":\"pause\"}",
                  "the date: only a day other than the core's; a button's click (" + string.Join(" ", sent) + ")");
            Check(PrepareModel.Day("2026-12-13") == new DateTime(2026, 12, 13) && PrepareModel.Day("") == null && PrepareModel.Day("13/12/2026") == null, "the core's date read");
        }

        // The views' bindings (the VM's round 5: a TwoWay binding on the get-only Progress stopped
        // Window.Show). WPF can't run here, so the views' sources are read: every binding whose
        // target binds TwoWay by default, or that says TwoWay or OneWayToSource, has a source
        // property with a public setter (on the view model or row type it names in C#, else on
        // every known type with that name), or says OneWay or OneTime. The default-TwoWay table is
        // keyed by property name, conservatively: a name that binds TwoWay by default on any
        // control counts (Expander.IsExpanded is TwoWay, TreeViewItem.IsExpanded isn't). It fails
        // closed: a binding site no branch parses is a finding.
        private static string ThisFile([CallerFilePath] string path = "") => path;

        private static readonly HashSet<string> TwoWayByDefault = new HashSet<string>
        {
            "Value", "IsChecked", "SelectedIndex", "SelectedItem", "SelectedValue", "SelectedDate", "DisplayDate",
            "DisplayDateStart", "DisplayDateEnd", "DisplayMode", "IsExpanded", "IsSelected", "IsOpen", "IsDropDownOpen",
            "IsSubmenuOpen", "IsOverflowOpen",
        };

        // the view models and rows the views bind to
        private static readonly Type[] BoundTypes = typeof(PrepareModel).Assembly.GetTypes()
            .Where(t => t.Namespace == "RbStemsPlus.Shell" && typeof(INotifyPropertyChanged).IsAssignableFrom(t)).ToArray();

        private static bool Settable(Type t, string prop) => t.GetProperty(prop, BindingFlags.Public | BindingFlags.Instance)?.GetSetMethod() != null;

        // the index of the bracket closing s[open] ('(' or '{'), quotes skipped; -1 none
        private static int Closing(string s, int open)
        {
            char o = s[open], c = o == '(' ? ')' : '}';
            int depth = 0;
            char quote = '\0';
            for (int i = open; i < s.Length; i++)
            {
                char ch = s[i];
                if (quote != '\0') { if (ch == quote && s[i - 1] != '\\') quote = '\0'; continue; }
                if (ch == '"' || (ch == '\'' && o == '{')) { quote = ch; continue; }
                if (ch == '(' || ch == '{') depth++;
                else if (ch == ')' || ch == '}') { depth--; if (depth == 0) return ch == c ? i : -1; }
            }
            return -1;
        }

        // s's comma-separated parts at bracket depth 0 (quotes kept whole)
        private static List<string> TopSplit(string s)
        {
            var parts = new List<string>();
            int depth = 0, from = 0;
            char quote = '\0';
            for (int i = 0; i < s.Length; i++)
            {
                char ch = s[i];
                if (quote != '\0') { if (ch == quote) quote = '\0'; continue; }
                if (ch == '"' || ch == '\'') quote = ch;
                else if (ch == '(' || ch == '{') depth++;
                else if (ch == ')' || ch == '}') depth--;
                else if (ch == ',' && depth == 0) { parts.Add(s.Substring(from, i - from).Trim()); from = i + 1; }
            }
            parts.Add(s.Substring(from).Trim());
            return parts.Where(x => x.Length > 0).ToList();
        }

        // a binding as written: its source type (C#'s nameof), property ("" none, null unknown), mode
        private sealed class BindingRef
        {
            public Type Type;
            public string Path = "";
            public string Mode = "default";
        }

        // "new Binding(...) { ... }" from s[at]: the binding and the index past it; null when not one
        private static BindingRef NewBinding(string s, int at, out int end)
        {
            end = at;
            var m = Regex.Match(s.Substring(at), @"^new\s+Binding\b\s*");
            if (!m.Success) return null;
            int i = at + m.Length;
            var b = new BindingRef();
            if (i < s.Length && s[i] == '(')
            {
                int close = Closing(s, i);
                if (close < 0) return null;
                string arg = s.Substring(i + 1, close - i - 1).Trim();
                if (!ReadPath(arg, b)) return null;
                i = close + 1;
                while (i < s.Length && char.IsWhiteSpace(s[i])) i++;
            }
            if (i < s.Length && s[i] == '{')
            {
                int close = Closing(s, i);
                if (close < 0) return null;
                foreach (var part in TopSplit(s.Substring(i + 1, close - i - 1)))
                {
                    var mode = Regex.Match(part, @"^Mode\s*=\s*BindingMode\.(\w+)$");
                    if (mode.Success) b.Mode = mode.Groups[1].Value;
                    var path = Regex.Match(part, @"^Path\s*=\s*new\s+PropertyPath\((?<a>.*)\)$", RegexOptions.Singleline);
                    if (path.Success && !ReadPath(path.Groups["a"].Value.Trim(), b)) return null;
                }
                i = close + 1;
            }
            end = i;
            return b;
        }

        // a path argument: nameof(Type.P), "P", empty, or a variable (unknown)
        private static bool ReadPath(string arg, BindingRef b)
        {
            Match m;
            if (arg.Length == 0) b.Path = "";
            else if ((m = Regex.Match(arg, @"^nameof\((?<t>\w+)\.(?<p>\w+)\)$")).Success)
            {
                b.Type = BoundTypes.FirstOrDefault(x => x.Name == m.Groups["t"].Value);
                b.Path = m.Groups["p"].Value;
                if (b.Type == null) b.Path = null;
            }
            else if ((m = Regex.Match(arg, @"^""(?<s>[^""]*)""$")).Success) b.Path = m.Groups["s"].Value;
            else if (Regex.IsMatch(arg, @"^\w+$")) b.Path = null;   // a variable: unknown
            else return false;
            return true;
        }

        // one binding's verdict: null fine, else why not
        private static string Verdict(string target, bool twoWayTarget, BindingRef b)
        {
            string mode = b.Mode;
            if (mode == "OneWay" || mode == "OneTime") return null;
            bool twoWay = mode == "TwoWay" || mode == "OneWayToSource" || ((mode == "default" || mode == "Default") && twoWayTarget);
            if (!twoWay) return null;
            string what = target + " <- " + (b.Type != null ? b.Type.Name + "." : "") + (b.Path ?? "?") + " (" + mode + ")";
            if (b.Path == null) return what + ": the source isn't known";
            if (b.Path.Length == 0) return what + ": no path";
            if (b.Type != null) return Settable(b.Type, b.Path) ? null : what;
            var owners = BoundTypes.Where(x => x.GetProperty(b.Path, BindingFlags.Public | BindingFlags.Instance) != null).ToList();
            return owners.Count > 0 && owners.All(x => Settable(x, b.Path)) ? null : what;
        }

        private static bool TwoWayName(string name, string element) =>
            TwoWayByDefault.Contains(name) || (name == "Text" && (element == "TextBox" || element == "ComboBox" || element == "Setter"));

        /// The bindings of one source file that can't work, and the sites no branch parses: one line each.
        internal static List<string> BadBindings(string file, string text)
        {
            var bad = new List<string>();
            var handled = new HashSet<int>();
            string f = Path.GetFileName(file);
            void Add(string why) { if (why != null) bad.Add(f + ": " + why); }

            // C#: [BindingOperations.]SetBinding(<target, >DP, binding), new Setter(DP, binding),
            // DisplayMemberBinding = binding; a binding is new Binding(...) { ... } or (SetBinding's
            // string overload) "P"
            foreach (Match m in Regex.Matches(text, @"(?<bo>BindingOperations\.)?SetBinding\(|new\s+Setter\(|DisplayMemberBinding\s*=\s*"))
            {
                bool setter = m.Value.StartsWith("new"), member = m.Value.StartsWith("Display");
                int siteAt = m.Groups["bo"].Success ? m.Index + m.Groups["bo"].Length : m.Index;
                string dp, bindingText;
                int bindingAt;
                if (member)
                {
                    dp = "DisplayMemberBinding";
                    bindingAt = m.Index + m.Length;
                    bindingText = null;
                }
                else
                {
                    int open = m.Index + m.Length - 1, close = Closing(text, open);
                    if (close < 0) continue;   // left unparsed: reported below
                    var args = TopSplit(text.Substring(open + 1, close - open - 1));
                    int dpi = m.Groups["bo"].Success ? 1 : 0;
                    if (args.Count != dpi + 2) continue;
                    var d = Regex.Match(args[dpi], @"^(?<dp>[\w.]+)Property$");
                    if (!d.Success) continue;
                    dp = d.Groups["dp"].Value;
                    bindingText = args[dpi + 1];
                    bindingAt = text.IndexOf(bindingText, open + 1, StringComparison.Ordinal);
                }
                BindingRef b;
                if (bindingText != null && Regex.IsMatch(bindingText, @"^""[^""]*""$"))
                {
                    if (setter) { handled.Add(siteAt); continue; }   // a Setter's plain value
                    b = new BindingRef();
                    ReadPath(bindingText, b);
                }
                else
                {
                    if (setter && (bindingText == null || !bindingText.StartsWith("new Binding"))) { handled.Add(siteAt); continue; }
                    b = bindingAt >= 0 ? NewBinding(text, bindingAt, out _) : null;
                    if (b == null) continue;   // unparsed
                    handled.Add(bindingAt);
                }
                handled.Add(siteAt);
                string name = dp.Substring(dp.LastIndexOf('.') + 1);
                bool twoWayTarget = !member && TwoWayName(name, dp.Contains("TextBox") ? "TextBox" : dp.Contains("ComboBox") ? "ComboBox" : "");
                Add(Verdict(dp, twoWayTarget, b));
            }

            // XAML (files and the views' parsed strings): {Binding ...} in an attribute (a Setter's
            // Value: its Property), its path positional or Path=, its Mode after any nested {...}
            foreach (Match m in Regex.Matches(text, @"\{Binding(?=[\s}])"))
            {
                int close = Closing(text, m.Index);
                if (close < 0) continue;
                var attr = Regex.Match(text.Substring(Math.Max(0, m.Index - 200), Math.Min(200, m.Index)), @"(?<name>[\w.:]+)\s*=\s*[""']\s*$");
                int lt = text.LastIndexOf('<', m.Index);
                if (!attr.Success || lt < 0) continue;
                var el = Regex.Match(text.Substring(lt), @"^<(?<el>[\w.:]+)");
                if (!el.Success) continue;
                int gt = text.IndexOf('>', close);
                string tag = gt > lt ? text.Substring(lt, gt - lt) : "";
                string element = el.Groups["el"].Value, target = attr.Groups["name"].Value;
                handled.Add(m.Index);
                if (target == "Binding") continue;   // a DataTrigger's: OneWay
                if (element == "Setter" && target == "Value")
                {
                    var prop = Regex.Match(tag, @"\bProperty\s*=\s*[""'](?<p>[\w.:]+)[""']");
                    if (!prop.Success) { bad.Add(f + ": a Setter's binding with no Property"); continue; }
                    target = prop.Groups["p"].Value;
                }
                var b = new BindingRef();
                var args = TopSplit(text.Substring(m.Index + 8, close - m.Index - 8));
                for (int i = 0; i < args.Count; i++)
                {
                    var kv = Regex.Match(args[i], @"^(?<k>\w+)\s*=\s*(?<v>.*)$", RegexOptions.Singleline);
                    if (!kv.Success) { if (i == 0) b.Path = args[i]; continue; }
                    if (kv.Groups["k"].Value == "Path") b.Path = kv.Groups["v"].Value.Trim();
                    if (kv.Groups["k"].Value == "Mode") b.Mode = kv.Groups["v"].Value.Trim();
                }
                string name = target.Substring(target.LastIndexOf('.') + 1);
                Add(Verdict(element + "." + target, TwoWayName(name, element), b));
            }

            // fail closed: every binding site is one a branch parsed (a DisplayMemberBinding's own
            // new Binding included), else a finding
            foreach (Match m in Regex.Matches(text, @"SetBinding\(|new\s+Binding\b|\{Binding[\s}]|<Binding\b|DisplayMemberBinding\s*="))
                if (!handled.Contains(m.Index))
                    bad.Add(f + ": unparsed binding at offset " + m.Index + " (" + m.Value.Trim() + ")");
            return bad;
        }

        private static void BindingModes()
        {
            string shellDir = Path.GetDirectoryName(Path.GetDirectoryName(ThisFile()));
            var files = Directory.GetFiles(shellDir, "*.cs").Concat(Directory.GetFiles(shellDir, "*.xaml")).ToList();
            var bad = new List<string>();
            int bindings = 0;
            foreach (var f in files)
            {
                string text = File.ReadAllText(f);
                bindings += Regex.Matches(text, @"SetBinding\(|\{Binding\s").Count;
                bad.AddRange(BadBindings(f, text));
            }
            Check(files.Any(f => f.EndsWith("PrepareView.cs")) && files.Any(f => f.EndsWith("MainWindow.xaml")) && bindings > 20,
                "bindings: the views' sources found (" + files.Count + " files, " + bindings + " bindings)");
            Check(bad.Count == 0, "bindings: every TwoWay-by-default target bound to a get-only property says OneWay, every site parsed (" + string.Join("; ", bad) + ")");
            // the checker itself: the round-5 binding without its OneWay, and the others it must tell apart
            var caught = BadBindings("x.cs", "Bar.SetBinding(RangeBase.ValueProperty, new Binding(nameof(PrepareModel.Progress)));");
            Check(caught.Count == 1, "bindings: Progress bound TwoWay by default is caught (" + caught.Count + ")");
            Check(BadBindings("x.cs", "Bar.SetBinding(RangeBase.ValueProperty, new Binding(nameof(PrepareModel.Progress)) { Mode = BindingMode.OneWay });").Count == 0
                  && BadBindings("x.cs", "b.SetBinding(IsEnabledProperty, new Binding(nameof(PrepareModel.Running)));").Count == 0
                  && BadBindings("x.cs", "t.SetBinding(Selector.SelectedIndexProperty, new Binding(nameof(PrepareModel.SelectedRow)) { Mode = BindingMode.TwoWay });").Count == 1
                  && BadBindings("x.xaml", "<Setter Property='IsExpanded' Value='{Binding IsExpanded, Mode=TwoWay}' />").Count == 0
                  && BadBindings("x.xaml", "<TextBox Text=\"{Binding Name}\" />").Count == 1
                  && BadBindings("x.xaml", "<TextBlock Text=\"{Binding Name}\" />").Count == 0,
                "bindings: OneWay, a OneWay-by-default target, a settable TwoWay source pass; a get-only TwoWay one, TextBox's Text fail");
            // conservative by name (TreeViewItem.IsExpanded isn't TwoWay by default, Expander's is)
            Check(BadBindings("x.xaml", "<Setter Property='IsExpanded' Value='{Binding IsFolder}' />").Count == 1,
                "bindings: IsExpanded counts as TwoWay by default, conservatively (any control's)");
            // BM1: the forms the first checker skipped, each a finding
            var mustFail = new[]
            {
                ("x.cs", "t.SetBinding(Selector.SelectedIndexProperty, \"SelectedRow\");", "the string overload"),
                ("x.cs", "t.SetBinding(RangeBase.ValueProperty, b);", "a binding held in a variable"),
                ("x.cs", "BindingOperations.SetBinding(Bar, RangeBase.ValueProperty, new Binding(nameof(PrepareModel.Progress)));", "BindingOperations.SetBinding"),
                ("x.cs", "Bar.SetBinding(RangeBase.ValueProperty, new Binding { Path = new PropertyPath(nameof(PrepareModel.Progress)) });", "Path = new PropertyPath(...)"),
                ("x.cs", "Bar.SetBinding(RangeBase.ValueProperty, new Binding());", "a Binding with no path on a TwoWay target"),
                ("x.cs", "x.SetBinding(TagProperty, new Binding(nameof(PrepareModel.Progress)) { Converter = new C { }, Mode = BindingMode.TwoWay });", "a C# Mode after nested braces"),
                ("x.cs", "new GridViewColumn { DisplayMemberBinding = new Binding(nameof(TrackRow.Name)) { Mode = BindingMode.TwoWay } }", "DisplayMemberBinding with an explicit TwoWay"),
                ("x.cs", "var b = new Binding(nameof(PrepareModel.Progress));", "a Binding no branch reads"),
                ("x.xaml", "<Slider><Slider.Value><Binding Path='Progress' /></Slider.Value></Slider>", "a <Binding> property element"),
                ("x.xaml", "<Slider Value='{Binding}' />", "{Binding} with no path on a TwoWay target"),
                ("x.xaml", "<Setter Property='Text' Value='{Binding Title}' />", "a style's Text setter, its type unresolved"),
                ("x.xaml", "<TextBlock Tag='{Binding Progress, Converter={StaticResource C}, Mode=TwoWay}' />", "a XAML Mode after nested braces"),
            };
            foreach (var (name, snippet, what) in mustFail)
                Check(BadBindings(name, snippet).Count >= 1, "bindings: " + what + " is a finding (" + string.Join("; ", BadBindings(name, snippet)) + ")");
            // BM2: valid forms the first checker refused, each clean
            var mustPass = new[]
            {
                ("x.xaml", "<Setter Property='IsExpanded' Value='{Binding Path=IsExpanded, Mode=TwoWay}'/>", "a named Path="),
                ("x.xaml", "<Slider Value='{Binding Progress, Converter={StaticResource C}, Mode=OneWay}' />", "a XAML OneWay after nested braces"),
                ("x.cs", "Bar.SetBinding(RangeBase.ValueProperty, new Binding(nameof(PrepareModel.Progress)) { Converter = new C { }, Mode = BindingMode.OneWay });", "a C# OneWay after nested braces"),
                ("x.cs", "style.Setters.Add(new Setter(AutomationProperties.NameProperty, new Binding(\"Spoken\")));", "a C# Setter's OneWay-by-default binding"),
            };
            foreach (var (name, snippet, what) in mustPass)
                Check(BadBindings(name, snippet).Count == 0, "bindings: " + what + " is clean (" + string.Join("; ", BadBindings(name, snippet)) + ")");
        }

        // A DatePicker as WPF's behaves for the glue (the VM's round 5: two sends per refused change):
        // setting SelectedDate raises SelectedDateChanged; a day past DisplayDateEnd raises it again
        // after the picker's own coercion (cleared, then the day back), as the VM saw.
        private sealed class FakePicker
        {
            public DateTime? Value;
            public DateTime End = DateTime.MaxValue;
            /// the picker's coercion on any set (a redraw's own clear-and-back), not only past End
            public bool Transient;
            public Action Changed;
            public readonly List<Action> Later = new List<Action>();
            public void Set(DateTime? v)
            {
                if (Value == v) return;
                Value = v;
                Changed();
                if (v != null && (v.Value > End || Transient))
                {
                    Value = null;
                    Changed();
                    Value = v;
                    Changed();
                }
            }
            public void RunLater()
            {
                var l = new List<Action>(Later);
                Later.Clear();
                foreach (var a in l) a();
            }
        }

        // One user change of "Keep the stems until" sends exactly one until, refused or accepted;
        // a redraw (the render's, the deferred one, the calendar opened) sends none.
        private static void UntilOnce()
        {
            var sent = new List<string>();
            var m = new PrepareModel(sent.Add);
            var pk = new FakePicker();
            var today = new DateTime(2026, 11, 13);
            var glue = new UntilPicker(m, () => pk.Value, d => pk.Set(d), (from, to) => pk.End = to, a => pk.Later.Add(a)) { Today = () => today };
            pk.Changed = glue.Changed;
            m.Applied += glue.Draw;
            var s = new FakePrep();
            s.F[Field.F_PREP_UNTIL] = "2026-11-23";
            m.Apply(s);
            Check(pk.Value == new DateTime(2026, 11, 23) && pk.End == today.AddDays(365) && sent.Count == 0, "until: the core's day drawn, its range, nothing sent");
            // a refused day (400 days on): one send; the core keeps its day, the render draws it back: none
            pk.Set(today.AddDays(400));
            Check(sent.Count == 1 && sent[0] == "{\"ev\":\"until\",\"date\":\"2027-12-18\"}", "until: a refused change sends once (" + string.Join(" ", sent) + ")");
            pk.RunLater();
            s.Full = false; s.Ch.Clear();
            m.Apply(s);
            pk.RunLater();
            Check(sent.Count == 1 && pk.Value == new DateTime(2026, 11, 23), "until: the render draws the core's day back, nothing more sent (" + sent.Count + ")");
            // an accepted day: one send; the render with it: none
            sent.Clear();
            pk.Set(today.AddDays(30));
            Check(sent.Count == 1 && sent[0].Contains("2026-12-13"), "until: an accepted change sends once (" + string.Join(" ", sent) + ")");
            s.F[Field.F_PREP_UNTIL] = "2026-12-13";
            m.Apply(s);
            pk.RunLater();
            Check(sent.Count == 1 && pk.Value == new DateTime(2026, 12, 13), "until: the render with it sends nothing");
            // the same refused day picked again after the core's answer: sent again (one each)
            sent.Clear();
            pk.Set(today.AddDays(400));
            m.Apply(s);
            pk.Set(today.AddDays(400));
            Check(sent.Count == 2, "until: the refused day again, after the render: sent again, once (" + sent.Count + ")");
            m.Apply(s);
            // a cleared field: nothing sent, the core's day back later; the calendar opened past midnight: the new range, none sent
            sent.Clear();
            pk.Set(null);
            pk.RunLater();
            Check(sent.Count == 0 && pk.Value == new DateTime(2026, 12, 13), "until: a cleared field shows the core's day again after the change, nothing sent (PT7)");
            pk.Transient = true; pk.Value = null; glue.Draw(); pk.Transient = false;
            Check(sent.Count == 0 && pk.Later.Count == 0 && pk.Value == new DateTime(2026, 12, 13), "until: a redraw's own events are the render's (" + pk.Later.Count + " queued)");
            today = today.AddDays(1);
            glue.Draw();
            Check(sent.Count == 0 && pk.Value == new DateTime(2026, 12, 13) && pk.End == today.AddDays(365), "until: a cleared field and a redraw past midnight send nothing (" + sent.Count + ")");
        }

        // A Prepare model over the usual outline: a folder (0, open) with two playlists (1, 2), a
        // playlist at the top (3), an intelligent one (4); two tracks.
        private static FakePrep PrepSource()
        {
            var s = new FakePrep();
            s.F[Field.F_PREP_UNTIL] = "2026-12-13";
            s.F[Field.F_PREP_SELECTED_ROW] = 2L;
            s.RowsOf[Table.T_PREP_OUTLINE] = 5;
            s.Out(0, -1, "Sets", 0, open: true);
            s.Out(1, 0, "Warm-up (12)", 1);
            s.Out(2, 0, "Peak (40)", 1);
            s.Out(3, -1, "Chill (8)", 1);
            s.Out(4, -1, "Recent", 2, secondary: true);
            s.RowsOf[Table.T_PREP_TRACKS] = 2;
            s.Trk(0, "a.mp3", "Ready", "");
            s.Trk(1, "b.mp3", "To prepare", "");
            return s;
        }

        // The Prepare tab's review fixes (PT1, PT4, PT5, PT6, PT9, PT10): the echo guards under a
        // binding that writes back during the render, the outline's and the tracks' rebuilds, the
        // one-shots' deliveries, the shown selection, the redraw after every render.
        private static void PrepareGuards()
        {
            // PT5: a fake TwoWay binding writes back through the public setters while the render runs
            var sent = new List<string>();
            var m = new PrepareModel(sent.Add);
            var s = PrepSource();
            m.Apply(s);
            bool inApply = false;
            int writes = 0;
            for (int r = 0; r < m.OutlineCount; r++)
            {
                var row = m.OutlineAt(r);
                row.PropertyChanged += (o, e) =>
                {
                    if (!inApply) return;
                    if (e.PropertyName == nameof(OutlineRow.IsExpanded) && row.IsFolder && !row.IsExpanded)
                    {
                        writes++;
                        row.IsSelected = true;     // a recycled container's selection written back
                        row.IsExpanded = true;     // and its expander toggled back
                    }
                };
            }
            m.PropertyChanged += (o, e) =>
            {
                if (!inApply) return;
                if (e.PropertyName == nameof(PrepareModel.Until)) { writes++; m.UntilPicked(new DateTime(2027, 3, 4)); }
                if (e.PropertyName == nameof(PrepareModel.Name)) { writes++; m.Click("prepare"); }
            };
            s.Out(0, -1, "Sets", 0, open: false);
            s.F[Field.F_PREP_UNTIL] = "2026-12-20";
            s.F[Field.F_PREP_NAME] = "Peak";
            s.Delta(Table.T_PREP_OUTLINE, 0);
            inApply = true;
            m.Apply(s);
            inApply = false;
            Check(writes == 3 && sent.Count == 0, "PT5: writes back during a render (select, expand, until, click) send nothing (" + writes + " writes: " + string.Join(" ", sent) + ")");

            // PT1: every render's pass ends with Applied, a render that changed nothing too
            m = new PrepareModel(sent.Add);
            s = PrepSource();
            int applied = 0;
            m.Applied += () => applied++;
            m.Apply(s);
            s.Full = false; s.Ch.Clear();
            m.Apply(s);
            Check(applied == 2, "PT1: Applied after each render, the same F_PREP_UNTIL included (" + applied + ")");

            // PT4: the tab's name is the probe's
            Check(ProbeIds.Tab("PrepareTab") == CoreApi.CORE_TAB_PREPARE, "PT4: the tab named PrepareTab is the probe's Prepare tab");

            // PT9: a leaf's expander sends nothing; the probe's rows are only the ones shown
            sent.Clear();
            m.OutlineAt(1).IsExpanded = true;
            Check(sent.Count == 0, "PT9: a leaf expanded (a double click) sends no folder event (" + string.Join(" ", sent) + ")");
            m.OutlineAt(1).IsExpanded = false;
            Check(m.ShownOutlineById("id1") == m.OutlineAt(1) && m.ShownOutlineById("id0") == m.OutlineAt(0), "PT9: rows under an open folder are shown");
            m.OutlineAt(0).IsExpanded = false;
            sent.Clear();
            Check(m.ShownOutlineById("id1") == null && m.ShownOutlineById("id3") == m.OutlineAt(3) && m.ShownOutlineById("nope") == null,
                "PT9: a row under a closed folder isn't shown");

            // PT10: the outline rebuilt for a new gen at the same size and for another row count
            m = new PrepareModel(sent.Add);
            s = PrepSource();
            m.Apply(s);
            var first = m.OutlineAt(1);
            s.Full = false; s.Ch.Clear();
            s.Gen[Table.T_PREP_OUTLINE] = 2;
            m.Apply(s);
            Check(m.OutlineAt(1) != first && m.OutlineCount == 5, "PT10: a new outline gen at the same size rebuilds");
            first = m.OutlineAt(1);
            s.RowsOf[Table.T_PREP_OUTLINE] = 4;
            m.Apply(s);
            Check(m.OutlineAt(1) != first && m.OutlineCount == 4, "PT10: another outline row count rebuilds");
            // the tracks swapped at the same size with a new gen (a reload, as core.h's)
            var track = m.Tracks[0];
            s.Full = true;
            s.Gen[Table.T_PREP_TRACKS] = 2;
            s.Trk(0, "x.mp3", "Ready", "");
            m.Apply(s);
            Check(m.Tracks.Count == 2 && m.Tracks[0] != track && m.Tracks[0].Name == "x.mp3", "PT10: the tracks at the same size, a new gen: rebuilt");
            // the tracks' scroll: none at seq 0, one per new value, none on repeats
            int scrolls = 0;
            m.ScrollTracksTo += r => scrolls++;
            s.Full = false; s.Ch.Clear();
            s.F[Field.F_PREP_SCROLL_ROW] = 1L;
            m.Apply(s);
            Check(scrolls == 0, "PT10: no scroll at seq 0");
            s.F[Field.F_PREP_SCROLL_SEQ] = 1L;
            m.Apply(s);
            m.Apply(s);
            Check(scrolls == 1, "PT10: one scroll per new seq, none on repeats (" + scrolls + ")");
            // the core moves the selection: the user's earlier row selected again is sent
            sent.Clear();
            m.OutlineAt(3).IsSelected = true;
            s.F[Field.F_PREP_SELECTED_ROW] = 1L;
            m.Apply(s);
            m.OutlineAt(3).IsSelected = true;
            Check(sent.Count == 2 && sent[1].Contains("\"row\":3"), "PT10: after the core selects row 1, the user's row 3 again is sent (" + string.Join(" ", sent) + ")");

            // PT6: an empty list filled in the same gen is drawn whole, as the Mac's reloadData
            var renders = new List<string>();
            m = new PrepareModel(sent.Add);
            m.TracksRendered = (whole, n) => renders.Add((whole ? "reload " : "rows ") + n);
            s = PrepSource();
            s.RowsOf[Table.T_PREP_TRACKS] = 0;
            m.Apply(s);
            s.Full = false; s.Ch.Clear();
            s.RowsOf[Table.T_PREP_TRACKS] = 2;
            m.Apply(s);
            Check(string.Join(",", renders) == "reload 0,reload 2" && m.Tracks.Count == 2, "PT6: an empty list filled in the same gen: the whole list (" + string.Join(",", renders) + ")");
        }

        // the activation aggregator (app.md §4.3): one event per turn, nothing before "shown"
        private static void Aggregator()
        {
            var sent = new List<string>();
            var posted = new Queue<Action>();
            var a = new Activation(ev => { sent.Add(ev); return 0; }, posted.Enqueue);
            string Turn()
            {
                while (posted.Count > 0) posted.Dequeue()();
                string r = string.Join(" ", sent.ConvertAll(e => e.Substring(7, e.Length - 9)));
                sent.Clear();
                return r;
            }
            a.App(true);
            Check(posted.Count == 0 && Turn() == "", "before \"shown\": nothing is sent or posted");
            a.Ready();
            Check(posted.Count == 1 && Turn() == "activated", "the start's activation is sent after \"shown\"");
            a.App(false); a.App(true);
            Check(Turn() == "activated", "deactivated, then activated in one turn: one \"activated\"");
            a.App(true); a.App(false);
            Check(Turn() == "deactivated", "activated, then deactivated in one turn: \"deactivated\"");
            a.Restored(); a.App(true);
            Check(posted.Count == 1 && Turn() == "activated", "restored and activated in one turn: one flush, one \"activated\"");
            a.Restored();
            Check(Turn() == "activated", "leaving Minimized alone: \"activated\"");
            a.SecondStart(); a.App(true);
            Check(Turn() == "activated check_request", "a second start: \"activated\" then \"check_request\", once");
            Check(Turn() == "", "a turn with no source sends nothing (closing one of our dialogs: no WM_ACTIVATEAPP)");
            a.App(true);
            Turn();
            a.App(true);
            Check(Turn() == "activated", "each turn's activation is its own event");
        }

        // menus.def's Windows shortcuts as WPF takes them (app.md §4.5)
        private static void MenuKeys()
        {
            string K(string key, uint mods)
            {
                var k = MenuKey.Parse(key, mods);
                return k == null ? "none" : k.Key + "/" + k.Text;
            }
            uint ctrl = CoreApi.CORE_MOD_CMD_OR_CTRL;
            Check(K(",", ctrl) == "OemComma/Ctrl+," && K(".", ctrl) == "OemPeriod/Ctrl+.", "Ctrl+, and Ctrl+.");
            Check(K("1", ctrl) == "D1/Ctrl+1" && K("r", ctrl) == "R/Ctrl+R", "Ctrl+1 and Ctrl+R");
            Check(K("F1", 0) == "F1/F1" && K("F4", CoreApi.CORE_MOD_ALT) == "F4/Alt+F4", "F1 and Alt+F4");
            Check(K("x", ctrl | CoreApi.CORE_MOD_SHIFT) == "X/Ctrl+Shift+X", "the modifiers in Windows' order");
            Check(K("", ctrl) == "none" && K("?", ctrl) == "none" && K("F0", 0) == "none" && K("F01", 0) == "none" && K("F25", 0) == "none",
                  "no key, or one the table doesn't know: no shortcut");
            int n = Core.core_menu_count(CoreApi.CORE_OS_WIN), keys = 0, bad = 0;
            for (int i = 0; i < n; i++)
            {
                if (Core.core_menu_at(CoreApi.CORE_OS_WIN, i, out CoreMenuItem m) != CoreApi.CORE_OK) continue;
                string key = Core.Utf8z(m.key);
                if (key.Length == 0) continue;
                keys++;
                if (MenuKey.Parse(key, m.mods) == null) { bad++; Console.WriteLine("  menu " + (Menu)m.id + ": key \"" + key + "\""); }
            }
            Check(keys > 0 && bad == 0, "every Windows menu row's key is one the shell binds (" + keys + " keys)");
        }

        // the window's size and position file (app.md §4.6)
        private static void Place()
        {
            var p = WindowPlace.Parse("v1 -1200 40 -100 760 1\n");
            Check(p != null && p.Left == -1200 && p.Top == 40 && p.Width == 1100 && p.Height == 720 && p.Maximized,
                  "a kept rectangle on a monitor left of the main one, maximized");
            Check(p != null && p.Format() == "v1 -1200 40 -100 760 1\n", "written back as read");
            Check(WindowPlace.Parse("v2 0 0 100 100 0") == null && WindowPlace.Parse("v1 0 0 100 100") == null
                  && WindowPlace.Parse("v1 0 0 0 100 0") == null && WindowPlace.Parse("v1 0 0 100 100 2") == null
                  && WindowPlace.Parse("v1 0 0 900000 100 0") == null && WindowPlace.Parse("v1 0 0 1e3 100 0") == null
                  && WindowPlace.Parse("") == null && WindowPlace.Parse(null) == null,
                  "another version, a missing or bad number, an empty or huge rectangle: ignored");
            // WindowPlace.Fit (review WR5): the monitor's work area, the strip rule, the minimum
            RECT R(int l, int t, int r, int b) => new RECT { left = l, top = t, right = r, bottom = b };
            string F(RECT saved, RECT work, int minW = 960, int minH = 540) { var f = WindowPlace.Fit(saved, work, minW, minH); return f == null ? "none" : f.Value.ToString(); }
            RECT hd = R(0, 0, 1920, 1032);   // a 1080p laptop's work area, the taskbar below
            Check(F(R(100, 50, 1140, 730), hd) == "100,50,1140,730", "inside the work area: unchanged");
            Check(F(R(200, 100, 3640, 2100), hd) == "0,0,1920,1032", "kept on a 4K monitor, opened on the 1080p laptop: shrunk to its work area");
            Check(F(R(1500, 600, 2540, 1280), hd) == "880,352,1920,1032", "hanging off the right and bottom: moved inside");
            Check(F(R(-800, 100, 300, 800), hd) == "0,100,1100,800", "straddling a monitor that is gone, 300 px of the strip on this one: moved onto it");
            Check(F(R(-1000, 100, 20, 800), hd) == "none", "a 20-px sliver of the strip on this monitor: the start size");
            Check(F(R(100, -40, 1140, 640), hd) == "none" && F(R(100, 1032, 1140, 1600), hd) == "none", "the strip's top above or below the work area: the start size");
            Check(F(R(10, 10, 110, 60), hd, 100, 50) == "10,10,110,60", "a window narrower than 120 px needs all its strip inside");
            Check(F(R(100, 100, 1100, 700), R(0, 0, 1097, 569), 1680, 945) == "0,0,1680,945",
                  "a work area smaller than the minimum at the monitor's DPI (960x540 at 175 %): the minimum, at its corner");
            Check(F(R(2020, 30, 3060, 710), R(1920, 0, 3840, 1040)) == "2020,30,3060,710", "a second monitor to the right: its own work area");
            string dir = Path.Combine(Path.GetTempPath(), "rbsp-shell-place-" + Environment.ProcessId);
            string path = Path.Combine(dir, "window");
            Check(WindowPlace.Load(path) == null, "no file: the start size");
            var w = new WindowPlace { Left = 10, Top = 20, Right = 1050, Bottom = 700 };
            bool saved = w.Save(path);
            w.Maximized = true;
            saved &= w.Save(path);
            var back = WindowPlace.Load(path);
            Check(saved && back != null && back.Format() == w.Format() && !File.Exists(path + ".tmp"),
                  "saved (its folder made), saved again over it, read back");
            Directory.Delete(dir, true);
        }

        // (c) a PropertyChanged handler throws part-way (the render fails); the reload after it
        // raises every property again
        private static void ReloadAfterThrow(IntPtr snap)
        {
            var m = new ManageModel();
            PropertyChangedEventHandler bad = (s, e) => throw new InvalidOperationException("a view failed");
            m.PropertyChanged += bad;
            bool threw = false;
            try { m.Apply(snap, true); } catch (InvalidOperationException) { threw = true; }
            m.PropertyChanged -= bad;
            var raised = new HashSet<string>();
            m.PropertyChanged += (s, e) => raised.Add(e.PropertyName);
            m.Apply(snap, true);
            var all = new HashSet<string>();
            foreach (var p in typeof(ManageModel).GetProperties(BindingFlags.Public | BindingFlags.Instance)) all.Add(p.Name);
            Check(threw && raised.SetEquals(all), "after a render that threw, the reload raises every property (" + raised.Count + " of " + all.Count + ")");
        }

        // (d) the presenter: two overlapping requests are shown one at a time, in call order; a
        // nested frame under the first doesn't show the second; Stop completes a queued request
        private static void PresenterOrder()
        {
            var uiq = new BlockingCollection<Action>();
            var order = new List<string>();
            var p = new Presenter(a => uiq.Add(a), e => order.Add("threw " + e.Message));
            bool nestedSawB = false;
            void Nested(int ms) { var sw = Stopwatch.StartNew(); while (sw.ElapsedMilliseconds < ms) if (uiq.TryTake(out var a, 10)) a(); }
            bool okA = false, okB = false;
            var ta = new Thread(() => okA = p.RunAndWait(() => { order.Add("A"); Nested(300); nestedSawB = order.Contains("B"); }));
            ta.Start();
            while (uiq.Count == 0) Thread.Sleep(1);          // A's Pump is posted
            var tb = new Thread(() => okB = p.RunAndWait(() => order.Add("B")));
            tb.Start();
            Thread.Sleep(100);                                 // B waits in the queue
            bool modalWhileQueued = p.ModalOpen;
            var sw2 = Stopwatch.StartNew();
            while ((ta.IsAlive || tb.IsAlive) && sw2.ElapsedMilliseconds < 5000) if (uiq.TryTake(out var a, 10)) a();
            Check(modalWhileQueued && !ta.IsAlive && !tb.IsAlive && okA && okB && string.Join(",", order) == "A,B" && !nestedSawB && !p.ModalOpen,
                  "the presenter: overlapping requests one at a time, in call order, none from a nested frame (" + string.Join(",", order) + ")");
            bool aboutRan = false;
            p.Run(() => aboutRan = true);
            while (uiq.TryTake(out var a2, 10)) a2();
            Check(aboutRan, "a UI-thread request (About) is queued and shown without a wait");

            // WP1: a reporter that throws, and a request whose Show throws: the queue still moves on
            var uiq2 = new BlockingCollection<Action>();
            var bad = new Presenter(a => uiq2.Add(a), e => throw new InvalidOperationException("the reporter failed"));
            bool? r1 = null, r2 = null;
            var t1 = new Thread(() => r1 = bad.RunAndWait(() => throw new InvalidOperationException("a window failed")));
            t1.Start();
            while (uiq2.Count == 0) Thread.Sleep(1);
            var t2 = new Thread(() => r2 = bad.RunAndWait(() => { }));
            t2.Start();
            Thread.Sleep(100);
            var sw3 = Stopwatch.StartNew();
            while ((t1.IsAlive || t2.IsAlive) && sw3.ElapsedMilliseconds < 2000)
            {
                try { if (uiq2.TryTake(out var a3, 10)) a3(); } catch (Exception) { }
            }
            Check(!t1.IsAlive && !t2.IsAlive && r1 == false && r2 == true && !bad.ModalOpen && bad.Idle,
                  "a throwing error report strands no waiter: false, then true, and the presenter idle (" + r1 + ", " + r2 + ")");

            var never = new Presenter(a => { }, e => { });     // its Pump is never run
            bool okC = true;
            var tc = new Thread(() => okC = never.RunAndWait(() => { }));
            tc.Start();
            Thread.Sleep(100);
            never.Stop();
            bool ended = tc.Join(1000);
            Check(ended && !okC && !never.RunAndWait(() => { }), "Stop completes a queued request, and every later one, as not shown");

            // the probe's bracket (TEST builds): around every window's Show, a throwing one too
            var uiq4 = new BlockingCollection<Action>();
            var seen = new List<string>();
            var probed = new Presenter(a => uiq4.Add(a), e => seen.Add("failed"), () => seen.Add("begin"), () => seen.Add("end"));
            probed.Run(() => seen.Add("about"));
            probed.Run(() => throw new InvalidOperationException("x"));
            while (uiq4.TryTake(out var a4, 10)) a4();
            Check(string.Join(",", seen) == "begin,about,end,begin,failed,end", "the modal bracket: begin, the window, end, each time (" + string.Join(",", seen) + ")");
        }

        // (b) PostMain racing Stop: every job is run or dropped, exactly once
        private static void StopRace()
        {
            for (int round = 0; round < 20; round++)
            {
                var ui = new FakeUi();
                var host = NewHost(ui);
                int run = 0, dropped = 0, seen = 0;
                var done = new int[4001];
                host.RunJob = j => { Interlocked.Increment(ref run); Interlocked.Increment(ref done[(int)j]); };
                host.DropJob = j => { Interlocked.Increment(ref dropped); Interlocked.Increment(ref done[(int)j]); };
                var posters = new Thread[4];
                for (int t = 0; t < 4; t++)
                {
                    int t0 = t;
                    posters[t] = new Thread(() => { for (int i = 1; i <= 1000; i++) host.Post((IntPtr)(t0 * 1000 + i)); });
                    posters[t].Start();
                }
                var sw = Stopwatch.StartNew();
                while (sw.ElapsedMilliseconds < 5 + round) if (ui.Queue.TryTake(out var a, 1)) a();
                host.Stop();
                foreach (var t in posters) t.Join();
                while (ui.Queue.TryTake(out var a, 0)) a();
                for (int i = 1; i <= 4000; i++) if (done[i] == 1) seen++;
                if (run + dropped != 4000 || seen != 4000)
                {
                    Check(false, "PostMain racing Stop, round " + round + ": run " + run + ", dropped " + dropped + ", once " + seen);
                    return;
                }
            }
            Check(true, "PostMain racing Stop (20 rounds of 4 x 1000 posts): every job run or dropped, exactly once");
        }

        // I10, I86: the start config's "prepare_tabs" (Host.WithPrepareTabs, from Shell.PrepareShown)
        // decides the menu rows the shell builds from: without it View > Status, View > Prepare and
        // the Prepare menu are hidden and off (no shortcut: MenuOn is false), with it they are on
        private static void PrepareTabsMenus()
        {
            string Base(Host h) => "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + h.NativeList + "}";
            var rows = new[] { RbStemsPlus.Core.Menu.M_VIEW_STATUS, RbStemsPlus.Core.Menu.M_VIEW_PREPARE, RbStemsPlus.Core.Menu.M_PREPARE,
                               RbStemsPlus.Core.Menu.M_PREP_PLAYLIST, RbStemsPlus.Core.Menu.M_PREP_STOP };
            foreach (bool shown in new[] { false, true })
            {
                var ui = new FakeUi();
                var host = NewHost(ui, shellWork: true);
                string config = Host.WithPrepareTabs(Base(host), shown);
                Check(shown ? config.EndsWith(",\"prepare_tabs\":true}") : config == Base(host), "PrepareTabsMenus: the config (" + config + ")");
                Check(host.Start(config), "PrepareTabsMenus: the core takes it (" + shown + ")");
                foreach (var id in rows)
                {
                    uint st = Core.core_menu_state(host.C, (int)id, out IntPtr _, out UIntPtr _);
                    bool hidden = (st & CoreApi.CORE_MENU_HIDDEN) != 0, on = (st & CoreApi.CORE_MENU_ENABLED) != 0;
                    Check(shown ? !hidden : hidden && !on, "PrepareTabsMenus: " + id + " " + (shown ? "shown" : "hidden and off") + " (" + st + ")");
                }
                uint manage = Core.core_menu_state(host.C, (int)RbStemsPlus.Core.Menu.M_VIEW_MANAGE, out IntPtr _, out UIntPtr _);
                Check(manage == CoreApi.CORE_MENU_ENABLED, "PrepareTabsMenus: View > Manage on either way (" + manage + ")");
                host.Stop();
            }
        }

        // WU1: the update commits only on a frame that shows an applied render, once
        private static void Commit()
        {
            var ui = new FakeUi { ThrowRenders = 1 };
            var host = NewHost(ui, shellWork: true);
            string config = "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + host.NativeList + "}";
            Check(host.Start(config) && !host.RenderOk, "a first render that throws: Start true, RenderOk false");
            Core.core_test_set_str(host.C, (int)Field.F_STATUS_LINE, "reloaded", CoreApi.Nts);
            Check(ui.Pump(() => host.RenderOk) && ui.FullReloads == 1 && ui.Model.StatusLine == "reloaded", "the next render, a full reload, applies: RenderOk true");
            ui.ThrowRenders = 1;
            Core.core_test_set_str(host.C, (int)Field.F_STATUS_LINE, "again", CoreApi.Nts);
            Check(ui.Pump(() => !host.RenderOk), "a later render that throws: RenderOk false again");
            host.Stop();

            var g = new CommitGate();
            int finishes = 0;
            Func<bool> fin = () => { finishes++; return true; };
            Check(!g.Rendered(true) && !g.TryCommit(true, fin), "no frame yet: nothing to commit");
            g.Framed();
            Check(!g.TryCommit(false, fin) && !g.Committed, "a frame after a failed render: no commit");
            Check(!g.Rendered(false) && g.Rendered(true), "a failed render posts no try; a good one after the frame does");
            Check(g.TryCommit(true, fin) && g.Committed && finishes == 1, "the good frame: committed, the guard finished");
            Check(!g.TryCommit(true, fin) && !g.Rendered(true) && finishes == 1, "exactly once");
            g = new CommitGate();
            g.Framed();
            Check(!g.TryCommit(true, () => false) && !g.TryCommit(true, fin), "a guard that fired: no commit, never after");
            g = new CommitGate();
            g.Framed();
            Check(g.TryCommit(true, null), "no guard (a normal start): the commit on the first good frame");
        }

        // HG3: a sheet that said FACT_LIB_UNCHECKED warms again on "activated", on a worker, one at a
        // time, then one refresh ("facts_warm"); a sheet that said checked warms nothing
        private static void ActivationWarm()
        {
            var ui = new FakeUi();
            var host = NewHost(ui, shellWork: true);
            string config = "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + host.NativeList + "}";
            int mainId = Thread.CurrentThread.ManagedThreadId;
            int warms = 0, offMainWarms = 0, facts = 0, unchecked_ = 0;
            var gate = new ManualResetEventSlim(true);
            var steps = new List<string>();
            host.WarmFacts = c =>
            {
                Interlocked.Increment(ref warms);
                if (Thread.CurrentThread.ManagedThreadId != mainId && c == host.C) Interlocked.Increment(ref offMainWarms);
                gate.Wait(5000);
                lock (steps) steps.Add("warm");
            };
            host.CheapFacts = (c, sheet) => { facts++; lock (steps) steps.Add("facts"); return unchecked_; };
            Check(host.Start(config) && Volatile.Read(ref warms) == 1 && host.WarmTask.Wait(2000), "ActivationWarm: the start's warm, once");
            // checked: no warm on activation
            Check(host.Send(Activation.Activated) == CoreApi.CORE_OK && !host.LibUnchecked, "ActivationWarm: a checked sheet");
            Thread.Sleep(100);
            Check(Volatile.Read(ref warms) == 1, "ActivationWarm: the fact not set: no warm on activation (" + warms + ")");
            // a sheet that isn't the refresh's (-1) keeps the last answer
            unchecked_ = -1;
            host.Send(Activation.Activated);
            Check(!host.LibUnchecked && Volatile.Read(ref warms) == 1, "ActivationWarm: nothing read (-1): the last answer kept, no warm");
            // unchecked: the activation's own refresh says so, the warm starts off main
            unchecked_ = 1;
            gate.Reset();
            lock (steps) steps.Clear();
            host.Send(Activation.Activated);
            Check(host.LibUnchecked && SpinWait.SpinUntil(() => Volatile.Read(ref warms) == 2, 2000), "ActivationWarm: unchecked: one warm on activation");
            Check(Volatile.Read(ref offMainWarms) == 2, "ActivationWarm: every warm off the UI thread, with the core (HF2)");
            // a second activation while it runs: no second warm
            host.Send(Activation.Activated);
            host.Send(Activation.Activated);
            Thread.Sleep(100);
            Check(Volatile.Read(ref warms) == 2, "ActivationWarm: one warm at a time (" + warms + ")");
            // it ends: one refresh on main, after it; the sheet now says checked
            unchecked_ = 0;
            int before = facts;
            gate.Set();
            Check(ui.Pump(() => facts > before) && host.WarmTask.Wait(2000), "ActivationWarm: the warm's end refreshes");
            ui.Pump(() => false, 200);
            lock (steps)
            {
                int w = steps.IndexOf("warm");
                Check(w >= 0 && steps.IndexOf("facts", w) > w && steps.LastIndexOf("warm") == w,
                      "ActivationWarm: one warm, then its refresh (" + string.Join(",", steps) + ")");
            }
            Check(facts == before + 1 && !host.LibUnchecked, "ActivationWarm: one refresh, no other (" + (facts - before) + ")");
            Check(!ui.Logged("event refused"), "ActivationWarm: \"facts_warm\" accepted");
            // checked again: the next activation warms nothing
            host.Send(Activation.Activated);
            Thread.Sleep(100);
            Check(Volatile.Read(ref warms) == 2, "ActivationWarm: checked again: no warm");
            // a warm that throws: logged, no refresh
            unchecked_ = 1;
            host.Send(Activation.Activated);   // unchecked again: the next warm
            host.WarmTask.Wait(2000);
            ui.Pump(() => false, 200);         // its refresh (still unchecked)
            host.WarmFacts = c => throw new InvalidOperationException("x");
            host.Send(Activation.Activated);
            before = facts;                    // after the activation's own refresh
            Check(host.WarmTask.Wait(2000) && ui.Logged("shell: the facts' warm-up on activation failed: InvalidOperationException"),
                  "ActivationWarm: a warm that throws is logged");
            ui.Pump(() => false, 200);
            Check(facts == before, "ActivationWarm: a failed warm sends no refresh (" + (facts - before) + ")");
            // stopped: no warm
            host.Stop();
            int n = Volatile.Read(ref warms);
            host.WarmFacts = c => Interlocked.Increment(ref warms);
            host.Send(Activation.Activated);
            Thread.Sleep(100);
            Check(Volatile.Read(ref warms) == n, "ActivationWarm: stopped: no warm");
        }

        // HG3 (AW1): the start's warm still running at an unchecked activation: no second warm, and
        // its end refreshes once; a warm on activation still running at Stop() refreshes nothing
        private static void ActivationWarmStartOverlap()
        {
            var ui = new FakeUi();
            var host = NewHost(ui, shellWork: true);
            string config = "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + host.NativeList + "}";
            int warms = 0, facts = 0, unchecked_ = 1;
            var gate = new ManualResetEventSlim(false);
            host.WarmWaitMs = 100;
            host.WarmFacts = c => { Interlocked.Increment(ref warms); gate.Wait(5000); };
            host.CheapFacts = (c, sheet) => { facts++; return unchecked_; };
            Check(host.Start(config) && host.LibUnchecked && !host.WarmTask.IsCompleted,
                  "AW1: the start's warm still running, the sheet unchecked");
            host.Send(Activation.Activated);
            host.Send(Activation.Activated);
            Thread.Sleep(100);
            Check(Volatile.Read(ref warms) == 1, "AW1: the start's warm running: no second warm (" + warms + ")");
            unchecked_ = 0;
            int before = facts;
            gate.Set();
            Check(host.WarmTask.Wait(2000), "AW1: the start's warm ends");
            ui.Pump(() => false, 300);
            Check(facts == before + 1 && !host.LibUnchecked,
                  "AW1: one refresh after the start's warm (" + (facts - before) + "), LibUnchecked=" + host.LibUnchecked);
            Check(!ui.Logged("event refused"), "AW1: \"facts_warm\" accepted");
            // a warm on activation still running at Stop(): its end sends no refresh
            unchecked_ = 1;
            gate.Reset();
            host.Send(Activation.Activated);
            Check(host.LibUnchecked && SpinWait.SpinUntil(() => Volatile.Read(ref warms) == 2, 2000), "AW1: unchecked: a warm on activation");
            host.Stop();
            before = facts;
            gate.Set();
            Check(host.WarmTask.Wait(2000), "AW1: the activation's warm ends after Stop()");
            ui.Pump(() => false, 200);
            Check(facts == before, "AW1: stopped: the warm's end reads no facts (" + (facts - before) + ")");
        }

        // app.md §4.1: the main-only host entries called from another thread report it, touch no
        // window state and return their safe values; on main they run as before
        private static void MainOnly()
        {
            var ui = new FakeUi();
            var host = NewHost(ui, shellWork: true);
            string config = "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + host.NativeList + "}";
            // HF2: the warm runs once, off the UI thread, with the core, and ends before "start"'s first facts read
            var order = new List<string>();
            int mainId = Thread.CurrentThread.ManagedThreadId;
            host.WarmFacts = c =>
            {
                Thread.Sleep(50);
                lock (order) order.Add((Thread.CurrentThread.ManagedThreadId != mainId ? "warm" : "warm-on-main") + (c == host.C ? "" : "?"));
            };
            host.CheapFacts = (c, sheet) => { lock (order) order.Add("facts"); return 0; };
            Check(host.Start(config), "MainOnly: the core starts");
            lock (order) Check(order.Count >= 2 && order[0] == "warm" && order.IndexOf("warm", 1) < 0 && order[1] == "facts",
                               "MainOnly: the warm once, on a worker, with the core, before the first facts read (" + string.Join(",", order) + ")");
            host.Stop();
            // a warm that takes longer than WarmWaitMs: Start goes on and says so; the warm carries on
            ui = new FakeUi();
            host = NewHost(ui, shellWork: true);
            var release = new ManualResetEventSlim(false);
            host.WarmWaitMs = 100;
            host.WarmFacts = c => release.Wait(5000);
            var sw = System.Diagnostics.Stopwatch.StartNew();
            Check(host.Start(config) && sw.ElapsedMilliseconds < 3000, "MainOnly: a slow warm: Start waits WarmWaitMs, not longer (" + sw.ElapsedMilliseconds + " ms)");
            Check(ui.Logged("shell: the facts' warm-up is still running after"), "MainOnly: the slow warm is logged");
            release.Set();
            host.Stop();
            // HR4: Create starts the warm, the window is built meanwhile, Begin sends "start" after it;
            // Begin's bound counts from Create, so the time the window took is not waited again
            ui = new FakeUi();
            host = NewHost(ui, shellWork: true);
            var steps = new List<string>();
            var warmGo = new ManualResetEventSlim(false);
            host.WarmFacts = c => { lock (steps) steps.Add("warm started"); warmGo.Wait(5000); lock (steps) steps.Add("warm done"); };
            host.CheapFacts = (c, sheet) => { lock (steps) steps.Add("facts"); return 0; };
            host.WarmWaitMs = 400;
            var clock = System.Diagnostics.Stopwatch.StartNew();
            Check(host.Create(config), "MainOnly: Create");
            Check(SpinWait.SpinUntil(() => { lock (steps) return steps.Contains("warm started"); }, 2000), "MainOnly: the warm starts at Create, before the window");
            lock (steps) steps.Add("window built");
            Thread.Sleep(300);                     // the window's construction
            lock (steps) Check(!steps.Contains("facts"), "MainOnly: no facts read before Begin");
            long beforeBegin = clock.ElapsedMilliseconds;
            host.Begin();                          // the warm still runs: Begin waits what's left of 400 ms only
            long waited = clock.ElapsedMilliseconds - beforeBegin;
            Check(waited < 300, "MainOnly: Begin waits WarmWaitMs from Create, not again after the window (" + waited + " ms)");
            Check(ui.Logged("shell: the facts' warm-up is still running after"), "MainOnly: the bound reached is logged");
            lock (steps) Check(steps.IndexOf("window built") < steps.IndexOf("facts") && steps.IndexOf("warm started") < steps.IndexOf("window built"),
                               "MainOnly: warm started, window built, then start's facts (" + string.Join(",", steps) + ")");
            Check(host.WarmTask != null && !host.WarmTask.IsCompleted, "MainOnly: the warm task is kept and carries on");
            warmGo.Set();
            Check(host.WarmTask.Wait(2000), "MainOnly: the warm task ends");
            host.Stop();
            ui = new FakeUi();
            host = NewHost(ui, shellWork: true);
            host.WarmFacts = c => throw new EntryPointNotFoundException("core_win_warm_facts");
            Check(host.Start(config) && ui.Logged("shell: the facts' warm-up failed: EntryPointNotFoundException"),
                  "MainOnly: a warm that throws: logged, the start goes on");
            host.Stop();
            ui = new FakeUi();
            host = NewHost(ui, shellWork: true);
            Check(host.Start(config), "MainOnly: the core starts");
            var off = new List<string>();
            host.OffMain = what => { lock (off) off.Add(what); };
            T Entry<T>(string name) where T : class =>
                (T)typeof(Host).GetField(name, System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance).GetValue(host);
            var render = Entry<RenderFn>("renderFn");
            var front = Entry<FrontFactsFn>("frontFactsFn");
            var modal = Entry<ModalOpenFn>("modalOpenFn");
            var facts = Entry<FactsFn>("factsFn");
            var close = Entry<CloseReplyFn>("closeReplyFn");
            Check(front(IntPtr.Zero) == ui.FrontFacts() && modal(IntPtr.Zero) == 0 && off.Count == 0, "MainOnly: on main, front_facts and modal_open answer from the window, nothing reported");
            // the facts entry: the one call the app sets (core_win_cheap_facts), with the core and the sheet
            var cheap = new List<string>();
            host.CheapFacts = (c, sheet) => { cheap.Add((c == host.C ? "core" : "?") + "," + sheet); return 0; };
            facts(IntPtr.Zero, (IntPtr)1234);
            Check(string.Join(";", cheap) == "core,1234", "MainOnly: facts on main is the one cheap-facts call, with the core and the sheet (" + string.Join(";", cheap) + ")");
            int renders = ui.Renders;
            Core.core_snap_retain(host.Held);   // the reference the off-main render must release
            uint f = 1; int m = 0;
            var t = new Thread(() =>
            {
                render(IntPtr.Zero, host.Held, IntPtr.Zero);
                f = front(IntPtr.Zero);
                m = modal(IntPtr.Zero);
                facts(IntPtr.Zero, IntPtr.Zero);
                close(IntPtr.Zero, 0, 1);
            });
            t.Start();
            t.Join();
            Check(string.Join(",", off) == "render,front_facts,modal_open,facts,close_reply", "MainOnly: each main-only entry off main is reported (" + string.Join(",", off) + ")");
            Check(cheap.Count == 1, "MainOnly: facts off main makes no cheap-facts call (" + cheap.Count + ")");
            Check(f == 0 && m == 1, "MainOnly: off main, front_facts answers 0 and modal_open 1 (their safe values)");
            Check(ui.Renders == renders && ui.OffMain == 0, "MainOnly: off main, the window is never touched");
            Check(!host.RenderOk, "MainOnly: a render skipped off main leaves RenderOk false, as a failed one");
            Core.core_test_set_str(host.C, (int)Field.F_STATUS_LINE, "after", CoreApi.Nts);
            int full = ui.FullReloads;
            Check(ui.Pump(() => ui.Model.StatusLine == "after") && ui.FullReloads == full + 1 && host.RenderOk, "MainOnly: the next render on main reloads everything, RenderOk true again");
            host.Stop();
        }

        // WU5: the updater's and the shell's lines share one flush
        private static void Sink()
        {
            var sink = new LogSink(new LogBuffer(null, () => new DateTime(2026, 11, 12, 10, 0, 0)));
            int posted = 0;
            sink.Add("before the window (the updater)");
            sink.Add("before the window (the shell)");
            sink.SetFlush(() => posted++);
            posted++;   // the shell's one flush once the window exists
            Check(sink.Buffer.Take() == "10:00:00  before the window (the updater)\n10:00:00  before the window (the shell)\n",
                  "the lines from before the window are drained by the shell's first flush");
            posted = 0;
            sink.Add("update: a line from the updater");
            sink.Add("a line from the shell");
            Check(posted == 1, "an updater line then a shell line: exactly one flush posted (" + posted + ")");
            Check(sink.Buffer.Take().Split('\n').Length == 3 && posted == 1, "the one flush takes both");
            sink.Add("next batch");
            Check(posted == 2, "the next batch posts its own flush");
        }

        private static void LifeCycle()
        {
            var ui = new FakeUi();
            var host = NewHost(ui, shellWork: true);
            string config = "{\"os\":\"win\",\"version\":\"1.4.0\",\"native\":" + host.NativeList + "}";
            Check(host.Start(config), "core_new with the C# host table, start, the first commit");
            Check(ui.Renders == 1 && ui.FullReloads == 1 && host.Held != IntPtr.Zero, "the first render: synchronous, a full reload, the snapshot held");

            // a field the core changes: one render, only that property raised
            var changed = new List<string>();
            PropertyChangedEventHandler h = (s, e) => changed.Add(e.PropertyName);
            ui.Model.PropertyChanged += h;
            var spinnerPasses = new List<bool>();
            ui.Model.SpinnerPass = on => spinnerPasses.Add(on);
            IntPtr held = host.Held;
            Core.core_test_set_str(host.C, (int)Field.F_STATUS_LINE, "Checking… ✓ 日本語", CoreApi.Nts);
            bool rendered = ui.Pump(() => ui.Renders >= 2);
            Check(rendered && ui.Model.StatusLine == "Checking… ✓ 日本語", "a change renders through the drain (post_main → core_commit), UTF-8 intact");
            Check(changed.Count == 1 && changed[0] == nameof(ManageModel.StatusLine), "PropertyChanged only for the field that moved (" + string.Join(",", changed) + ")");
            Check(ui.FullReloads == 1 && host.Held != held, "the delta render: no reload, the new snapshot held, the previous released");
            Check(spinnerPasses.Count == 1 && !spinnerPasses[0] && !changed.Contains(nameof(ManageModel.Spinner)),
                  "the probe's spinner pass: every S_APP pass, the spinner unchanged too (CoreShell.swift's start/stopAnimation per render)");
            ui.Model.SpinnerPass = null;
            ui.Model.PropertyChanged -= h;

            // the one-shot log seq and the requested tab
            Core.core_test_set_i64(host.C, (int)Field.F_LOG_SHOW_SEQ, 7);
            Core.core_test_set_i64(host.C, (int)Field.F_TAB, CoreApi.CORE_TAB_STATUS);
            ui.Pump(() => ui.Model.LogShowSeq == 7);
            Check(ui.Model.LogShowSeq == 7 && ui.Model.Tab == CoreApi.CORE_TAB_STATUS, "F_LOG_SHOW_SEQ and F_TAB reach the model");
            Check(host.Send(Json.Tab(CoreApi.CORE_TAB_MANAGE)) == CoreApi.CORE_OK, "{\"ev\":\"tab\"} for the tab shown is accepted");

            // a dialog flow: the flow thread blocks in host->dialog, the form read, the answer filled
            Check(Core.core_test_flow(host.C, 0 /* CORE_TEST_FLOW_DIALOG */) == CoreApi.CORE_OK, "a test flow with a dialog starts");
            bool answered = ui.Pump(() => ui.Logged("test flow: button 1"));
            DialogForm form;
            lock (ui.Dialogs) form = ui.Dialogs.Count == 1 ? ui.Dialogs[0] : null;
            Check(answered && form != null, "host->dialog on the flow thread, answered with button 1 (the core's log: \"test flow: button 1\")");
            Check(form != null && form.Title.Length > 0 && form.Buttons.Count == 2 && form.Default >= 0 && form.Default < 2,
                  "the dialog model read: a title, two buttons, the default (" + (form == null ? "none" : form.Title + " / " + string.Join(" / ", form.Buttons)) + ")");

            // the aggregator's events are the core's
            Check(host.Send(Activation.Activated) == CoreApi.CORE_OK && host.Send(Activation.Deactivated) == CoreApi.CORE_OK
                  && host.Send(Activation.CheckRequest) == CoreApi.CORE_OK, "\"activated\", \"deactivated\" and \"check_request\" are accepted");

            // an event the core refuses is logged by the shell
            Check(host.Send("{\"ev\":\"no such event\"}") != CoreApi.CORE_OK && ui.Logged("core: event refused"), "a refused event is logged");
            Check(ui.OffMain == 0, "every render on the core's main thread");

            ReloadAfterThrow(host.Held);

            // (a) shutdown under a dialog: the flow is in host->dialog when Stop runs; once it is
            // answered, its revalidation (a main job) is dropped, not stranded: the core logs
            // "revalidate 0" at once
            ui.Gate = new ManualResetEventSlim(false);
            ui.Entered = new ManualResetEventSlim(false);
            Check(Core.core_test_flow(host.C, 0 /* CORE_TEST_FLOW_DIALOG */) == CoreApi.CORE_OK, "a second test flow with a dialog starts");
            bool entered = ui.Pump(() => ui.Entered.IsSet);
            host.Stop();
            ui.Gate.Set();
            var sw = Stopwatch.StartNew();
            bool dropped = false;
            while (!dropped && sw.ElapsedMilliseconds < 2000) { dropped = ui.Logged("revalidate 0"); if (!dropped) Thread.Sleep(10); }
            Check(entered && dropped, "Stop under a dialog: the flow's revalidation is dropped, the core logs \"revalidate 0\" (" + sw.ElapsedMilliseconds + " ms)");
            lock (ui.Lines) foreach (var l in ui.Lines) if (l.StartsWith("shell: exception")) Check(false, l);
        }
    }
}
